#include "rt4k.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "hardware/irq.h"
#include "tusb.h"

#include "rtl1.h"
#include "rtl1_core.h"
#include "console.h"
#include "power.h"

#define RT4K_TASK_STACK     1024
#define RT4K_TASK_PRIORITY  (tskIDLE_PRIORITY + 6) // above the Wi-Fi stack (4): Wi-Fi bursts delaying
                                                   // USB reads overflowed the FT232R (bad RTL1 CRCs)
#define RT4K_TASK_CORE      1          // TinyUSB's host IRQ is registered on the core that initializes it
#define TX_QUEUE_SIZE       2048
#define RX_RING_SIZE        8192u      // power of two
#define CMD_MAX             240

static StreamBufferHandle_t tx_queue;
static SemaphoreHandle_t tx_lock;      // stream buffers allow one writer at a time
static SemaphoreHandle_t link_lock;    // one conversation with the RT4K at a time (rtl1 transfers)

static char rx_ring[RX_RING_SIZE];
static uint32_t rx_head;
static critical_section_t rx_lock;

static volatile rt4k_status_t status;
static volatile uint8_t cdc_idx = 0xff;
static TaskHandle_t rt4k_handle;
static volatile bool want_suspend, suspended;
static struct { uint32_t sent, dropped, last_wait_ms, max_wait_ms; } cmd_stats; // debug
static volatile uint32_t last_cmd_ms;
static volatile uint32_t last_event_us; // debug: the last host event (see tuh_cdc_rx_cb)
static struct { uint32_t packets, overruns, errors, last_overrun_ms; } ftdi_stats; // debug

// RTS/CTS flow control on the FT232R (TinyUSB turns it off at mount; we switch it back on with a
// vendor request at every mount). With it on, the FT232R only sends to the RT4K while the RT4K
// asserts CTS, so uploads can stream without per-frame ACKs. Always on: the RT4K keeps CTS asserted
// in standby too (measured: "pwr on" wakes it with flow control on; CTS only blinks off for a packet
// or two while it switches).
static volatile uint8_t modem_status_last; // FTDI modem status byte: bit 4 CTS, bit 5 DSR
static volatile uint32_t cts_off_packets;  // debug: packets whose status showed CTS off
static volatile bool flow_wanted = true, flow_on;
static volatile int8_t flow_request = -1;  // -1 none, else 0/1: the rt4k task sends it
static volatile uint8_t cdc_daddr;

enum { TR_IRQ = 1, TR_EVENT, TR_XFER, TR_OVERRUN }; // debug trace (see trace_add)
static void trace_add(uint16_t kind, uint16_t val);

// Every host event (most often from the USB IRQ) wakes the rt4k task right away. Polling tuh_task()
// once per tick moved one 64-byte packet per ms (~62 KB/s), below 2 Mbaud (200 KB/s), and the
// FT232R's 256-byte buffer overflowed during RTL1 transfers.
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)eventid;
    (void)in_isr; // ask the CPU instead: calling the wrong FreeRTOS variant corrupts the kernel
    last_event_us = time_us_32();
    trace_add(TR_EVENT, (uint16_t)eventid);
    if (!rt4k_handle) return;
    if (__get_current_exception()) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(rt4k_handle, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xTaskNotifyGive(rt4k_handle);
    }
}

// --- TinyUSB callbacks (run inside tuh_task(), i.e. in the rt4k task) --------------------------

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    printf("rt4k: USB device %04x:%04x attached\n", vid, pid);
}

void tuh_umount_cb(uint8_t daddr) {
    printf("rt4k: USB device %u removed\n", daddr);
}

void tuh_cdc_mount_cb(uint8_t idx) {
    tuh_itf_info_t info;
    uint16_t vid = 0, pid = 0;
    if (tuh_cdc_itf_get_info(idx, &info)) tuh_vid_pid_get(info.daddr, &vid, &pid);
    cdc_line_coding_t lc = {0};
    tuh_cdc_get_local_line_coding(idx, &lc);
    status.vid = vid;
    status.pid = pid;
    status.baud = lc.bit_rate;
    status.mounted = true;
    cdc_daddr = info.daddr;
    cdc_idx = idx;
    flow_on = false; // TinyUSB's setup turned it off
    if (flow_wanted) flow_request = 1;
    printf("rt4k: serial %04x:%04x mounted at %lu baud\n", vid, pid, (unsigned long)lc.bit_rate);
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (idx == cdc_idx) {
        cdc_idx = 0xff;
        status.mounted = false;
        flow_on = false;
        printf("rt4k: serial unmounted\n");
    }
}

