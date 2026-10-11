// Cruller on the ESP32-S3: the same startup as rp2's main.c, on ESP-IDF. No RT4K link yet
// (src/core/rt4k_stub.c); the USB host comes next.

#include <stdio.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "app.h"
#include "freeze.h"
#include "health.h"
#include "log.h"
#include "ota.h"
#include "rtl1.h"
#include "status_led.h"
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
    status_led_start();
    app_run();
}
