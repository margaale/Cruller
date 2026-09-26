// The RT4K's text console, shared: every command goes through here, one at a time, and the reply
// lines are routed to whoever sent it (see console_core.h for how replies are told apart).
//
//   page (web terminal, remote)  CON_PAGE      its terminal shows every line but Cruller's own checks
//   power probes ("ver")         CON_POWER     nobody sees the replies (power.c reads all lines)
//   queries (rt4k_query)         CON_QUERY     the caller gets the line it waits for
//   POST /api/command            CON_HTTP      the request's own replies, in the response
//   RFC 2217 client n            CON_CLIENT(n) that client, plus lines outside any window

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_core.h"

#define CON_PAGE      0
#define CON_POWER     1
#define CON_QUERY     2
#define CON_HTTP      3   // POST /api/command
#define CON_CLIENT(n) (4 + (n))

void console_start(void);

// Queues a command (without line endings) from `owner`. False if it doesn't fit or the queue is full.
bool console_send(int owner, const char *cmd);

// Sends a command and waits for the first reply line containing `expect`, copied to out ("[COM] "
// dropped). False when no such line came within timeout_ms. One query at a time.
bool console_query(const char *cmd, const char *expect, char *out, size_t size, uint32_t timeout_ms);

// Sends a command as `owner` and waits until its reply window closes, handing each line routed to it
// to on_line. False if it couldn't be queued or sent (no RT4K, link busy). One caller per owner.
bool console_run(int owner, const char *cmd, void (*on_line)(const char *line, void *ctx), void *ctx);

// Text from the RT4K (rt4k task): split into lines and routed.
void console_feed(const uint8_t *data, size_t len);

// Routed lines, in order, for readers that pick their own (RFC 2217 clients). *seq: the reader's
// position, from console_head(); false when nothing is new. Lines older than the last 32 are gone.
uint32_t console_head(void);
bool console_read(uint32_t *seq, int *owner, char *out, size_t size);
