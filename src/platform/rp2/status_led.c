#include "status_led.h"

#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile led_mode_t mode = LED_JOINING;

void status_led_set(led_mode_t m) { mode = m; }

static void led_task(void *param) {
    (void)param;
    bool on = false, last = !on;
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        switch (mode) {
            case LED_JOINING:   on = (now / pdMS_TO_TICKS(100)) % 2; break;
            case LED_PORTAL:    on = (now / pdMS_TO_TICKS(500)) % 2; break;
            case LED_CONNECTED: on = true; break;
        }
        // Each write is a CYW43 bus transaction: only write on change.
        if (on != last) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
            last = on;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void status_led_start(void) {
    xTaskCreate(led_task, "led", 512, NULL, tskIDLE_PRIORITY + 1, NULL);
}
