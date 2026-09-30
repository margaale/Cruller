#include "health.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/scb.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"

#include "log.h"
#include "net.h"

#define WDT_TIMEOUT_MS    8000
#define WDT_FEED_MS       1000
#define WDT_TASK_PRIORITY (tskIDLE_PRIORITY + 2) // with the busy tasks (http, ws): time slicing gives it
                                                 // a turn; at 1 an OTA upload starved it for ~4 s

// The network can die while everything else keeps running (seen with heavy USB host traffic: OTA
// flash writes, RTL1 bursts): the board is then unreachable but not hung, and asking the CYW43 for
// its RSSI still worked. What counts is traffic, so the gateway is pinged; once it has answered
// (some routers ignore pings), going NET_DEAD_MS without a reply stops feeding the watchdog.
// TODO: make these configurable (settings) once there is a settings store.
#define PING_PERIOD_MS    2000
#define NET_DEAD_MS       10000  // ~5 unanswered pings
#define PING_ID           0xC3u

static volatile uint32_t last_reply_ms; // 0 = the gateway has never answered
static struct raw_pcb *ping_pcb;
static SemaphoreHandle_t ping_gate; // held by the ping task while it uses lwIP
static uint16_t ping_seq;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static volatile uint32_t last_feed_ms;
volatile uint32_t health_feed_us; // the same, in timer microseconds, for the freeze recorder's ISR
static volatile bool rebooting; // stop feeding: a reboot is scheduled on the watchdog
static volatile bool armed;     // our watchdog is in charge (not during a TBYB trial)

// Runs in the timer task (highest priority): if the feeder is being starved, say who holds the cores
// before the watchdog resets the board (the log survives the reset).
static void starve_check(TimerHandle_t t) {
    (void)t;
    const uint32_t late = now_ms() - last_feed_ms;
    if (!armed || !last_feed_ms || late < 3000) return;
    char line[96];
    snprintf(line, sizeof(line), "\n*** watchdog not fed for %lu ms; core0=%s core1=%s\n", (unsigned long)late,
        pcTaskGetName(xTaskGetCurrentTaskHandleForCore(0)), pcTaskGetName(xTaskGetCurrentTaskHandleForCore(1)));
    log_write_raw(line);
}

static void wdt_task(void *param) {
    (void)param;
    for (;;) {
        const uint32_t last = last_reply_ms;
        if (last && net_state() != NET_PORTAL && now_ms() - last > NET_DEAD_MS) {
            printf("health: no reply from the gateway for %u s, letting the watchdog reset\n", NET_DEAD_MS / 1000);
            vTaskDelete(NULL);
        }
        // watchdog_update() reloads the counter with our 8 s: it would postpone a reboot that
        // rom_reboot()/watchdog_reboot() scheduled on the same watchdog, forever.
        if (armed && !rebooting) watchdog_update();
        last_feed_ms = now_ms();
        health_feed_us = timer_hw->timerawl;
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_MS));
    }
}

// lwIP (tcpip thread): take our echo replies, pass everything else on.
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg;
    (void)pcb;
    (void)addr;
    if (p->tot_len >= PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr) && pbuf_remove_header(p, PBUF_IP_HLEN) == 0) {
        const struct icmp_echo_hdr *echo = p->payload;
        if (ICMPH_TYPE(echo) == ICMP_ER && echo->id == PING_ID) {
            last_reply_ms = now_ms() | 1; // never 0
            pbuf_free(p);
            return 1;
        }
        pbuf_add_header(p, PBUF_IP_HLEN);
    }
    return 0;
}

static void ping_gateway(void) {
    if (!netif_default || !netif_is_link_up(netif_default) || ip4_addr_isany_val(*netif_ip4_gw(netif_default))) return;
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr), PBUF_RAM);
    if (!p) return;
    struct icmp_echo_hdr *echo = p->payload;
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->id = PING_ID;
    echo->seqno = lwip_htons(++ping_seq);
    echo->chksum = 0;
    echo->chksum = inet_chksum(echo, sizeof(*echo));
    ip_addr_t gw;
    ip_addr_copy_from_ip4(gw, *netif_ip4_gw(netif_default));
    raw_sendto(ping_pcb, p, &gw);
    pbuf_free(p);
}

static void ping_task(void *param) {
    (void)param;
    cyw43_arch_lwip_begin();
    ping_pcb = raw_new(IP_PROTO_ICMP);
    if (ping_pcb) {
        raw_recv(ping_pcb, ping_recv, NULL);
        raw_bind(ping_pcb, IP_ADDR_ANY);
    }
    cyw43_arch_lwip_end();
    if (!ping_pcb) {
        printf("health: no raw pcb, network check off\n");
        vTaskDelete(NULL);
    }
    for (;;) {
        xSemaphoreTake(ping_gate, portMAX_DELAY);
        cyw43_arch_lwip_begin();
        ping_gateway();
        cyw43_arch_lwip_end();
        xSemaphoreGive(ping_gate);
        vTaskDelay(pdMS_TO_TICKS(PING_PERIOD_MS));
    }
}

void health_stop_net_probe(void) {
    // Keep the gate: the ping task then never touches lwIP/CYW43 again, which matters once
    // cyw43_arch_deinit() has torn the async context down (it crashed the reboot into an update).
    if (ping_gate) xSemaphoreTake(ping_gate, pdMS_TO_TICKS(100));
    last_reply_ms = 0; // and don't count the missing replies against us
}

