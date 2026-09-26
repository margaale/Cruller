// Host unit tests for the console reply windows (src/console_core.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "console_core.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)

static uint32_t now;

// Polls every 10 ms until the window closes (or `limit` ms); returns how long it stayed open.
static uint32_t run_until_closed(uint32_t limit) {
    const uint32_t t0 = now;
    while (!console_core_poll(now) && now - t0 < limit) now += 10;
    return now - t0;
}

static void reset(void) {
    now = 1000;
    run_until_closed(100000); // close anything left open
}

static void test_no_window_broadcasts(void) {
    reset();
    CHECK(console_core_line("[MCU] Powering Up", now) == CON_BROADCAST);
    CHECK(console_core_poll(now));
}

static void test_reply_goes_to_sender(void) {
    reset();
    console_core_begin(4, NULL, 0, NULL, now);
    now += 5;
    CHECK(console_core_line("[COM] Serial Remote: menu", now) == 4);
    CHECK(!console_core_poll(now));
}

static void test_closes_after_quiet(void) {
    reset();
    console_core_begin(4, NULL, 0, NULL, now);
    now += 5;
    console_core_line("[COM] RT4KPRO, FW Version: 1.87.3", now);
    now += 3;
    console_core_line("[COM] Build tag: b0925a", now); // multi-line replies stay together
    const uint32_t open = run_until_closed(5000);
    CHECK(open >= CON_QUIET_MS && open < CON_QUIET_MS + 20);
    CHECK(console_core_line("[COM] later", now) == CON_BROADCAST);
}

static void test_no_reply_closes(void) {
    reset();
    console_core_begin(5, NULL, 0, NULL, now); // e.g. SVS: never answered
    const uint32_t open = run_until_closed(5000);
    CHECK(open >= CON_NO_REPLY_MS && open < CON_NO_REPLY_MS + 20);
}

static void test_long_listing_stays_together(void) {
    reset();
    console_core_begin(4, NULL, 0, NULL, now);
    int mine = 0;
    for (int i = 0; i < 40; i++) { // ls: one entry every 20 ms
        now += 20;
        if (console_core_line("[COM] ent t=F sz=1 nm=x", now) == 4) mine++;
        CHECK(!console_core_poll(now));
    }
    CHECK(mine == 40);
}

static void test_chatty_window_is_capped(void) {
    reset();
    console_core_begin(4, NULL, 0, NULL, now);
    const uint32_t t0 = now;
    while (!console_core_poll(now) && now - t0 < 20000) {
        now += 50;
        console_core_line("[COM] spam", now);
    }
    CHECK(now - t0 >= CON_MAX_MS && now - t0 < CON_MAX_MS + 100);
}

static void test_expect_waits_for_its_line(void) {
    reset();
    console_core_begin(2, "fwup", 15000, NULL, now);
    now += 20;
    console_core_line("[COM] Serial Remote: menu", now); // not it
    now += 3000;
    CHECK(!console_core_poll(now)); // quiet, but its line hasn't come: still open
    now += 4000;
    CHECK(console_core_line("[COM] fwup ok version=1.87.3 token=AB12", now) == 2);
    CHECK(console_core_got_expected());
    const uint32_t open = run_until_closed(5000);
    CHECK(open >= CON_QUIET_MS && open < CON_QUIET_MS + 20);
}

static void test_expect_timeout(void) {
    reset();
    console_core_begin(2, "fwup", 2000, NULL, now);
    const uint32_t open = run_until_closed(10000);
    CHECK(open >= 2000 && open < 2020);
    CHECK(!console_core_got_expected());
}

static void test_next_window_new_owner(void) {
    reset();
    console_core_begin(4, NULL, 0, NULL, now);
    now += 5;
    console_core_line("[COM] a", now);
    run_until_closed(5000);
    console_core_begin(5, NULL, 0, NULL, now);
    now += 5;
    CHECK(console_core_line("[COM] b", now) == 5);
}

static void test_key_closes_on_its_reply(void) {
    reset();
    console_core_begin(4, NULL, 0, "Serial Remote:", now);
    CHECK(console_core_waiting_reply());
    now += 4;
    CHECK(console_core_line("[COM] Serial Remote: down", now) == 4);
    CHECK(!console_core_waiting_reply());
    CHECK(console_core_poll(now)); // at once: no quiet wait
    CHECK(console_core_line("[COM] next", now) == CON_BROADCAST);
}

static void test_refusal_closes_at_once(void) {
    reset();
    console_core_begin(4, NULL, 0, "Serial Remote:", now);
    now += 4;
    console_core_line("[COM] Bad Command: remote dwn", now);
    CHECK(console_core_poll(now));
    reset();
    console_core_begin(4, NULL, 0, NULL, now); // no final line known: a refusal still ends it
    now += 4;
    console_core_line("[COM] Bad Command: nonsense", now);
    CHECK(console_core_poll(now));
}

static void test_alternative_final_lines(void) {
    reset();
    console_core_begin(4, NULL, 0, "ls end|ls err|ls:", now);
    for (int i = 0; i < 5; i++) {
        now += 2;
        console_core_line("[COM] ent t=F sz=1 nm=x", now);
        CHECK(!console_core_poll(now));
    }
    now += 2;
    console_core_line("[COM] ls end 5", now);
    CHECK(console_core_poll(now));
    reset();
    console_core_begin(4, NULL, 0, "ls end|ls err|ls:", now);
    now += 2;
    console_core_line("[COM] ls err=2 NOSUCH", now); // the second alternative
    CHECK(console_core_poll(now));
}

static void test_other_line_than_final_waits(void) {
    reset();
    console_core_begin(4, NULL, 0, "Build tag:", now);
    now += 5;
    console_core_line("[COM] RT4KPRO, FW Version: 1.87.3", now);
    CHECK(!console_core_poll(now)); // not the last line yet
    now += 3;
    console_core_line("[COM] Build tag: b0925a", now);
    CHECK(console_core_poll(now));
}

static void test_waiting_reply_until_no_reply_timeout(void) {
    reset();
    console_core_begin(5, NULL, 0, NULL, now);
    CHECK(console_core_waiting_reply());
    run_until_closed(5000);
    CHECK(!console_core_waiting_reply());
}

#define T(fn) {#fn, fn}
static const struct { const char *name; void (*fn)(void); } tests[] = {
    T(test_key_closes_on_its_reply),
    T(test_refusal_closes_at_once),
    T(test_alternative_final_lines),
    T(test_other_line_than_final_waits),
    T(test_waiting_reply_until_no_reply_timeout),
    T(test_no_window_broadcasts),
    T(test_reply_goes_to_sender),
    T(test_closes_after_quiet),
    T(test_no_reply_closes),
    T(test_long_listing_stays_together),
    T(test_chatty_window_is_capped),
    T(test_expect_waits_for_its_line),
    T(test_expect_timeout),
    T(test_next_window_new_owner),
};

int main(void) {
    const size_t n = sizeof(tests) / sizeof(tests[0]);
    size_t failed_tests = 0;
    for (size_t i = 0; i < n; i++) {
        current = tests[i].name;
        const int before = failures;
        tests[i].fn();
        const bool ok = failures == before;
        if (!ok) failed_tests++;
        printf("%s %s\n", ok ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%u tests, %d checks, %u failed\n", (unsigned)n, checks, (unsigned)failed_tests);
    return failed_tests ? 1 : 0;
}
