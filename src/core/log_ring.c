// log.h's ring, the same on every board; each board's log_init() sends stdout to it (log_ring_write()).

#include "log.h"

#include <string.h>

#include "platform.h"

#define LOG_SIZE  8192u        // power of two
#define LOG_MAGIC 0x43524c47u  // "CRLG"

// Not zeroed at startup: kept across watchdog/software resets (see log.h).
typedef struct {
    uint32_t magic;
    uint32_t head;             // total bytes ever written
    uint32_t check;            // magic ^ head, to tell a kept ring from random power-on RAM
    char ring[LOG_SIZE];
} log_state_t;
static log_state_t PLAT_NOINIT(st);

static plat_lock_t lock;

static inline void put(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) st.ring[(st.head + i) & (LOG_SIZE - 1)] = buf[i];
    st.head += (uint32_t)len;
    st.check = LOG_MAGIC ^ st.head;
}

void log_ring_init(bool soft_reset) {
    plat_lock_init(&lock);
    if (st.magic == LOG_MAGIC && st.check == (LOG_MAGIC ^ st.head)) {
        const char *why = soft_reset ? "\n===== reset by watchdog/software, log kept =====\n" : "\n===== reset, log kept =====\n";
        put(why, strlen(why));
    } else {
        st.magic = LOG_MAGIC;
        st.head = 0;
        st.check = LOG_MAGIC;
    }
}

void log_ring_write(const char *buf, size_t len) {
    plat_lock_enter(&lock);
    put(buf, len);
    plat_lock_exit(&lock);
}

// In RAM, and no strlen() (it's in flash): the Pico 2 W's HardFault handler logs with it when flash has
// failed. One loop copies and counts: a loop that only counted, GCC turns into a strlen() call.
void PLAT_RAMFUNC(log_write_raw)(const char *s) {
    uint32_t n = 0;
    for (; s[n]; n++) st.ring[(st.head + n) & (LOG_SIZE - 1)] = s[n];
    st.head += n;
    st.check = LOG_MAGIC ^ st.head;
}

size_t log_read(uint32_t *pos, char *out, size_t max) {
    plat_lock_enter(&lock);
    uint32_t from = *pos;
    // Overwritten, or ahead of us (a client from before a reset): restart at the oldest byte held.
    if (from > st.head || st.head - from > LOG_SIZE) from = st.head > LOG_SIZE ? st.head - LOG_SIZE : 0;
    size_t n = st.head - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = st.ring[(from + i) & (LOG_SIZE - 1)];
    *pos = from + (uint32_t)n;
    plat_lock_exit(&lock);
    return n;
}
