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

// Sends a console command the way the RT4K expects it: "\r<cmd>\r\n". False when it doesn't fit,
// or while an rtl1 transfer holds the link for more than 2 s.
bool rt4k_command(const char *cmd);

// Raw bytes to the RT4K (rtl1 commands and frames). False if the queue is full.
bool rt4k_write(const void *data, size_t len);

// One conversation at a time: rtl1 holds this for a whole transfer; rt4k_command() takes it briefly.
bool rt4k_link_lock(uint32_t timeout_ms);
void rt4k_link_unlock(void);

bool rt4k_connected(void);

// Milliseconds since rt4k_command() last sent something.
uint32_t rt4k_ms_since_command(void);

// Text for the terminal ring; rtl1_feed() passes on everything that isn't a binary frame.
void rt4k_text_push(const uint8_t *data, size_t len);

// Copies terminal text received from the RT4K after *pos (see log_read()).
size_t rt4k_rx_read(uint32_t *pos, char *out, size_t max);

void rt4k_get_status(rt4k_status_t *out);

// Debug: command counters and link waits, one line.
void rt4k_debug(char *out, size_t size);