static void flow_ctrl_done(tuh_xfer_t *xfer) {
    const bool on = xfer->user_data != 0;
    if (xfer->result == XFER_RESULT_SUCCESS) flow_on = on;
    printf("rt4k: RTS/CTS flow control %s%s\n", on ? "on" : "off", xfer->result == XFER_RESULT_SUCCESS ? "" : " FAILED");
}

// In the rt4k task (TinyUSB isn't thread safe). FTDI SET_FLOW_CTRL: wIndex high byte RTS_CTS_HS,
// low byte the channel (0 for the FT232R, as TinyUSB uses).
static void flow_ctrl_send(void) {
    static const tusb_control_request_t on_req = {
        .bmRequestType = 0x40, .bRequest = 2, .wValue = 0, .wIndex = 0x0100, .wLength = 0};
    static const tusb_control_request_t off_req = {
        .bmRequestType = 0x40, .bRequest = 2, .wValue = 0, .wIndex = 0x0000, .wLength = 0};
    const int8_t want = flow_request;
    if (want < 0 || cdc_idx == 0xff) return;
    tuh_xfer_t xfer = {
        .daddr = cdc_daddr, .ep_addr = 0, .setup = want ? &on_req : &off_req, .buffer = NULL,
        .complete_cb = flow_ctrl_done, .user_data = (uintptr_t)want};
    if (tuh_control_xfer(&xfer)) flow_request = -1; // else busy: again next time round
}

// Line speed change at the FT232R, sent from the rt4k task like the flow control request.
static volatile uint32_t baud_request; // 0: none
static volatile bool baud_done;

static void baud_set_done(tuh_xfer_t *xfer) {
    if (xfer->result == XFER_RESULT_SUCCESS) status.baud = (uint32_t)xfer->user_data;
    baud_done = true;
    printf("rt4k: FT232R at %lu baud%s\n", (unsigned long)xfer->user_data,
        xfer->result == XFER_RESULT_SUCCESS ? "" : " FAILED");
}

static void baud_send(void) {
    const uint32_t want = baud_request;
    if (!want || cdc_idx == 0xff) return;
    if (tuh_cdc_set_baudrate(cdc_idx, want, baud_set_done, want)) baud_request = 0; // else busy: next time
}

bool rt4k_set_baud(uint32_t baud) {
    baud_done = false;
    baud_request = baud;
    if (rt4k_handle) xTaskNotifyGive(rt4k_handle);
    for (int i = 0; i < 100 && !baud_done; i++) vTaskDelay(pdMS_TO_TICKS(5));
    return baud_done && status.baud == baud;
}

void rt4k_set_flow_control(bool on) {
    flow_wanted = on;
    flow_request = on;
    if (rt4k_handle) xTaskNotifyGive(rt4k_handle);
}

bool rt4k_flow_control(void) {
    return flow_on;
}

uint8_t rt4k_modem_status(void) {
    return modem_status_last;
}

// Debug: how long the bulk IN endpoint sits unpolled after a transfer completes (the completion IRQ
// until tuh_task() re-arms it, right after tuh_cdc_rx_cb()).
static uint32_t last_rearm_lat_us;
static struct { uint32_t max_us, slow, overrun_us, max_overrun_us; } gap_stats;

// Debug: a timeline of USB events during transfers, frozen shortly after an overrun shows up
// (GET /debug/usbtrace). Written on core 1 only (USB IRQ and the rt4k task).
#define TRACE_SIZE 512u // power of two
static struct { uint32_t us; uint16_t kind, val; } trace[TRACE_SIZE];
static volatile uint32_t trace_head, trace_stop_at; // trace_stop_at: 0 while recording
static uint32_t xfer_packets;

static void __not_in_flash_func(trace_add)(uint16_t kind, uint16_t val) {
    if (rtl1_core_phase() == RTL1_PH_IDLE) return;
    const uint32_t irq = save_and_disable_interrupts();
    if (!trace_stop_at || trace_head < trace_stop_at) {
        const uint32_t i = trace_head++ & (TRACE_SIZE - 1);
        trace[i].us = time_us_32();
        trace[i].kind = kind;
        trace[i].val = val;
    }
    restore_interrupts(irq);
}

// Debug: every USB interrupt goes into the trace, with the tasks running on each core.
static void __not_in_flash_func(usb_irq_probe)(void) {
    // Value: first letters of the tasks running on core 0 and core 1.
    const char *c0 = pcTaskGetName(xTaskGetCurrentTaskHandleForCore(0));
    const char *c1 = pcTaskGetName(xTaskGetCurrentTaskHandleForCore(1));
    trace_add(TR_IRQ, (uint16_t)((uint8_t)c0[0] << 8 | (uint8_t)c1[0]));
}

