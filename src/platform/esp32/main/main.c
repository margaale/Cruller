// Cruller on the ESP32-S3: the same startup as rp2's main.c, on ESP-IDF. No RT4K link yet
// (src/core/rt4k_stub.c); the USB host comes next.

#include <stdio.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "cfgfs.h"
#include "console.h"
#include "gameid_run.h"
#include "freeze.h"
#include "health.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "ota.h"
#include "platform.h"
#include "power.h"
#include "rfc2217.h"
#include "rt4k.h"
#include "rtl1.h"
#include "status_led.h"
#include "ws.h"
#include "version.h"

void app_main(void) {
    log_init(); // stdout -> ring buffer read by the web UI (/log), and the console
    freeze_report();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) printf("nvs: init failed (%s)\n", esp_err_to_name(err));
    rtl1_init();
    printf("\nCruller %s, boot partition %d (%s boot)\n", cruller_version, ota_boot_partition(), ota_last_boot_type());

    health_start(ota_is_trial_boot());
    freeze_start();
    esp_netif_init();
    esp_event_loop_create_default();
    health_start_net_probe();
    status_led_start();
    if (!cfgfs_mount()) printf("cfgfs: no files this run\n"); // the files (gameID's)
    console_start(); // before rt4k_start(): the rt4k task feeds them text from the start
    power_start();
    rt4k_start();
    ws_start();
    http_start();
    gameid_run_start(); // asks the consoles once the network is up
    rfc2217_start();
    net_start();

    // Healthy enough to keep an updated image: the web server (which carries OTA) is listening.
    // Joining the network is not required, so a network outage never rejects a good image.
    while (!http_listening()) vTaskDelay(pdMS_TO_TICKS(50));
    ota_confirm_if_trial();
    health_rearm_watchdog();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        printf("alive %lu s, heap free %u (min %u)\n", (unsigned long)(plat_ms() / 1000),
            (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());
    }
}
