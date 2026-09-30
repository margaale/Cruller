#include "log.h"

#include <string.h>

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/sync.h"
#include "hardware/watchdog.h"

#define LOG_SIZE  8192u        // power of two
#define LOG_MAGIC 0x43524c47u  // "CRLG"

// Not zeroed at startup: kept across watchdog/software resets (see log.h).
typedef struct {
    uint32_t magic;
    uint32_t head;             // total bytes ever written
    uint32_t check;            // magic ^ head, to tell a kept ring from random power-on RAM
    char ring[LOG_SIZE];
} log_state_t;
static log_state_t __uninitialized_ram(st);

static critical_section_t lock;

static __force_inline void put(const char *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) st.ring[(st.head + i) & (LOG_SIZE - 1)] = buf[i];
    st.head += len;
    st.check = LOG_MAGIC ^ st.head;
}

static void log_out_chars(const char *buf, int len) {
    critical_section_enter_blocking(&lock);
    put(buf, (uint32_t)len);
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
    if (st.magic == LOG_MAGIC && st.check == (LOG_MAGIC ^ st.head)) {
        const char *why = watchdog_caused_reboot() ? "\n===== reset by watchdog/software, log kept =====\n"
                                                   : "\n===== reset, log kept =====\n";
        put(why, (uint32_t)strlen(why));
    } else {
        st.magic = LOG_MAGIC;
        st.head = 0;
        st.check = LOG_MAGIC;
    }
    stdio_set_driver_enabled(&log_driver, true);
}

// In RAM, and no strlen() (it's in flash): the HardFault handler logs with it when flash has failed.
// One loop copies and counts: a loop that only counted, GCC turns into a strlen() call.
void __not_in_flash_func(log_write_raw)(const char *s) {
    uint32_t n = 0;
    for (; s[n]; n++) st.ring[(st.head + n) & (LOG_SIZE - 1)] = s[n];
    st.head += n;
    st.check = LOG_MAGIC ^ st.head;
}

size_t log_read(uint32_t *pos, char *out, size_t max) {
    critical_section_enter_blocking(&lock);
    uint32_t from = *pos;
    // Overwritten, or ahead of us (a client from before a reset): restart at the oldest byte held.
    if (from > st.head || st.head - from > LOG_SIZE) from = st.head > LOG_SIZE ? st.head - LOG_SIZE : 0;
    size_t n = st.head - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = st.ring[(from + i) & (LOG_SIZE - 1)];
    *pos = from + (uint32_t)n;
    critical_section_exit(&lock);
    return n;
}