// Status bytes of every FTDI packet (patches/tinyusb/0002). An overrun means the FT232R's receive
// buffer filled up: the host didn't collect packets fast enough and serial bytes were lost.
void tuh_cdc_ftdi_status_cb(uint8_t idx, uint8_t modem_status, uint8_t line_status) {
    (void)idx;
    modem_status_last = modem_status;
    if (!(modem_status & 0x10)) cts_off_packets++; // the RT4K asked us to wait (with flow control on)
    ftdi_stats.packets++;
    xfer_packets++;
    if (line_status & 0x02) {
        trace_add(TR_OVERRUN, (uint16_t)xfer_packets);
        if (!trace_stop_at) trace_stop_at = trace_head + 64;
        ftdi_stats.overruns++;
        ftdi_stats.last_overrun_ms = to_ms_since_boot(get_absolute_time());
        gap_stats.overrun_us = last_rearm_lat_us; // the re-arm before the data that shows the loss
        if (last_rearm_lat_us > gap_stats.max_overrun_us) gap_stats.max_overrun_us = last_rearm_lat_us;
    }
    if (line_status & 0x1c) ftdi_stats.errors++; // parity, framing, break
    if (line_status & 0x10) power_break();       // break: the RT4K's output went low (powered down)
}

void tuh_cdc_rx_cb(uint8_t idx) {
    (void)idx;
    trace_add(TR_XFER, (uint16_t)xfer_packets);
    xfer_packets = 0;
    last_rearm_lat_us = time_us_32() - last_event_us;
    if (last_rearm_lat_us > gap_stats.max_us) gap_stats.max_us = last_rearm_lat_us;
    if (last_rearm_lat_us > 1000) gap_stats.slow++;
}

// --- task --------------------------------------------------------------------------------------

void rt4k_text_push(const uint8_t *data, size_t len) {
    console_feed(data, len); // routes each line: see console.h
}

void rt4k_term_push(const uint8_t *data, size_t len) {
    critical_section_enter_blocking(&rx_lock);
    for (size_t i = 0; i < len; i++) rx_ring[(rx_head + i) & (RX_RING_SIZE - 1)] = (char)data[i];
    rx_head += (uint32_t)len;
    critical_section_exit(&rx_lock);
}

static void host_init(void) {
    static const tusb_rhport_init_t rh = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUH_RHPORT, &rh);
    irq_add_shared_handler(USBCTRL_IRQ, usb_irq_probe, PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY);
}

static void rt4k_task(void *param) {
    (void)param;
    host_init();
    printf("rt4k: USB host started\n");
    uint8_t buf[64];
    for (;;) {
        if (want_suspend) {
            // Host quiet while flash is written (see flash_ops.h): its IRQ off (this task and the IRQ
            // are on core 1) and this task parked. Not tuh_deinit(): TinyUSB 0.21's hcd_deinit() calls
            // critical_section_deinit(), which force-unlocks a striped spinlock shared with other
            // critical sections (the log's...); an OTA then hung on it about 3 times in 4.
            irq_set_enabled(USBCTRL_IRQ, false);
            suspended = true;
            while (want_suspend) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            irq_set_enabled(USBCTRL_IRQ, true);
            suspended = false;
            printf("rt4k: USB host resumed\n");
            continue;
        }
        tuh_task();
        flow_ctrl_send();
        baud_send();
        const uint8_t idx = cdc_idx;
        if (idx != 0xff && tuh_cdc_mounted(idx)) {
            uint32_t n;
            while ((n = tuh_cdc_read_available(idx)) > 0) {
                n = tuh_cdc_read(idx, buf, n < sizeof(buf) ? n : sizeof(buf));
                if (!n) break;
                status.rx_bytes += n;
                rtl1_feed(buf, n);
            }
            bool wrote = false;
            for (;;) {
                uint32_t room = tuh_cdc_write_available(idx);
                if (!room) break;
                if (room > sizeof(buf)) room = sizeof(buf);
                const size_t got = xStreamBufferReceive(tx_queue, buf, room, 0);
                if (!got) break;
                tuh_cdc_write(idx, buf, got);
                status.tx_bytes += got;
                wrote = true;
            }
            if (wrote) tuh_cdc_write_flush(idx);
        } else {
            // No RT4K: drop queued bytes so stale commands don't fire on the next connection.
            const size_t got = xStreamBufferReceive(tx_queue, buf, sizeof(buf), 0);
            status.tx_dropped += got;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)); // USB event or queued command; 10 ms at most
    }
}

