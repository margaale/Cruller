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
    cyw43_arch_deinit();
    gpio_init(CYW43_DEFAULT_PIN_WL_REG_ON);
    gpio_set_dir(CYW43_DEFAULT_PIN_WL_REG_ON, GPIO_OUT);
    gpio_put(CYW43_DEFAULT_PIN_WL_REG_ON, 0);
    busy_wait_ms(150);
}

void platform_reboot(void) {
    platform_prepare_reboot();
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}
