#include "rt4k.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "pico/sync.h"
#include "hardware/irq.h"
#include "tusb.h"

#define RT4K_TASK_STACK     1024
#define RT4K_TASK_PRIORITY  (tskIDLE_PRIORITY + 3)
#define RT4K_TASK_CORE      1          // TinyUSB's host IRQ is registered on the core that initializes it
#define TX_QUEUE_SIZE       2048
#define RX_RING_SIZE        8192u      // power of two
#define CMD_MAX             240

static StreamBufferHandle_t tx_queue;
static SemaphoreHandle_t tx_lock;      // stream buffers allow one writer at a time

static char rx_ring[RX_RING_SIZE];
static uint32_t rx_head;
static critical_section_t rx_lock;

static volatile rt4k_status_t status;
static volatile uint8_t cdc_idx = 0xff;
static TaskHandle_t rt4k_handle;
static volatile bool want_suspend, suspended;

// Every host event (most often from the USB IRQ) wakes the rt4k task right away. Polling tuh_task()
// once per tick moved one 64-byte packet per ms (~62 KB/s), below 2 Mbaud (200 KB/s), and the
// FT232R's 256-byte buffer overflowed during RTL1 transfers.
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)eventid;
    (void)in_isr; // ask the CPU instead: calling the wrong FreeRTOS variant corrupts the kernel
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
    cdc_idx = idx;
    printf("rt4k: serial %04x:%04x mounted at %lu baud\n", vid, pid, (unsigned long)lc.bit_rate);
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (idx == cdc_idx) {
        cdc_idx = 0xff;
        status.mounted = false;
        printf("rt4k: serial unmounted\n");
    }
}

// --- task --------------------------------------------------------------------------------------

static void rx_push(const uint8_t *data, uint32_t len) {
    critical_section_enter_blocking(&rx_lock);
    for (uint32_t i = 0; i < len; i++) rx_ring[(rx_head + i) & (RX_RING_SIZE - 1)] = (char)data[i];
    rx_head += len;
    critical_section_exit(&rx_lock);
    status.rx_bytes += len;
}

static void rt4k_task(void *param) {
    (void)param;
    tuh_init(BOARD_TUH_RHPORT);
    printf("rt4k: USB host started\n");
    uint8_t buf[64];
    for (;;) {
        if (want_suspend) {
            // Controller and IRQ off while flash is written (see flash_ops.h).
            tuh_deinit(BOARD_TUH_RHPORT);
            cdc_idx = 0xff;
            status.mounted = false;
            suspended = true;
            while (want_suspend) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            tuh_init(BOARD_TUH_RHPORT);
            suspended = false;
            printf("rt4k: USB host resumed\n");
            continue;
        }
        // TinyUSB's rp2040 host driver doesn't guard endpoint state between task and IRQ
        // (hw_endpoint_lock_update() is an empty "todo add critsec"), so an IRQ landing while a
        // transfer is being (re)armed sees it half set up. Task and IRQ share core 1: keeping the
        // IRQ off while we call into TinyUSB is the guard that TODO asks for.
        irq_set_enabled(USBCTRL_IRQ, false);
        tuh_task();
        const uint8_t idx = cdc_idx;
        if (idx != 0xff && tuh_cdc_mounted(idx)) {
            uint32_t n;
            while ((n = tuh_cdc_read_available(idx)) > 0) {
                n = tuh_cdc_read(idx, buf, n < sizeof(buf) ? n : sizeof(buf));
                if (!n) break;
                rx_push(buf, n);
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
        irq_set_enabled(USBCTRL_IRQ, true);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)); // USB event or queued command; 10 ms at most
    }
}

void rt4k_start(void) {
    tx_queue = xStreamBufferCreate(TX_QUEUE_SIZE, 1);
    tx_lock = xSemaphoreCreateMutex();
    critical_section_init(&rx_lock);
    xTaskCreateAffinitySet(rt4k_task, "rt4k", RT4K_TASK_STACK, NULL, RT4K_TASK_PRIORITY, 1u << RT4K_TASK_CORE, &rt4k_handle);
}

bool rt4k_suspend(void) {
    if (!rt4k_handle) return true; // not started: nothing running
    want_suspend = true;
    for (int i = 0; i < 100 && !suspended; i++) vTaskDelay(pdMS_TO_TICKS(10));
    return suspended;
}

void rt4k_resume(void) {
    want_suspend = false;
    if (rt4k_handle) xTaskNotifyGive(rt4k_handle);
}

bool rt4k_command(const char *cmd) {
    char line[CMD_MAX + 4];
    const int n = snprintf(line, sizeof(line), "\r%s\r\n", cmd);
    if (n < 0 || n >= (int)sizeof(line)) return false;
    if (xSemaphoreTake(tx_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const bool ok = xStreamBufferSpacesAvailable(tx_queue) >= (size_t)n &&
        xStreamBufferSend(tx_queue, line, (size_t)n, 0) == (size_t)n;
    xSemaphoreGive(tx_lock);
    if (ok && rt4k_handle) xTaskNotifyGive(rt4k_handle);
    return ok;
}

size_t rt4k_rx_read(uint32_t *pos, char *out, size_t max) {
    critical_section_enter_blocking(&rx_lock);
    uint32_t from = *pos;
    if (rx_head - from > RX_RING_SIZE) from = rx_head - RX_RING_SIZE;
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