void rt4k_start(void) {
    tx_queue = xStreamBufferCreate(TX_QUEUE_SIZE, 1);
    tx_lock = xSemaphoreCreateMutex();
    link_lock = xSemaphoreCreateMutex();
    critical_section_init(&rx_lock);
    xTaskCreateAffinitySet(rt4k_task, "rt4k", RT4K_TASK_STACK, NULL, RT4K_TASK_PRIORITY, 1u << RT4K_TASK_CORE, &rt4k_handle);
}

static bool link_held_for_suspend;

bool rt4k_suspend(void) {
    if (!rt4k_handle) return true; // not started: nothing running
    // Only between transfers: tearing TinyUSB down while the FT232R is streaming an RTL1 transfer left
    // core 1 stuck in a TinyUSB critical section, and core 0 then hung on the shared spinlock (the OTA
    // froze right after "serial unmounted"). Every transfer holds the link, so owning it means quiet;
    // keep it until the resume so no transfer starts with the host off.
    if (!rt4k_link_lock(5000)) return false;
    link_held_for_suspend = true;
    want_suspend = true;
    for (int i = 0; i < 100 && !suspended; i++) vTaskDelay(pdMS_TO_TICKS(10));
    return suspended;
}

void rt4k_resume(void) {
    want_suspend = false;
    if (rt4k_handle) xTaskNotifyGive(rt4k_handle);
    if (link_held_for_suspend) {
        link_held_for_suspend = false;
        rt4k_link_unlock();
    }
}

