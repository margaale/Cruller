// RT4K link over the native USB port (USB host; the RT4K is an FTDI FT232R at 2 Mbaud).
// Only the rt4k task touches TinyUSB; other tasks queue bytes and read the RX ring.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool mounted;
    uint16_t vid, pid;
    uint32_t baud;
    uint32_t tx_bytes, rx_bytes, tx_dropped;
} rt4k_status_t;

void rt4k_start(void);

// Turns the USB host (controller, IRQ and task) off and back on; see flash_quiet_begin().
// False if the rt4k task didn't stop within 1 s.
bool rt4k_suspend(void);
void rt4k_resume(void);

// Sends a console command the way the RT4K expects it: "\r<cmd>\r\n". False when it doesn't fit.
bool rt4k_command(const char *cmd);

// Copies bytes received from the RT4K after *pos (see log_read()).
size_t rt4k_rx_read(uint32_t *pos, char *out, size_t max);

void rt4k_get_status(rt4k_status_t *out);
