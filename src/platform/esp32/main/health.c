// health.h on the ESP32-S3: the task watchdog (8 s, then a reset; see sdkconfig.defaults) watches the
// wdt task, which stops feeding it once the network stops passing traffic (health_net.c). Unlike
// the Pico 2 W, an image on trial runs under the same watchdog: a reset before it is confirmed makes
// the bootloader roll back.

#include "health.h"

#include <stdio.h>
#include <string.h>

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform.h"

#define WDT_FEED_MS       1000
#define WDT_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

static volatile bool rebooting;

static void wdt_task(void *param) {
    (void)param;
    esp_task_wdt_add(NULL);
    for (;;) {
        if (health_net_dead()) {
            for (;;) vTaskDelay(portMAX_DELAY); // subscribed and never fed again
        }
        if (!rebooting) esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_MS));
    }
}

void health_start(bool trial) {
    xTaskCreate(wdt_task, "wdt", PLAT_STACK(768), NULL, WDT_TASK_PRIORITY, NULL);
    printf("health: task watchdog %d s%s\n", CONFIG_ESP_TASK_WDT_TIMEOUT_S, trial ? ", image on trial" : "");
}

void health_stop_feeding(void) {
    rebooting = true;
}

// Sensors: the DevKit has no supply divider; its chip temperature isn't wired up (yet).
bool health_sensors(health_sensors_t *out) {
    (void)out;
    return false;
}

size_t health_sensor_samples(uint32_t *seq, health_sample_t *out, size_t max) {
    (void)seq;
    (void)out;
    (void)max;
    return 0;
}

// Self-test of the fault reports: ESP-IDF's panic handler prints the fault and restarts.
static void fault_task(void *param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(300)); // the request's answer leaves first
    *(volatile uint32_t *)0 = 0;
    for (;;) vTaskDelay(portMAX_DELAY);
}

bool health_fault_test(const char *kind) {
    if (strcmp(kind, "task")) return false; // "flash": the Pico 2 W's QMI only
    xTaskCreate(fault_task, "fault", PLAT_STACK(512), NULL, tskIDLE_PRIORITY + 2, NULL);
    return true;
}
