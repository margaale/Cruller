// In-memory log: stdout goes to a ring buffer that the web UI reads (/log). There is no serial
// console once the USB port is the RT4K's host.
//
// The ring lives in RAM that startup code doesn't clear, so it survives a watchdog or software
// reset: after a hang, the next boot still shows what led to it. A power cycle starts it over.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Each board's: sends stdout to the ring (log_ring_init(), then log_ring_write()).
void log_init(void);

// The ring (log_ring.c), for log_init(): carries on with the ring a reset kept (saying so, and whether
// the reset was soft: a watchdog's or a restart) or starts it over.
void log_ring_init(bool soft_reset);
void log_ring_write(const char *buf, size_t len);

// Copies log text written after *pos into out (up to max bytes) and advances *pos. Returns the
// number of bytes copied. If *pos is older than what the ring still holds, it skips ahead.
size_t log_read(uint32_t *pos, char *out, size_t max);

// Appends text without stdio or locks, for fault handlers.
void log_write_raw(const char *s);