void health_start(bool trial) {
    xTimerStart(xTimerCreate("starve", pdMS_TO_TICKS(1000), pdTRUE, NULL, starve_check), 0);
    xTaskCreate(wdt_task, "wdt", 256, NULL, WDT_TASK_PRIORITY, NULL);
    if (trial) {
        // An image on trial is watched by the boot ROM (reset and roll back unless confirmed within
        // ~16.7 s). Arming and feeding our own watchdog now would defeat that: a new image hanging
        // early would never roll back. Ours takes over after the confirmation (health_rearm_watchdog).
        printf("health: trial boot, the boot ROM's watchdog is in charge until confirmed\n");
        return;
    }
    // pause_on_debug: a debugger halting the cores doesn't reset the board.
    watchdog_enable(WDT_TIMEOUT_MS, true);
    armed = true;
    printf("health: watchdog %u ms\n", WDT_TIMEOUT_MS);
}

uint32_t health_last_feed_ms(void) {
    return last_feed_ms;
}

void health_stop_feeding(void) {
    rebooting = true;
}

void health_rearm_watchdog(void) {
    watchdog_enable(WDT_TIMEOUT_MS, true);
    armed = true;
}

void health_start_net_probe(void) {
    ping_gate = xSemaphoreCreateMutex();
    xTaskCreate(ping_task, "ping", 512, NULL, WDT_TASK_PRIORITY, NULL);
}

// --- self-test ---------------------------------------------------------------------------------
// Freezes the network the way the real failures look (lwIP/CYW43 lock held, CPU fine) and lets go
// after `seconds`. With a working check the watchdog resets the board first.

static void wedge_task(void *param) {
    const uint32_t seconds = (uint32_t)(uintptr_t)param;
    printf("health: self-test, holding the network lock for %lu s\n", (unsigned long)seconds);
    cyw43_arch_lwip_begin();
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    cyw43_arch_lwip_end();
    printf("health: self-test released the network lock (watchdog did NOT fire)\n");
    vTaskDelete(NULL);
}

void health_wedge_network(uint32_t seconds) {
    xTaskCreate(wedge_task, "wedge", 256, (void *)(uintptr_t)seconds, tskIDLE_PRIORITY + 2, NULL);
}

// --- self-test of the fault reports ------------------------------------------------------------
// In RAM, since the "flash" kind stops flash answering under it.

static void __not_in_flash_func(fault_task)(void *param) {
    vTaskDelay(pdMS_TO_TICKS(300)); // the request's answer leaves first
    if (param) {
        // In the QMI's direct mode (a flash write's), every XIP access is a bus error: flash stops
        // answering the way it does when its chip browns out. This read faults first; the other core
        // faults on its next read from flash.
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
        (void)*(const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    } else {
        __asm volatile("udf #0"); // an undefined instruction: a UsageFault, escalated to a HardFault
    }
    for (;;) __asm volatile("nop");
}

bool health_fault_test(const char *kind) {
    const bool flash = !strcmp(kind, "flash");
    if (!flash && strcmp(kind, "task")) return false;
    xTaskCreate(fault_task, "fault", 256, flash ? (void *)1 : NULL, configMAX_PRIORITIES - 2, NULL);
    return true;
}

// --- faults ------------------------------------------------------------------------------------
// Log where it happened and spin; the watchdog resets the board and the log survives the reset.
// All of it runs from RAM and reads nothing in flash (not even a string literal): a handler in flash
// can't run when the fault came from flash failing, and the core locks up instead (both cores did,
// the freeze record showed; see freeze.c).

static void __not_in_flash_func(hex32)(char *out, uint32_t v) {
    for (int i = 7; i >= 0; i--, v >>= 4) out[i] = (char)((v & 15) < 10 ? '0' + (v & 15) : 'a' + (v & 15) - 10);
}

// One per core (they can fault at once), filled in place: each '=' is followed by a value, in order.
static char fault_line[2][160] = {
    [0 ... 1] = "\n*** HardFault core ? pc=........ lr=........ r0=........ r1=........ r2=........ r3=........ "
                "cfsr=........ hfsr=........ mmfar=........ bfar=........\n",
};

const uint32_t *volatile health_fault_frame[2];

void __attribute__((used)) __not_in_flash_func(hardfault_report)(const uint32_t *frame) {
    const uint core = get_core_num();
    health_fault_frame[core] = frame;
    // Fault status: CFSR (MMFSR|BFSR|UFSR), HFSR, and the faulting addresses if valid.
    const uint32_t values[] = {frame[6], frame[5], frame[0], frame[1], frame[2], frame[3], scb_hw->cfsr, scb_hw->hfsr,
        scb_hw->mmfar, scb_hw->bfar};
    char *line = fault_line[core];
    char *p = line;
    while (*p != '?') p++;
    *p = (char)('0' + core);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        while (*p != '=') p++;
        hex32(++p, values[i]);
    }
    log_write_raw(line);
    for (;;) __asm volatile("nop");
}

// Picks the stack the exception frame was pushed to (MSP or PSP) and hands it to the C code.
void __attribute__((naked, section(".time_critical.isr_hardfault"))) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4       \n"
        "ite eq           \n"
        "mrseq r0, msp    \n"
        "mrsne r0, psp    \n"
        "b hardfault_report\n");
}
