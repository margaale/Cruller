// log.h on the Pico 2 W: stdout (pico_stdio) goes to the ring (log_ring.c).

#include "log.h"

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "hardware/watchdog.h"

static void log_out_chars(const char *buf, int len) {
    log_ring_write(buf, (size_t)len);
}

static stdio_driver_t log_driver = {
    .out_chars = log_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = true,
#endif
};

void log_init(void) {
    log_ring_init(watchdog_caused_reboot());
    stdio_set_driver_enabled(&log_driver, true);
}
