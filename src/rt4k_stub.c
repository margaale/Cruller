// Bench builds: the USB port is the serial console, so there is no RT4K link.

#include "rt4k.h"

#include <string.h>

void rt4k_start(void) {
}

bool rt4k_suspend(void) {
    return true;
}

void rt4k_resume(void) {
}

bool rt4k_command(const char *cmd) {
    (void)cmd;
    return false;
}

bool rt4k_write(const void *data, size_t len) {
    (void)data;
    (void)len;
    return false;
}

bool rt4k_link_lock(uint32_t timeout_ms) {
    (void)timeout_ms;
    return true;
}

void rt4k_link_unlock(void) {
}

bool rt4k_connected(void) {
    return false;
}

void rt4k_text_push(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
}

size_t rt4k_rx_read(uint32_t *pos, char *out, size_t max) {
    (void)pos;
    (void)out;
    (void)max;
    return 0;
}

void rt4k_get_status(rt4k_status_t *out) {
    memset(out, 0, sizeof(*out));
}
