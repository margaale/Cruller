// On-board status LED (on the CYW43): joining = fast blink, portal = slow blink, connected = on.

#pragma once

typedef enum {
    LED_JOINING,
    LED_PORTAL,
    LED_CONNECTED,
} led_mode_t;

void status_led_start(void);   // after cyw43_arch_init()
void status_led_set(led_mode_t mode);
