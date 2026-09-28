// Cruller: RetroTINK 4K controller firmware for the Raspberry Pi Pico 2 W.
// M0: Wi-Fi (with DonutShop credential import and a setup portal), web UI, A/B OTA.
// M1: RT4K link over USB (host) and a web terminal.

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

#include "flash_ops.h"
#include "freeze.h"
#include "health.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "console.h"
#include "ota.h"
#include "power.h"
#include "rfc2217.h"
#include "rt4k.h"
#include "rtl1.h"
#include "status_led.h"
#include "ws.h"

#define MAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define MAIN_TASK_STACK    2048 // words

static uint32_t ms_since_boot(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void main_task(void *param) {
    (void)param;
    printf("\nCruller %s, boot partition %d (%s boot)\n", CRULLER_VERSION, ota_boot_partition(), ota_last_boot_type());

    health_start(ota_is_trial_boot());
    freeze_start();
    const uint32_t t0 = ms_since_boot();
    // The CYW43's async context pins its task, and registers the chip's IRQ, on the core that calls
    // cyw43_arch_init(). Do it from core 0: core 1 belongs to the RT4K's USB (task and IRQ), and Wi-Fi
    // work landing there made the FT232R overflow (bad RTL1 CRCs whenever the web page was busy).
    vTaskCoreAffinitySet(NULL, 1u << 0);
    taskYIELD();
    const int cyw43_err = cyw43_arch_init();
    vTaskCoreAffinitySet(NULL, tskNO_AFFINITY);
    if (cyw43_err) {
        // Without the CYW43 there is no network and no way to update: let a trial image roll back.
        printf("cyw43_arch_init failed, resetting\n");
        health_stop_feeding(); // or the feeder keeps postponing the reboot
        watchdog_reboot(0, 0, 100);
        for (;;) vTaskDelay(portMAX_DELAY);
    }
    printf("CYW43 up in %lu ms, on core %u\n", (unsigned long)(ms_since_boot() - t0),
        (unsigned)async_context_core_num(cyw43_arch_async_context()));
    health_start_net_probe();
    status_led_start();
    console_start(); // before rt4k_start(): the rt4k task feeds them text from the start
    power_start();
    rt4k_start();
    ws_start();
    http_start();
    rfc2217_start();
    net_start();

    // An updated image must be confirmed within ~16.7 s or the boot ROM reverts to the previous
    // one. Healthy enough = CYW43 up and the web server (which carries OTA) listening; joining the
    // network is not required, so a network outage never rejects a good image.
    while (!http_listening()) vTaskDelay(pdMS_TO_TICKS(50));
    ota_confirm_if_trial();
    health_rearm_watchdog(); // the buy above stops the watchdog

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        printf("alive %lu s, heap free %u (min %u)\n", (unsigned long)(ms_since_boot() / 1000),
            (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());
    }
}

int main(void) {
    stdio_init_all();
    log_init(); // stdout -> ring buffer read by the web UI (/log)
    freeze_report(); // where the cores were, if the last run ended in a watchdog reset
    flash_ops_init();
    rtl1_init();
    xTaskCreate(main_task, "main", MAIN_TASK_STACK, NULL, MAIN_TASK_PRIORITY, NULL);
    vTaskStartScheduler();
    return 0; // not reached
}
