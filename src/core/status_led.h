// The status LED: joining = fast blink, portal = slow blink, connected = on. net.c sets the mode; each
// board lights its LED as status_led_lit() says, looking at least every 100 ms: the Pico 2 W's sensors
// task (its LED is on the CYW43, see health_rp2.h), the ESP32's LED task (status_led_start()).

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_JOINING,
    LED_PORTAL,
    LED_CONNECTED,
} led_mode_t;

void status_led_set(led_mode_t mode);
led_mode_t status_led_mode(void);
bool status_led_lit(led_mode_t mode, uint32_t now_ms); // on, at that time?

// The ESP32's LED task, after its bring-up.
void status_led_start(void);
