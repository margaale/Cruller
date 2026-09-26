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

// A console command from the page (web terminal, remote), queued in console.c. False when it doesn't
// fit or the queue is full.
bool rt4k_command(const char *cmd);

// Sends a console command and waits for the first reply line containing `expect` ("[COM] " prefix
// dropped), copied to out (console_query). False if no such line came in time.
bool rt4k_query(const char *cmd, const char *expect, char *out, size_t size, uint32_t timeout_ms);

// Writes a console command now, the way the RT4K expects it: "\r<cmd>\r\n" (console.c's task; others
// queue with console_send). False when it doesn't fit, or while an rtl1 transfer holds the link for
// more than 2 s.
bool rt4k_send_command(const char *cmd);

// The position after the last byte in the terminal ring (a starting point for rt4k_rx_read()).
uint32_t rt4k_rx_head(void);

// Raw bytes to the RT4K (rtl1 commands and frames). False if the queue is full.
bool rt4k_write(const void *data, size_t len);

// One conversation at a time: rtl1 holds this for a whole transfer; rt4k_command() takes it briefly.
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

// Milliseconds since rt4k_command() last sent something.
uint32_t rt4k_ms_since_command(void);

// Text from the RT4K; rtl1_feed() passes on everything that isn't a binary frame. It goes to
// console.c, which routes each line and writes the web terminal's share with rt4k_term_push().
void rt4k_text_push(const uint8_t *data, size_t len);
void rt4k_term_push(const uint8_t *data, size_t len);

// Copies terminal text received from the RT4K after *pos (see log_read()).
size_t rt4k_rx_read(uint32_t *pos, char *out, size_t max);

void rt4k_get_status(rt4k_status_t *out);

// Debug: command counters and link waits, one line.
void rt4k_debug(char *out, size_t size);

// Debug: the USB event timeline around the last FT232R overrun, as text; restarts the recording.
size_t rt4k_trace_dump(char *out, size_t size);
