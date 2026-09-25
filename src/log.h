// In-memory log: stdout goes to a ring buffer that the web UI reads (/log). There is no serial
// console once the USB port is the RT4K's host.

#pragma once

#include <stddef.h>
#include <stdint.h>

void log_init(void);

// Copies log text written after *pos into out (up to max bytes) and advances *pos. Returns the
// number of bytes copied. If *pos is older than what the ring still holds, it skips ahead.
size_t log_read(uint32_t *pos, char *out, size_t max);
