// Cruller: RetroTINK 4K controller firmware for the Raspberry Pi Pico 2 W.
// M0: Wi-Fi (with DonutShop credential import and a setup portal), web UI, A/B OTA.

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"

#include "http.h"
#include "net.h"
#include "ota.h"
#include "status_led.h"

#define MAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define MAIN_TASK_STACK    2048 // words

static uint32_t ms_since_boot(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void main_task(void *param) {
    (void)param;
    printf("\nCruller %s, boot partition %d (%s boot)\n", CRULLER_VERSION, ota_boot_partition(), ota_last_boot_type());

    const uint32_t t0 = ms_since_boot();
    if (cyw43_arch_init()) {
        // Without the CYW43 there is no network and no way to update: let a trial image roll back.
        printf("cyw43_arch_init failed\n");
        vTaskDelete(NULL);
    }
    printf("CYW43 up in %lu ms\n", (unsigned long)(ms_since_boot() - t0));
    status_led_start();
    http_start();
    net_start();

    // An updated image must be confirmed within ~16.7 s or the boot ROM reverts to the previous
    // one. Healthy enough = CYW43 up and the web server (which carries OTA) listening; joining the
    // network is not required, so a network outage never rejects a good image.
    while (!http_listening()) vTaskDelay(pdMS_TO_TICKS(50));
    ota_confirm_if_trial();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        printf("alive %lu s, heap free %u (min %u)\n", (unsigned long)(ms_since_boot() / 1000),
            (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());
    }
}

int main(void) {
    stdio_init_all();
    xTaskCreate(main_task, "main", MAIN_TASK_STACK, NULL, MAIN_TASK_PRIORITY, NULL);
    vTaskStartScheduler();
    return 0; // not reached
}
