#include "platform_reboot.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"

#include "health.h"

#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#endif

void platform_prepare_reboot(void) {
    health_stop_net_probe();
    // Hold the driver's lock for good instead of cyw43_arch_deinit(): tearing the driver down under
    // other tasks (network rejoin, LED, lwIP) crashed them on its freed context. Now they just
    // block. Then power the chip off so the next boot starts it from reset.
    cyw43_arch_lwip_begin();
    gpio_init(CYW43_DEFAULT_PIN_WL_REG_ON);
    gpio_set_dir(CYW43_DEFAULT_PIN_WL_REG_ON, GPIO_OUT);
    gpio_put(CYW43_DEFAULT_PIN_WL_REG_ON, 0);
    busy_wait_ms(150);
}

void platform_reboot(void) {
    health_stop_feeding();
    watchdog_reboot(0, 0, PLATFORM_REBOOT_DELAY_MS); // scheduled first: see ota_reboot_into_update()
    platform_prepare_reboot();
    for (;;) tight_loop_contents();
}