bool rt4k_write(const void *data, size_t len) {
    if (xSemaphoreTake(tx_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const bool ok = xStreamBufferSpacesAvailable(tx_queue) >= len && xStreamBufferSend(tx_queue, data, len, 0) == len;
    xSemaphoreGive(tx_lock);
    if (ok && rt4k_handle) xTaskNotifyGive(rt4k_handle);
    return ok;
}

bool rt4k_link_lock(uint32_t timeout_ms) {
    return xSemaphoreTake(link_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void rt4k_link_unlock(void) {
    xSemaphoreGive(link_lock);
}

bool rt4k_connected(void) {
    return status.mounted;
}

bool rt4k_command(const char *cmd) {
    return console_send(CON_PAGE, cmd);
}

bool rt4k_send_command(const char *cmd) {
    char line[CMD_MAX + 4];
    const int n = snprintf(line, sizeof(line), "\r%s\r\n", cmd);
    if (n < 0 || n >= (int)sizeof(line)) return false;
    const uint32_t t0 = to_ms_since_boot(get_absolute_time());
    const bool locked = rt4k_link_lock(2000); // waits for a running rtl1 transfer
    const uint32_t waited = to_ms_since_boot(get_absolute_time()) - t0;
    cmd_stats.last_wait_ms = waited;
    if (waited > cmd_stats.max_wait_ms) cmd_stats.max_wait_ms = waited;
    if (!locked) {
        cmd_stats.dropped++;
        return false;
    }
    const bool ok = rt4k_write(line, (size_t)n);
    rt4k_link_unlock();
    cmd_stats.sent++;
    last_cmd_ms = to_ms_since_boot(get_absolute_time());
    if (ok && !strcmp(cmd, "pwr on")) power_woken();
    return ok;
}

bool rt4k_query(const char *cmd, const char *expect, char *out, size_t size, uint32_t timeout_ms) {
    return console_query(cmd, expect, out, size, timeout_ms);
}

uint32_t rt4k_rx_head(void) {
    critical_section_enter_blocking(&rx_lock);
    const uint32_t head = rx_head;
    critical_section_exit(&rx_lock);
    return head;
}

uint32_t rt4k_ms_since_command(void) {
    return to_ms_since_boot(get_absolute_time()) - last_cmd_ms;
}

void rt4k_debug(char *out, size_t size) {
    snprintf(out, size, "commands: sent %lu dropped %lu, link wait last %lu ms max %lu ms\n"
        "ftdi: %lu packets, %lu overruns (last at %lu ms), %lu line errors\n"
        "rx re-arm latency: max %lu us, %lu over 1 ms; before an overrun: last %lu us max %lu us\n",
        (unsigned long)cmd_stats.sent, (unsigned long)cmd_stats.dropped, (unsigned long)cmd_stats.last_wait_ms,
        (unsigned long)cmd_stats.max_wait_ms, (unsigned long)ftdi_stats.packets, (unsigned long)ftdi_stats.overruns,
        (unsigned long)ftdi_stats.last_overrun_ms, (unsigned long)ftdi_stats.errors, (unsigned long)gap_stats.max_us,
        (unsigned long)gap_stats.slow, (unsigned long)gap_stats.overrun_us,
        (unsigned long)gap_stats.max_overrun_us);
    const size_t n = strlen(out);
    const uint8_t ms = modem_status_last;
    snprintf(out + n, size - n, "ftdi modem status 0x%02x (CTS %s, DSR %s, CTS off in %lu packets), RTS/CTS flow control %s\n",
        ms, ms & 0x10 ? "on" : "off", ms & 0x20 ? "on" : "off", (unsigned long)cts_off_packets, flow_on ? "on" : "off");
}

size_t rt4k_debug_json(char *out, size_t size) {
    const uint8_t ms = modem_status_last; // FTDI: bit 4 CTS, 5 DSR, 6 RI, 7 DCD
    const int n = snprintf(out, size,
        "{\"usb\":%s,\"vid\":%u,\"pid\":%u,\"baud\":%lu,\"rx\":%lu,\"tx\":%lu,\"tx_dropped\":%lu,"
        "\"packets\":%lu,\"overruns\":%lu,\"line_errors\":%lu,\"flow\":%s,"
        "\"cts\":%s,\"dsr\":%s,\"ri\":%s,\"dcd\":%s,\"cts_off_packets\":%lu,"
        "\"commands\":%lu,\"commands_dropped\":%lu,\"link_wait_max_ms\":%lu,\"rearm_max_us\":%lu}",
        status.mounted ? "true" : "false", status.vid, status.pid, (unsigned long)status.baud,
        (unsigned long)status.rx_bytes, (unsigned long)status.tx_bytes, (unsigned long)status.tx_dropped,
        (unsigned long)ftdi_stats.packets, (unsigned long)ftdi_stats.overruns, (unsigned long)ftdi_stats.errors,
        flow_on ? "true" : "false", ms & 0x10 ? "true" : "false", ms & 0x20 ? "true" : "false",
        ms & 0x40 ? "true" : "false", ms & 0x80 ? "true" : "false", (unsigned long)cts_off_packets,
        (unsigned long)cmd_stats.sent, (unsigned long)cmd_stats.dropped, (unsigned long)cmd_stats.max_wait_ms,
        (unsigned long)gap_stats.max_us);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

size_t rt4k_rx_read(uint32_t *pos, char *out, size_t max) {
    critical_section_enter_blocking(&rx_lock);
    uint32_t from = *pos;
    // Overwritten, or ahead of us: restart at the oldest byte held.
    if (from > rx_head || rx_head - from > RX_RING_SIZE) from = rx_head > RX_RING_SIZE ? rx_head - RX_RING_SIZE : 0;
    size_t n = rx_head - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = rx_ring[(from + i) & (RX_RING_SIZE - 1)];
    *pos = from + (uint32_t)n;
    critical_section_exit(&rx_lock);
    return n;
}

void rt4k_get_status(rt4k_status_t *out) {
    memcpy(out, (const void *)&status, sizeof(*out));
}

size_t rt4k_trace_dump(char *out, size_t size) {
    static const char *const kinds[] = {"?", "irq", "event", "xfer", "OVERRUN"};
    const uint32_t head = trace_head;
    const uint32_t count = head < TRACE_SIZE ? head : TRACE_SIZE;
    size_t o = (size_t)snprintf(out, size, "%s, %lu entries (us since the first; xfer/OVERRUN value: packets)\n",
        trace_stop_at ? "frozen after an overrun" : "recording", (unsigned long)count);
    const uint32_t t0 = count ? trace[(head - count) & (TRACE_SIZE - 1)].us : 0;
    for (uint32_t k = head - count; k != head && o + 40 < size; k++) {
        const uint32_t i = k & (TRACE_SIZE - 1);
        const unsigned kind = trace[i].kind < 5 ? trace[i].kind : 0u;
        if (kind == TR_IRQ) // tasks running on core 0 / core 1 (first letters)
            o += (size_t)snprintf(out + o, size - o, "%8lu irq %c/%c\n", (unsigned long)(trace[i].us - t0),
                trace[i].val >> 8, trace[i].val & 0xff);
        else
            o += (size_t)snprintf(out + o, size - o, "%8lu %s %u\n", (unsigned long)(trace[i].us - t0), kinds[kind],
                trace[i].val);
    }
    trace_head = 0; // start over
    trace_stop_at = 0;
    return o;
}
