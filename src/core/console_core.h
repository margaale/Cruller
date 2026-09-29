// Who a console line from the RT4K belongs to, without an RTOS. console.c drives it; the host unit
// tests (tests/test_console.c) drive it directly.
//
// The RT4K's replies carry no sender, so commands go out one at a time and each opens a reply window:
// the lines that come back meanwhile belong to that command's sender. The window closes once the
// replies go quiet (CON_QUIET_MS after the last line), or CON_NO_REPLY_MS after the command if
// nothing came back (some commands, like SVS, never answer). A command waiting for a particular line
// (expect) keeps it open until that line and the quiet after it, or its timeout. Lines outside any
// window (boot messages, "Power On Requested"...) belong to nobody: everyone gets them.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CON_BROADCAST  (-1)  // a line outside any window
#define CON_QUIET_MS    50   // above the FT232R's 16 ms latency timer, which can split a reply in two
#define CON_NO_REPLY_MS 1000
#define CON_MAX_MS      5000 // a window never lasts longer, however chatty (unless expect's timeout)
#define CON_EXPECT_MAX  48

// Opens a window for `owner` (>= 0). expect: a text the reply must contain (NULL/"" for none);
// timeout_ms: how long to wait for it (0: CON_MAX_MS). done_when: the reply's last line is known to
// contain one of these '|'-separated texts ("Serial Remote:" for a remote key, "ls end|ls err|ls:"):
// the window closes as soon as it comes, without the quiet wait (NULL/"" for none; other lines follow
// the usual rules). A "Bad Command" line always closes it: a refusal is the whole answer.
void console_core_begin(int owner, const char *expect, uint32_t timeout_ms, const char *done_when,
    uint32_t now_ms);

// True while the window is open and nothing has come back yet.
bool console_core_waiting_reply(void);

// True while a reply has started and its known last line (done_when) hasn't come yet ("ls" sends its
// entries for up to ~165 ms).
bool console_core_reply_coming(void);

// How long after the command its first reply line came (-1: none), for the open or last window.
int32_t console_core_first_reply_ms(void);

// A line from the RT4K: returns its owner (CON_BROADCAST outside a window).
int console_core_line(const char *line, uint32_t now_ms);

// True once no window is open (the last one has closed).
bool console_core_poll(uint32_t now_ms);

// Whether the open (or last) window saw its expected line.
bool console_core_got_expected(void);
