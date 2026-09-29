// Host unit tests for the RFC 2217 server side (src/core/rfc2217_proto.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "rfc2217_proto.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)

#define IAC 255
#define SB 250
#define SE 240
#define WILL 251
#define WONT 252
#define DO 253
#define DONT 254

static rfc2217_t s;
static uint8_t data[256], reply[256];
static rfc2217_io_t io;

static void reset(void) {
    rfc2217_init(&s);
    io = (rfc2217_io_t){.data = data, .data_max = sizeof(data), .reply = reply, .reply_max = sizeof(reply)};
}

static void feed(const uint8_t *in, size_t len) {
    io.data_len = io.reply_len = 0;
    rfc2217_input(&s, in, len, &io, 0x30);
}

static bool reply_is(const uint8_t *want, size_t len) {
    return io.reply_len == len && !memcmp(reply, want, len);
}

static void test_plain_data_passes(void) {
    reset();
    feed((const uint8_t *)"remote menu\r\n", 13);
    CHECK(io.data_len == 13 && !memcmp(data, "remote menu\r\n", 13));
    CHECK(io.reply_len == 0);
}

static void test_escaped_ff_in_data(void) {
    reset();
    const uint8_t in[] = {'a', IAC, IAC, 'b'};
    feed(in, sizeof(in));
    CHECK(io.data_len == 3 && data[0] == 'a' && data[1] == 0xff && data[2] == 'b');
}

static void test_greeting(void) {
    reset();
    uint8_t out[32];
    const size_t n = rfc2217_greeting(&s, out, sizeof(out));
    const uint8_t want[] = {IAC, WILL, 0, IAC, DO, 0, IAC, WILL, 3, IAC, DO, 3, IAC, DO, 44};
    CHECK(n == sizeof(want) && !memcmp(out, want, n));
}

static void test_com_port_will_gets_do(void) {
    reset();
    const uint8_t in[] = {IAC, WILL, 44};
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, DO, 44};
    CHECK(reply_is(want, sizeof(want)));
}

static void test_known_options_answered_once(void) {
    reset();
    const uint8_t in[] = {IAC, DO, 0};
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, WILL, 0};
    CHECK(reply_is(want, sizeof(want)));
    feed(in, sizeof(in)); // again: already on, no answer (no ping-pong)
    CHECK(io.reply_len == 0);
}

static void test_after_greeting_no_echo_of_our_offers(void) {
    reset();
    uint8_t out[32];
    rfc2217_greeting(&s, out, sizeof(out));
    const uint8_t in[] = {IAC, DO, 0, IAC, WILL, 0, IAC, DO, 3, IAC, WILL, 3};
    feed(in, sizeof(in));
    CHECK(io.reply_len == 0); // the client just agreed with what we offered
}

static void test_unknown_options_refused(void) {
    reset();
    const uint8_t in[] = {IAC, DO, 1, IAC, WILL, 24}; // ECHO, TERMINAL-TYPE
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, WONT, 1, IAC, DONT, 24};
    CHECK(reply_is(want, sizeof(want)));
}

static void test_set_baudrate_echoed(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 1, 0x00, 0x01, 0xc2, 0x00, IAC, SE}; // 115200
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, SB, 44, 101, 0x00, 0x01, 0xc2, 0x00, IAC, SE};
    CHECK(reply_is(want, sizeof(want)));
    CHECK(s.baud == 115200);
    CHECK(io.data_len == 0); // commands never leak into the serial data
}

static void test_baudrate_query(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 1, 0, 0, 0, 0, IAC, SE}; // 0: what is it?
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, SB, 44, 101, 0x00, 0x1e, 0x84, 0x80, IAC, SE}; // 2000000
    CHECK(reply_is(want, sizeof(want)));
}

static void test_datasize_parity_stopsize(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 2, 8, IAC, SE, IAC, SB, 44, 3, 1, IAC, SE, IAC, SB, 44, 4, 1, IAC, SE};
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, SB, 44, 102, 8, IAC, SE, IAC, SB, 44, 103, 1, IAC, SE, IAC, SB, 44, 104, 1, IAC, SE};
    CHECK(reply_is(want, sizeof(want)));
}

