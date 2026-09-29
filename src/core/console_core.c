#include "console_core.h"

#include <stdio.h>
#include <string.h>

static struct {
    bool open;
    int owner;
    uint32_t start, first_line, last_line;
    bool any_line;
    char expect[CON_EXPECT_MAX];
    bool got_expect;
    uint32_t timeout_ms;
    char done_when[CON_EXPECT_MAX];
    bool done;                     // the done_when line came: close at the next poll
} w;

void console_core_begin(int owner, const char *expect, uint32_t timeout_ms, const char *done_when,
    uint32_t now_ms) {
    w.open = true;
    w.owner = owner;
    w.start = now_ms;
    w.any_line = false;
    snprintf(w.expect, sizeof(w.expect), "%s", expect ? expect : "");
    w.got_expect = false;
    w.timeout_ms = timeout_ms ? timeout_ms : CON_MAX_MS;
    snprintf(w.done_when, sizeof(w.done_when), "%s", done_when ? done_when : "");
    w.done = false;
}

// Whether `line` contains one of the '|'-separated texts in `list`.
static bool contains_any(const char *line, const char *list) {
    for (const char *p = list; *p;) {
        const size_t n = strcspn(p, "|");
        char one[CON_EXPECT_MAX];
        if (n && n < sizeof(one)) {
            memcpy(one, p, n);
            one[n] = 0;
            if (strstr(line, one)) return true;
        }
        p += n;
        if (*p == '|') p++;
    }
    return false;
}

int console_core_line(const char *line, uint32_t now_ms) {
    if (!w.open) return CON_BROADCAST;
    if (!w.any_line) w.first_line = now_ms;
    w.any_line = true;
    w.last_line = now_ms;
    if (w.expect[0] && strstr(line, w.expect)) w.got_expect = true;
    // A refusal is the whole answer, whatever the command.
    if ((w.done_when[0] && contains_any(line, w.done_when)) || strstr(line, "Bad Command")) w.done = true;
    return w.owner;
}

int32_t console_core_first_reply_ms(void) {
    return w.any_line ? (int32_t)(w.first_line - w.start) : -1;
}

bool console_core_waiting_reply(void) {
    return w.open && !w.any_line;
}

bool console_core_reply_coming(void) {
    return w.open && w.any_line && w.done_when[0] && !w.done;
}

bool console_core_poll(uint32_t now_ms) {
    if (!w.open) return true;
    if (w.done) {
        w.open = false;
        return true;
    }
    const uint32_t age = now_ms - w.start;
    bool over;
    if (w.expect[0] && !w.got_expect) {
        over = age >= w.timeout_ms; // still waiting for its line
    } else if (w.any_line) {
        over = now_ms - w.last_line >= CON_QUIET_MS || age >= (w.expect[0] ? w.timeout_ms : CON_MAX_MS);
    } else {
        over = age >= CON_NO_REPLY_MS; // no answer at all
    }
    if (over) w.open = false;
    return over;
}

bool console_core_got_expected(void) {
    return w.got_expect;
}
