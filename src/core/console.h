// The RT4K's text console, shared: every command goes through here, one at a time, and the reply
// lines are routed to whoever sent it (see console_core.h for how replies are told apart).
//
//   page (web terminal, remote)  CON_PAGE      its terminal shows every line but Cruller's own checks
//   power probes ("ver")         CON_POWER     nobody sees the replies (power.c reads all lines)
//   queries (rt4k_query)         CON_QUERY     the caller gets the line it waits for
//   POST /api/command            CON_HTTP      the request's own replies, in the response
//   the page's SD card browser   CON_FILES     the request's own replies (GET /rt4k/ls), not the terminal
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
#define CON_FILES     4   // GET /rt4k/ls
#define CON_CLIENT(n) (5 + (n))

#define CON_LINE_MAX  160 // a longer line from the RT4K arrives cut in pieces of CON_LINE_MAX - 1 characters

void console_start(void);

// Queues a command (without line endings) from `owner`. False if it doesn't fit or the queue is full.
bool console_send(int owner, const char *cmd);

// Sends a command and waits for the first reply line containing `expect`, copied to out ("[COM] "
// dropped). False when no such line came within timeout_ms. One query at a time.
bool console_query(const char *cmd, const char *expect, char *out, size_t size, uint32_t timeout_ms);

// Sends a command as `owner` and waits until its reply window closes, handing each line routed to it
// to on_line. False if it couldn't be queued or sent (no RT4K, link busy). One caller per owner.
bool console_run(int owner, const char *cmd, void (*on_line)(const char *line, void *ctx), void *ctx);

// Debug: the last commands with their reply and window times, as text (GET /debug/console).
size_t console_debug(char *out, size_t size);
// The same as a JSON array, oldest first: [{"cmd","owner","reply_ms","window_ms"}]. 0 if it didn't fit.
size_t console_debug_json(char *out, size_t size);

// True while the last command sent hasn't been answered yet (its window is open, no line so far).
bool console_reply_pending(void);

// Text from the RT4K (rt4k task): split into lines and routed.
void console_feed(const uint8_t *data, size_t len);

// Routed lines, in order, for readers that pick their own (RFC 2217 clients). *seq: the reader's
// position, from console_head(); false when nothing is new. Lines older than the last 32 are gone.
uint32_t console_head(void);
bool console_read_line(uint32_t *seq, int *owner, char *out, size_t size);
