#include "log.h"

#include <string.h>

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/sync.h"

#define LOG_SIZE 8192u // power of two

static char ring[LOG_SIZE];
static uint32_t head; // total bytes ever written
static critical_section_t lock;

static void log_out_chars(const char *buf, int len) {
    critical_section_enter_blocking(&lock);
    for (int i = 0; i < len; i++) ring[(head + (uint32_t)i) & (LOG_SIZE - 1)] = buf[i];
    head += (uint32_t)len;
    critical_section_exit(&lock);
}

static stdio_driver_t log_driver = {
    .out_chars = log_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = true,
#endif
};

void log_init(void) {
    critical_section_init(&lock);
    stdio_set_driver_enabled(&log_driver, true);
}

size_t log_read(uint32_t *pos, char *out, size_t max) {
    critical_section_enter_blocking(&lock);
    uint32_t from = *pos;
    if (head - from > LOG_SIZE) from = head - LOG_SIZE; // overwritten: skip ahead
    size_t n = head - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = ring[(from + i) & (LOG_SIZE - 1)];
    *pos = from + (uint32_t)n;
    critical_section_exit(&lock);
    return n;
}
