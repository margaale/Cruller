// status_led.h on the DevKitC-1's RGB LED (a WS2812): joining = blue fast blink, portal = amber slow
// blink, connected = green.

#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "platform.h"

#define LED_GPIO 48 // DevKitC-1 v1.0; v1.1 boards have it on GPIO 38

static volatile led_mode_t mode = LED_JOINING;

void status_led_set(led_mode_t m) { mode = m; }

static void led_task(void *param) {
    led_strip_handle_t strip = param;
    bool on = false, last = !on;
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        const led_mode_t m = mode;
        switch (m) {
            case LED_JOINING:   on = (now / pdMS_TO_TICKS(100)) % 2; break;
            case LED_PORTAL:    on = (now / pdMS_TO_TICKS(500)) % 2; break;
            case LED_CONNECTED: on = true; break;
        }
        if (on != last) {
            if (!on) led_strip_clear(strip);
            else if (m == LED_JOINING) led_strip_set_pixel(strip, 0, 0, 0, 24);
            else if (m == LED_PORTAL) led_strip_set_pixel(strip, 0, 24, 12, 0);
            else led_strip_set_pixel(strip, 0, 0, 16, 0);
            if (on) led_strip_refresh(strip);
            last = on;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void status_led_start(void) {
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip;
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &strip) != ESP_OK) return;
    led_strip_clear(strip);
    xTaskCreate(led_task, "led", PLAT_STACK(512), strip, tskIDLE_PRIORITY + 1, NULL);
}
