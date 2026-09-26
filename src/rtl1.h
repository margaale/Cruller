// RTL1: the RT4K's binary transfer protocol (firmware 1.75+), see docs/RTL1.md.
//
// A transfer is a text command ("osd", "osd2", "font", "get -- <path>") answered by a
// "<cmd> ready ... nonce=0x<hex>" line, then binary frames (DATA..., RESPONSE with the SHA-256 of
// all the data), then a text line ending in "done". Everything the RT4K sends goes through
// rtl1_feed(): text reaches the terminal, frames never do.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RTL1_OK,
    RTL1_ERR_NO_LINK,   // RT4K not connected, or the link is busy
    RTL1_ERR_TIMEOUT,   // no ready line, or the transfer stalled
    RTL1_ERR_DEVICE,    // the RT4K refused it (busy, bad command, nothing shown...), see detail
    RTL1_ERR_PROTOCOL,  // bad CRC, nonce, sequence, NAK, too much data, digest mismatch
} rtl1_result_t;

typedef struct {
    size_t len;         // payload bytes received
    char ready[160];    // the ready line (without the "[COM] " prefix)
    char detail[96];    // why it failed
} rtl1_info_t;

void rtl1_init(void);

// Bytes received from the RT4K (rt4k task).
void rtl1_feed(const uint8_t *data, size_t len);

// Runs one transfer and waits for it (up to about 5 s). One at a time; text commands wait meanwhile.
// quiet: the transfer's own lines (ready, done, refusal) stay out of the terminal (background polls).
// ready_timeout_ms: 0 = default (3 s); background polls use less, so a lost request frees the link soon.
rtl1_result_t rtl1_transfer(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info, bool quiet,
    uint32_t ready_timeout_ms);

// Where an upload's bytes come from: fills up to `max` bytes, returns how many (0 = no more).
typedef size_t (*rtl1_read_fn)(void *ctx, uint8_t *buf, size_t max);

// Writes a file to the RT4K's SD card (RTL1 put, acknowledged mode): `size` bytes read from `read`,
// whose SHA-256 (64 hex digits) the RT4K checks at the end. Blocks for the whole upload (about
// 100 KB/s); other transfers and text commands wait meanwhile. RTL1_OK once the RT4K said "put done".
rtl1_result_t rtl1_put(const char *path, uint32_t size, const char *sha256_hex, rtl1_read_fn read, void *ctx,
    rtl1_info_t *info);

const char *rtl1_result_name(rtl1_result_t r);

// Debug: raw bytes received during the last transfer that failed with a protocol error.
size_t rtl1_last_failure(const uint8_t **data);

// Debug: refuse transfers (RTL1_ERR_NO_LINK) for the next `ms`, so raw bytes sent to the RT4K
// (POST /debug/raw) don't interleave with the mirror's polls.
void rtl1_pause(uint32_t ms);
