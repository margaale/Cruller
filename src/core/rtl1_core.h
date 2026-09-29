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
    bool abort_on_error;     // send ABORT when a transfer goes wrong (acknowledged mode); else just drain
} rtl1_hooks_t;

typedef enum {
    RTL1_PH_IDLE,    // no transfer: text (a ready line or frames that come anyway start a drain)
    RTL1_PH_READY,   // command sent, waiting for "<name> ready ... nonce=0x..."
    RTL1_PH_BINARY,  // frames
    RTL1_PH_DONE,    // digest checked, waiting for the closing text line
    RTL1_PH_DRAIN,   // failed, aborted, or frames nobody waits for: swallow bytes until the closing text line
    RTL1_PH_SEND,    // upload (put): the caller sends frames; ACK/NAK frames and text lines come back
} rtl1_phase_t;

// What the RT4K has answered so far during an upload.
typedef struct {
    uint32_t acks, naks;     // ACK / NAK frames received
    uint8_t last_ack;        // sequence number of the last ACK
    uint8_t nak_seq, nak_reason;
    uint16_t nonce;          // from the ready line: the caller's frames carry it
} rtl1_put_state_t;

void rtl1_core_init(const rtl1_hooks_t *hooks);

// Starts waiting for the ready line of `cmd` (whose first word names it). The caller then sends the
// command; `out` and `info` must stay valid until the transfer ends. `quiet`: the transfer's own lines
// (ready, closing, refusal) stay out of the terminal, also when they come after it gave up waiting
// for them; other lines still show. ready_timeout_ms: how long to wait for the ready line
// (0 = RTL1_READY_TIMEOUT_MS).
void rtl1_core_begin(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info, bool quiet,
    uint32_t ready_timeout_ms, uint32_t now_ms);

// Starts an upload: `cmd` is the whole put command ("put -a <size> <sha256> <path>"). After the ready
// line the phase is RTL1_PH_SEND: the caller sends the data frames (rtl1_encode_frame) and follows
// the RT4K's replies with rtl1_core_put_state(). The transfer ends with the RT4K's closing line:
// "put done" (RTL1_OK) or any other "put..." line (RTL1_ERR_DEVICE, the line in info->detail).
// Always quiet. There is no timeout while sending: the caller has its own.
void rtl1_core_begin_put(const char *cmd, rtl1_info_t *info, uint32_t ready_timeout_ms, uint32_t now_ms);
void rtl1_core_put_state(rtl1_put_state_t *out);

// Bytes from the RT4K. Text goes to hooks->text a whole line at a time; a transfer's bytes, from its
// ready line to its closing line, never do: not when the ready line was lost or came late (frames are
// known by their header), nor after the transfer failed or timed out.
void rtl1_core_feed(const uint8_t *data, size_t len, uint32_t now_ms);

// Timeouts. True once the transfer is over (the result is then final).
bool rtl1_core_poll(uint32_t now_ms);

// Forgets the transfer (caller gave up, or is done reading the result). A drain goes on: the rest of
// the transfer is swallowed until its closing line, or until the RT4K has been quiet for
// RTL1_TAIL_TIMEOUT_MS.
void rtl1_core_end(void);

rtl1_phase_t rtl1_core_phase(void);
rtl1_result_t rtl1_core_result(void);

// Frame encoding, for ABORT and for tests.
size_t rtl1_encode_frame(uint8_t *out, uint16_t nonce, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len);
