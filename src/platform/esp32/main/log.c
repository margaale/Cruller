// log.h on the ESP32-S3: stdout (printf and ESP_LOG) goes to the ring (log_ring.c) and on to the
// console as well. The ring survives software, watchdog and panic resets.

#include "log.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include <unistd.h>

#include "esp_system.h"
#include "esp_vfs.h"

static int console_fd = -1; // the USB/UART console, where stdout went before

// The "/dev/log" device stdout is reopened on.
static ssize_t log_vfs_write(int fd, const void *data, size_t size) {
    (void)fd;
    log_ring_write(data, size);
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
    log_ring_init(esp_reset_reason() != ESP_RST_POWERON);
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