static void test_control_and_purge_acknowledged(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 5, 1, IAC, SE, IAC, SB, 44, 5, 8, IAC, SE, IAC, SB, 44, 12, 3, IAC, SE};
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, SB, 44, 105, 1, IAC, SE, IAC, SB, 44, 105, 8, IAC, SE, IAC, SB, 44, 112, 3, IAC, SE};
    CHECK(reply_is(want, sizeof(want)));
}

static void test_modem_state(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 7, 0, IAC, SE};
    feed(in, sizeof(in));
    const uint8_t want[] = {IAC, SB, 44, 107, 0x30, IAC, SE}; // CTS and DSR, as passed in
    CHECK(reply_is(want, sizeof(want)));
}

static void test_modem_state_pushed(void) {
    uint8_t out[16];
    const size_t n = rfc2217_modemstate(0x30, out, sizeof(out));
    const uint8_t want[] = {IAC, SB, 44, 107, 0x30, IAC, SE};
    CHECK(n == sizeof(want) && !memcmp(out, want, n));
}

static void test_signature(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 0, IAC, SE};
    feed(in, sizeof(in));
    CHECK(io.reply_len > 7 && reply[3] == 100 && !memcmp(reply + 4, "Cruller", 7));
}

static void test_split_across_reads(void) {
    reset();
    const uint8_t in[] = {'x', IAC, SB, 44, 1, 0x00, 0x01, 0xc2, 0x00, IAC, SE, 'y'};
    size_t data_total = 0, reply_total = 0;
    uint8_t d[16], r[32];
    for (size_t i = 0; i < sizeof(in); i++) { // one byte at a time
        feed(in + i, 1);
        memcpy(d + data_total, data, io.data_len);
        data_total += io.data_len;
        memcpy(r + reply_total, reply, io.reply_len);
        reply_total += io.reply_len;
    }
    CHECK(data_total == 2 && d[0] == 'x' && d[1] == 'y');
    CHECK(reply_total == 10 && r[3] == 101);
}

static void test_escaped_ff_in_subneg(void) {
    reset();
    const uint8_t in[] = {IAC, SB, 44, 1, 0x00, 0x00, IAC, IAC, IAC, IAC, IAC, SE}; // 65535, both 0xFF doubled
    feed(in, sizeof(in));
    CHECK(s.baud == 65535);
    const uint8_t want[] = {IAC, SB, 44, 101, 0x00, 0x00, IAC, IAC, IAC, IAC, IAC, SE};
    CHECK(reply_is(want, sizeof(want))); // 0xFF doubled on the way out too
}

static void test_escape_out(void) {
    const uint8_t in[] = {'a', 0xff, 'b'};
    uint8_t out[8];
    size_t used;
    const size_t n = rfc2217_escape(in, 3, out, sizeof(out), &used);
    CHECK(n == 4 && used == 3 && out[1] == 0xff && out[2] == 0xff);
    const size_t m = rfc2217_escape(in, 3, out, 2, &used); // room for 'a' only (0xFF needs 2)
    CHECK(m == 1 && used == 1);
}

static void test_overlong_subneg_ignored(void) {
    reset();
    uint8_t in[100];
    size_t n = 0;
    in[n++] = IAC; in[n++] = SB; in[n++] = 44; in[n++] = 1;
    while (n < 96) in[n++] = 1;
    in[n++] = IAC; in[n++] = SE;
    in[n++] = 'z';
    feed(in, n);
    CHECK(io.reply_len == 0);
    CHECK(io.data_len == 1 && data[0] == 'z'); // and the parser is back on its feet
}

#define T(fn) {#fn, fn}
static const struct { const char *name; void (*fn)(void); } tests[] = {
    T(test_plain_data_passes),
    T(test_escaped_ff_in_data),
    T(test_greeting),
    T(test_com_port_will_gets_do),
    T(test_known_options_answered_once),
    T(test_after_greeting_no_echo_of_our_offers),
    T(test_unknown_options_refused),
    T(test_set_baudrate_echoed),
    T(test_baudrate_query),
    T(test_datasize_parity_stopsize),
    T(test_control_and_purge_acknowledged),
    T(test_modem_state),
    T(test_modem_state_pushed),
    T(test_signature),
    T(test_split_across_reads),
    T(test_escaped_ff_in_subneg),
    T(test_escape_out),
    T(test_overlong_subneg_ignored),
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
