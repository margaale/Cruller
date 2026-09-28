// log.h on the ESP32-S3: stdout (printf and ESP_LOG) goes to the ring and on to the console as well.
// The ring is in RAM that startup doesn't clear (__NOINIT_ATTR), so it survives software, watchdog
// and panic resets.

#include "log.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"

#define LOG_SIZE  8192u        // power of two
#define LOG_MAGIC 0x43524c47u  // "CRLG"

typedef struct {
    uint32_t magic;
    uint32_t head;             // total bytes ever written
    uint32_t check;            // magic ^ head, to tell a kept ring from random power-on RAM
    char ring[LOG_SIZE];
} log_state_t;
static __NOINIT_ATTR log_state_t st;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static int console_fd = -1; // the USB/UART console, where stdout went before

static inline void put(const char *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) st.ring[(st.head + i) & (LOG_SIZE - 1)] = buf[i];
    st.head += len;
    st.check = LOG_MAGIC ^ st.head;
}

// The "/dev/log" device stdout is reopened on.
static ssize_t log_vfs_write(int fd, const void *data, size_t size) {
    (void)fd;
    portENTER_CRITICAL(&lock);
    put(data, (uint32_t)size);
    portEXIT_CRITICAL(&lock);
    if (console_fd >= 0) write(console_fd, data, size);
    return (ssize_t)size;
}

static int log_vfs_open(const char *path, int flags, int mode) {
    (void)path;
    (void)flags;
    (void)mode;
    return 0;
}

static int log_vfs_close(int fd) {
    (void)fd;
    return 0;
}

static int log_vfs_fstat(int fd, struct stat *s) {
    (void)fd;
    memset(s, 0, sizeof(*s));
    s->st_mode = S_IFCHR;
    return 0;
}

void log_init(void) {
    if (st.magic == LOG_MAGIC && st.check == (LOG_MAGIC ^ st.head)) {
        const esp_reset_reason_t r = esp_reset_reason();
        const char *why = r == ESP_RST_POWERON ? "\n===== reset, log kept =====\n"
                                               : "\n===== reset by watchdog/software, log kept =====\n";
        put(why, (uint32_t)strlen(why));
    } else {
        st.magic = LOG_MAGIC;
        st.head = 0;
        st.check = LOG_MAGIC;
    }
    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .write = log_vfs_write,
        .open = log_vfs_open,
        .close = log_vfs_close,
        .fstat = log_vfs_fstat,
    };
    if (esp_vfs_register("/dev/log", &vfs, NULL) != ESP_OK) return;
    fflush(stdout);
    console_fd = open("/dev/console", O_WRONLY);
    if (freopen("/dev/log", "w", stdout)) setvbuf(stdout, NULL, _IOLBF, 256);
}

void log_write_raw(const char *s) {
    put(s, (uint32_t)strlen(s));
}

size_t log_read(uint32_t *pos, char *out, size_t max) {
    portENTER_CRITICAL(&lock);
    uint32_t from = *pos;
    // Overwritten, or ahead of us (a client from before a reset): restart at the oldest byte held.
    if (from > st.head || st.head - from > LOG_SIZE) from = st.head > LOG_SIZE ? st.head - LOG_SIZE : 0;
    size_t n = st.head - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = st.ring[(from + i) & (LOG_SIZE - 1)];
    *pos = from + (uint32_t)n;
    portEXIT_CRITICAL(&lock);
    return n;
}
