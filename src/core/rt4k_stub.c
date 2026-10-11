// No RT4K link: the targets without one yet (the ESP32).
// Everything in rt4k.h, doing nothing: commands aren't sent (so queries fail), the terminal stays empty.

#include "rt4k.h"

#include <stdio.h>
#include <string.h>

void rt4k_start(void) {
}

bool rt4k_write(const void *data, size_t len) {
    (void)data;
    (void)len;
    return false; // no RT4K: console.c skips the reply window
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

void rt4k_set_flow_control(bool on) {
    (void)on;
}

bool rt4k_flow_control(void) {
    return false;
}

uint8_t rt4k_modem_status(void) {
    return 0;
}

bool rt4k_set_baud(uint32_t baud) {
    (void)baud;
    return false;
}

void rt4k_get_status(rt4k_status_t *out) {
    memset(out, 0, sizeof(*out));
}

static const char NO_LINK[] = "no RT4K link on this board yet\n";

void rt4k_debug(char *out, size_t size) {
    snprintf(out, size, "%s", NO_LINK);
}

size_t rt4k_debug_json(char *out, size_t size) {
    const int n = snprintf(out, size, "{\"usb\":false}");
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

size_t rt4k_trace_dump(char *out, size_t size) {
    return (size_t)snprintf(out, size, "%s", NO_LINK);
}
