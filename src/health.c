#include "health.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"

#include "log.h"
#include "net.h"

#define WDT_TIMEOUT_MS    8000
#define WDT_FEED_MS       1000
#define WDT_TASK_PRIORITY (tskIDLE_PRIORITY + 1) // lowest real priority: starving it is a hang too

// The CYW43 can stop answering while everything else keeps running (seen during OTA flash writes):
// the board is then unreachable but not hung. A probe asks the chip for the RSSI; if it stops
// completing, or keeps failing while we should be connected, the watchdog is no longer fed.
#define PROBE_PERIOD_MS   5000
#define PROBE_STALE_MS    20000
#define PROBE_MAX_FAILS   4

static volatile uint32_t probe_done_ms; // 0 = probe not started yet
static volatile bool net_dead;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void wdt_task(void *param) {
    (void)param;
    for (;;) {
        const uint32_t done = probe_done_ms;
        if (net_dead || (done && now_ms() - done > PROBE_STALE_MS)) {
            printf("health: CYW43 not responding (%s), letting the watchdog reset\n", net_dead ? "probe failures" : "probe stuck");
            vTaskDelete(NULL);
        }
        watchdog_update();
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_MS));
    }
}

static void probe_task(void *param) {
    (void)param;
    int fails = 0;
    for (;;) {
        int32_t rssi = 0;
        cyw43_arch_lwip_begin();
        const int rc = cyw43_wifi_get_rssi(&cyw43_state, &rssi);
        cyw43_arch_lwip_end();
        probe_done_ms = now_ms();
        if (rc != 0 && net_state() == NET_CONNECTED) {
            printf("health: RSSI probe failed (%d)\n", rc);
            if (++fails >= PROBE_MAX_FAILS) net_dead = true;
        } else {
            fails = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
    }
}

void health_start(void) {
    // pause_on_debug: a debugger halting the cores doesn't reset the board.
    watchdog_enable(WDT_TIMEOUT_MS, true);
    xTaskCreate(wdt_task, "wdt", 256, NULL, WDT_TASK_PRIORITY, NULL);
    printf("health: watchdog %u ms\n", WDT_TIMEOUT_MS);
}

void health_start_net_probe(void) {
    xTaskCreate(probe_task, "probe", 512, NULL, WDT_TASK_PRIORITY, NULL);
}

// --- faults ------------------------------------------------------------------------------------
// Log where it happened and spin; the watchdog resets the board and the log survives the reset.

static void hex32(char *out, uint32_t v) {
    for (int i = 7; i >= 0; i--, v >>= 4) out[i] = "0123456789abcdef"[v & 15];
}

void __attribute__((used)) hardfault_report(const uint32_t *frame) {
    char line[] = "\n*** HardFault pc=00000000 lr=00000000\n";
    hex32(line + 17, frame[6]);
    hex32(line + 29, frame[5]);
    log_write_raw(line);
    for (;;) __asm volatile("nop");
}

// Picks the stack the exception frame was pushed to (MSP or PSP) and hands it to the C code.
void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4       \n"
        "ite eq           \n"
        "mrseq r0, msp    \n"
        "mrsne r0, psp    \n"
        "b hardfault_report\n");
}
