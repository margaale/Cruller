// RTL1 protocol logic without an RTOS: text/frame demultiplexing, frame checks, timeouts.
// rtl1.c wraps it with locking and a blocking API; the host unit tests (tests/) drive it directly.
// See rtl1.h and docs/RTL1.md.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtl1.h"

#define RTL1_MAX_PAYLOAD        2048
#define RTL1_READY_TIMEOUT_MS   3000  // command sent -> ready line
#define RTL1_STALL_TIMEOUT_MS   3000  // no frame while binary
#define RTL1_TAIL_TIMEOUT_MS    1000  // after the last frame: waiting for "... done" (or draining)

typedef struct {
    bool (*write)(const void *data, size_t len);             // bytes to the RT4K (ABORT frames)
    void (*text)(const uint8_t *data, size_t len);           // terminal text
    bool (*sha256)(const uint8_t *data, size_t len, uint8_t out[32]);
    void (*finished)(void);                                  // the transfer has ended (any outcome)
} rtl1_hooks_t;

typedef enum {
    RTL1_PH_IDLE,    // no transfer: text only
    RTL1_PH_READY,   // command sent, waiting for "<name> ready ... nonce=0x..."
    RTL1_PH_BINARY,  // frames
    RTL1_PH_DONE,    // digest checked, waiting for the closing text line
    RTL1_PH_DRAIN,   // failed or aborted: swallow bytes until the closing text line
} rtl1_phase_t;

void rtl1_core_init(const rtl1_hooks_t *hooks);

// Starts waiting for the ready line of `cmd` (whose first word names it). The caller then sends the
// command; `out` and `info` must stay valid until the transfer ends. `quiet`: no text reaches the
// terminal while the transfer runs (ready, closing and refusal lines, and anything in between).
void rtl1_core_begin(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info, bool quiet, uint32_t now_ms);

void rtl1_core_feed(const uint8_t *data, size_t len, uint32_t now_ms);

// Timeouts. True once the transfer is over (the result is then final).
bool rtl1_core_poll(uint32_t now_ms);

// Forgets the transfer (caller gave up, or is done reading the result).
void rtl1_core_end(void);

rtl1_phase_t rtl1_core_phase(void);
rtl1_result_t rtl1_core_result(void);

// Frame encoding, for ABORT and for tests.
size_t rtl1_encode_frame(uint8_t *out, uint16_t nonce, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len);
