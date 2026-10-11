#include "status_led.h"

static volatile led_mode_t mode = LED_JOINING;

void status_led_set(led_mode_t m) { mode = m; }

led_mode_t status_led_mode(void) { return mode; }

bool status_led_lit(led_mode_t m, uint32_t now_ms) {
    switch (m) {
        case LED_JOINING: return (now_ms / 100) & 1;
        case LED_PORTAL: return (now_ms / 500) & 1;
        default: return true;
    }
}
