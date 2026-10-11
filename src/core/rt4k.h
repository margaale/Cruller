// RT4K link over the native USB port (USB host; the RT4K is an FTDI FT232R at 2 Mbaud).
// Only the rt4k task touches TinyUSB; other tasks queue bytes (rt4k_write). What the RT4K sends goes to
// rtl1_feed(): binary frames to RTL1, text to console.c.

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

// Raw bytes to the RT4K (rtl1 commands and frames, console.c's commands). False if the queue is full.
bool rt4k_write(const void *data, size_t len);

// One conversation at a time: rtl1 holds this for a whole transfer; console.c takes it for each command.
bool rt4k_link_lock(uint32_t timeout_ms);
void rt4k_link_unlock(void);

bool rt4k_connected(void);

// RTS/CTS hardware flow control on the FT232R: on by default, asked again at every mount (TinyUSB's
// setup turns it off). rt4k_flow_control() says whether the chip confirmed it.
void rt4k_set_flow_control(bool on);
bool rt4k_flow_control(void);
uint8_t rt4k_modem_status(void); // FTDI modem status byte: bit 4 CTS, bit 5 DSR

// Sets the FT232R's line speed (the RT4K must switch too: see its "baud" command; it takes 115200,
// 500000, 1000000 and 2000000). Waits up to 0.5 s for the chip to confirm; true once it's at `baud`.
bool rt4k_set_baud(uint32_t baud);

void rt4k_get_status(rt4k_status_t *out);

// Debug: command counters, link waits and the FT232R's counters, as text (GET /debug/tasks).
void rt4k_debug(char *out, size_t size);
// The same counters (and the modem lines) as a JSON object, for the page's Debug tab. Returns its
// length, 0 if it didn't fit.
size_t rt4k_debug_json(char *out, size_t size);

// Debug: the USB event timeline around the last FT232R overrun, as text; restarts the recording.
size_t rt4k_trace_dump(char *out, size_t size);
