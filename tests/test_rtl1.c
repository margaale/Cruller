// Host unit tests for the RTL1 engine (src/rtl1_core.c). Run with tests/run.sh.
//
// The RT4K side is simulated: tests build the text lines and frames it would send, feed them to
// the engine (whole, in chunks, byte by byte), advance a fake clock, and check the outcome, the
// terminal text and the frames sent back. Reference data comes from a real RT4K Pro (fw 1.87.0).

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtl1_core.h"
#include "sha256_ref.h"

// --- tiny test framework ------------------------------------------------------------------------

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)
#define CHECK_STR_HAS(s, sub) do { checks++; if (!strstr((s), (sub))) { failures++; \
    printf("  FAIL %s:%d in %s: \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, current, (s), (sub)); } } while (0)

// --- simulated surroundings -----------------------------------------------------------------------

static uint8_t tx[4096];     // what the engine sent to the RT4K
static size_t tx_len;
static uint8_t term[65536];  // what reached the terminal
static size_t term_len;
static int finished_calls;
static uint32_t clock_ms;

static bool hook_write(const void *data, size_t len) {
    memcpy(tx + tx_len, data, len);
    tx_len += len;
    return true;
}

static void hook_text(const uint8_t *data, size_t len) {
    memcpy(term + term_len, data, len);
    term_len += len;
}

static bool hook_sha(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_ref(data, len, out);
    return true;
}

static void hook_finished(void) {
    finished_calls++;
}

static const rtl1_hooks_t hooks = {hook_write, hook_text, hook_sha, hook_finished};

static uint8_t out[8192];
static rtl1_info_t info;

static void reset(void) {
    rtl1_core_init(&hooks);
    tx_len = term_len = 0;
    finished_calls = 0;
    clock_ms = 1000;
    memset(out, 0, sizeof(out));
    memset(&info, 0, sizeof(info));
}

static void begin(const char *cmd, size_t max) {
    rtl1_core_begin(cmd, out, max, &info, false, 0, clock_ms);
}

static void begin_quiet(const char *cmd, size_t max) {
    rtl1_core_begin(cmd, out, max, &info, true, 0, clock_ms);
}

// Everything the RT4K sends in a test goes through here, so it can be split up as the test wants.
static uint8_t wire[16384];
static size_t wire_len;

static void put(const void *data, size_t len) {
    memcpy(wire + wire_len, data, len);
    wire_len += len;
}

static void put_str(const char *s) {
    put(s, strlen(s));
}

static void put_frame(uint16_t nonce, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len) {
    wire_len += rtl1_encode_frame(wire + wire_len, nonce, type, seq, payload, len);
}

// Delivers the wire in chunks of `chunk` bytes (0 = all at once), 1 ms apart.
static void deliver(size_t chunk) {
    size_t i = 0;
    while (i < wire_len) {
        size_t n = chunk ? chunk : wire_len;
        if (n > wire_len - i) n = wire_len - i;
        rtl1_core_feed(wire + i, n, clock_ms);
        clock_ms += 1;
        i += n;
    }
    wire_len = 0;
}

// Pseudo-random chunk sizes (1..300), deterministic per seed.
static void deliver_random(uint32_t seed) {
    size_t i = 0;
    while (i < wire_len) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 1 + (seed >> 16) % 300;
        if (n > wire_len - i) n = wire_len - i;
        rtl1_core_feed(wire + i, n, clock_ms++);
        i += n;
    }
    wire_len = 0;
}

static bool term_has_binary(void) {
    for (size_t i = 0; i + 1 < term_len; i++) {
        if (term[i] == 0xa5 && term[i + 1] == 0x5a) return true;
    }
    for (size_t i = 0; i < term_len; i++) {
        if (term[i] >= 0x80 || (term[i] < 0x20 && term[i] != '\n' && term[i] != '\r' && term[i] != '\t')) return true;
    }
    return false;
}

static bool term_equals(const char *s) {
    return term_len == strlen(s) && memcmp(term, s, term_len) == 0;
}

// --- fixtures ----------------------------------------------------------------------------------

static uint8_t osd2[4096];     // real "HDMI / No Signal" osd2 payload
static uint8_t osd2_digest[32];

#define READY_OSD2 "[COM] osd2 ready on=1 osk=0 rows=4 cols=32 stride=64 cells=2048 nonce=0x5224\n"
#define NONCE      0x5224

static void load_fixtures(void) {
    FILE *f = fopen("tests/fixtures/osd2_no_signal.bin", "rb");
    if (!f || fread(osd2, 1, sizeof(osd2), f) != sizeof(osd2)) {
        printf("cannot read tests/fixtures/osd2_no_signal.bin (run from the repo root)\n");
        exit(2);
    }
    fclose(f);
    sha256_ref(osd2, sizeof(osd2), osd2_digest);
}

// The RT4K's full answer to "osd2", as captured.
static void put_osd2_transfer(void) {
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
    put_str("[COM] osd done\n");
}

static bool sent_abort(uint16_t nonce, uint8_t seq) {
    uint8_t f[10];
    rtl1_encode_frame(f, nonce, 6, seq, NULL, 0);
    return tx_len == sizeof(f) && memcmp(tx, f, sizeof(f)) == 0;
}

// --- tests: reference data -----------------------------------------------------------------------

static void test_sha256_reference(void) {
    static const uint8_t abc_digest[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    uint8_t d[32];
    sha256_ref((const uint8_t *)"abc", 3, d);
    CHECK(memcmp(d, abc_digest, 32) == 0);
}

static void test_fixture_digest_matches_rt4k(void) {
    // The digest the RT4K sent in its RESPONSE frame for this screen.
    static const uint8_t rt4k_digest[32] = {
        0x6d, 0xf4, 0x73, 0x59, 0x33, 0x40, 0x8a, 0xf3, 0xb1, 0xaa, 0xf1, 0x83, 0x5f, 0x20, 0x63, 0xa1,
        0xb0, 0xae, 0x91, 0xd8, 0x92, 0x09, 0x09, 0x03, 0x21, 0x52, 0x55, 0xf3, 0x37, 0x25, 0xff, 0xf0};
    CHECK(memcmp(osd2_digest, rt4k_digest, 32) == 0);
}

static void test_crc_matches_rt4k_frames(void) {
    // Two RESPONSE frames captured from the RT4K (different sessions, same screen).
    uint8_t f[42];
    CHECK(rtl1_encode_frame(f, 0x5224, 2, 2, osd2_digest, 32) == 42);
    CHECK(f[40] == 0x22 && f[41] == 0x0c);
    rtl1_encode_frame(f, 0xdb65, 2, 2, osd2_digest, 32);
    CHECK(f[40] == 0xaa && f[41] == 0xb5);
    CHECK(f[0] == 0xa5 && f[1] == 0x5a && f[2] == 0x65 && f[3] == 0xdb && f[4] == 0x20 && f[5] == 0x00);
}

// --- tests: successful transfers -----------------------------------------------------------------

static void check_osd2_ok(void) {
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
    CHECK(finished_calls == 1);
    CHECK(info.len == 4096);
    CHECK(memcmp(out, osd2, 4096) == 0);
    CHECK_STR_HAS(info.ready, "nonce=0x5224");
    CHECK(term_equals(READY_OSD2 "[COM] osd done\n"));
    CHECK(!term_has_binary());
    CHECK(tx_len == 0);
}

static void test_ok_whole(void) {
    reset();
    begin("osd2", sizeof(out));
    put_osd2_transfer();
    deliver(0);
    check_osd2_ok();
}

static void test_ok_byte_by_byte(void) {
    reset();
    begin("osd2", sizeof(out));
    put_osd2_transfer();
    deliver(1);
    check_osd2_ok();
}

static void test_ok_usb_packets(void) {
    // 62 data bytes per FTDI packet.
    reset();
    begin("osd2", sizeof(out));
    put_osd2_transfer();
    deliver(62);
    check_osd2_ok();
}

static void test_ok_random_chunks(void) {
    for (uint32_t seed = 1; seed <= 50; seed++) {
        reset();
        begin("osd2", sizeof(out));
        put_osd2_transfer();
        deliver_random(seed);
        check_osd2_ok();
    }
}

static void test_ok_with_chatter_around(void) {
    // Unrelated console text before the ready line and after the closing line.
    reset();
    begin("osd2", sizeof(out));
    put_str("[COM] Serial Remote: menu\n");
    put_osd2_transfer();
    put_str("[COM] Input changed\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(term_equals("[COM] Serial Remote: menu\n" READY_OSD2 "[COM] osd done\n[COM] Input changed\n"));
}

static void test_ok_font_closes_with_get_done(void) {
    reset();
    begin("font", sizeof(out));
    put_str("[COM] font ready size=4096 glyphs=256 w=8 h=16 layout=row-major nonce=0x1ED5\n");
    put_frame(0x1ed5, 3, 0, osd2, 2048);
    put_frame(0x1ed5, 3, 1, osd2 + 2048, 2048);
    put_frame(0x1ed5, 2, 2, osd2_digest, 32);
    put_str("[COM] get done\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(finished_calls == 1);
    CHECK(info.len == 4096);
}

static void test_ok_ignores_noise_between_frames(void) {
    // Stray bytes between frames: the decoder resynchronises on the next A5 5A.
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    put("\x00\x11\xa5\x00", 4);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
    put_str("[COM] osd done\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(memcmp(out, osd2, 4096) == 0);
}

static void test_ok_without_closing_line(void) {
    // Data verified; the closing line never comes: success once the tail timeout passes.
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_DONE);
    CHECK(!rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS - 10));
    CHECK(rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS + 10));
    CHECK(rtl1_core_result() == RTL1_OK);
}

static void test_text_passes_through_when_idle(void) {
    // Outside a transfer every byte is terminal text, even ones that look like a frame.
    reset();
    put_str("[COM] hello\n");
    put("\xa5\x5a", 2);
    deliver(0);
    CHECK(term_len == 14);
    CHECK(tx_len == 0);
}

// --- tests: the RT4K refuses -----------------------------------------------------------------------

static void check_refused(const char *line) {
    reset();
    begin("osd", sizeof(out));
    put_str(line);
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_ERR_DEVICE);
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
    CHECK(finished_calls == 1);
    CHECK(info.len == 0);
    CHECK(tx_len == 0);
    CHECK(term_equals(line)); // the refusal is shown in the terminal
}

static void test_refused_nothing_shown(void) {
    check_refused("[COM] osd: nothing shown\n");
    CHECK_STR_HAS(info.detail, "nothing shown");
}

static void test_refused_busy(void) {
    check_refused("[COM] busy\n");
}

static void test_refused_bad_command(void) {
    check_refused("[COM] Bad Command: osd\n");
}

static void test_refused_unknown_command(void) {
    check_refused("[COM] Unknown command\n");
}

static void test_ready_without_nonce(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str("[COM] osd2 ready on=1 osk=0\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
    CHECK(finished_calls == 1);
    CHECK_STR_HAS(info.detail, "nonce");
}

static void test_ready_for_another_command_is_ignored(void) {
    // "osd ready" while waiting for "osd2": not ours.
    reset();
    begin("osd2", sizeof(out));
    put_str("[COM] osd ready rows=32 nonce=0x1111\n");
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_READY);
}

// --- tests: timeouts ----------------------------------------------------------------------------

static void test_no_ready_line(void) {
    reset();
    begin("osd2", sizeof(out));
    const uint32_t t0 = clock_ms;
    CHECK(!rtl1_core_poll(t0 + RTL1_READY_TIMEOUT_MS - 1));
    CHECK(rtl1_core_poll(t0 + RTL1_READY_TIMEOUT_MS + 1));
    CHECK(rtl1_core_result() == RTL1_ERR_TIMEOUT);
    CHECK_STR_HAS(info.detail, "no ready line");
    CHECK(tx_len == 0); // no nonce yet, nothing to abort
}

static void test_short_ready_timeout(void) {
    // Background polls wait less for the ready line.
    reset();
    rtl1_core_begin("osd", out, sizeof(out), &info, true, 600, clock_ms);
    const uint32_t t0 = clock_ms;
    CHECK(!rtl1_core_poll(t0 + 599));
    CHECK(rtl1_core_poll(t0 + 601));
    CHECK(rtl1_core_result() == RTL1_ERR_TIMEOUT);
}

static void test_stall_mid_transfer(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    deliver(0);
    const uint32_t t = clock_ms;
    CHECK(!rtl1_core_poll(t + RTL1_STALL_TIMEOUT_MS - 10));
    CHECK(!rtl1_core_poll(t + RTL1_STALL_TIMEOUT_MS + 10)); // aborts, then drains
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
    CHECK_STR_HAS(info.detail, "stalled after 2048 bytes");
    CHECK(sent_abort(NONCE, 1));
    CHECK(rtl1_core_poll(t + RTL1_STALL_TIMEOUT_MS + RTL1_TAIL_TIMEOUT_MS + 20)); // drain gives up
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
}

// --- tests: broken frames -------------------------------------------------------------------------

// After a failure the rest of the transfer must be swallowed, and the closing line ends it.
static void check_failure_drained(const char *detail, bool abort_expected, uint8_t abort_seq) {
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
    CHECK(finished_calls == 1);
    CHECK_STR_HAS(info.detail, detail);
    CHECK(abort_expected ? sent_abort(NONCE, abort_seq) : tx_len == 0);
    CHECK(!term_has_binary());
    CHECK(term_equals(READY_OSD2 "[COM] after\n")); // closing line swallowed, later text shown
}

static void test_bad_crc(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    const size_t at = wire_len;
    put_frame(NONCE, 3, 0, osd2, 2048);
    wire[at + 500] ^= 0x01; // one flipped bit in the payload
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
    put_str("[COM] osd done\n[COM] after\n");
    deliver(62);
    check_failure_drained("bad CRC", true, 0);
}

static void test_wrong_nonce(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(0x1234, 3, 0, osd2, 2048);
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    check_failure_drained("another session", true, 0);
}

static void test_sequence_gap(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048); // seq 0 missing
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    check_failure_drained("sequence gap: got 1, expected 0", true, 0);
}

static void test_nak(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    const uint8_t reason = 5; // busy
    put_frame(NONCE, 5, 0, &reason, 1);
    put_str("[COM] osd aborted\n[COM] after\n");
    deliver(0);
    check_failure_drained("NAK, reason 5", false, 0);
}

static void test_more_data_than_buffer(void) {
    reset();
    begin("osd2", 1024);
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    check_failure_drained("more data than the 1024-byte buffer", true, 0);
}

static void test_digest_mismatch(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    uint8_t wrong[32];
    memcpy(wrong, osd2_digest, 32);
    wrong[31] ^= 0xff;
    put_frame(NONCE, 2, 2, wrong, 32);
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    check_failure_drained("SHA-256 mismatch over 4096 bytes", false, 0); // RT4K already finished
}

static void test_digest_wrong_size(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 2, 0, osd2_digest, 16);
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    check_failure_drained("digest of 16 bytes", true, 0);
}

static void test_unexpected_frame_type(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 7, 0, NULL, 0); // ping
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    check_failure_drained("unexpected frame type 7", true, 0);
}

static void test_drain_gives_up(void) {
    // Failure, then the RT4K goes silent: the transfer still ends after the tail timeout.
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(0x1234, 3, 0, osd2, 2048);
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_DRAIN);
    CHECK(!rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS - 10));
    CHECK(rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS + 10));
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
}

static void test_oversized_length_resyncs(void) {
    // A header claiming more than 2048 bytes is not a frame: skipped, the real frames still work.
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put("\xa5\x5a\x24\x52\xff\xff", 6);
    put_frame(NONCE, 3, 0, osd2, 2048);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
    put_str("[COM] osd done\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_OK);
}

static void test_next_transfer_after_failure(void) {
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(0x1234, 3, 0, osd2, 2048);
    put_str("[COM] osd done\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
    rtl1_core_end();

    tx_len = term_len = 0;
    finished_calls = 0;
    memset(&info, 0, sizeof(info));
    begin("osd2", sizeof(out));
    put_osd2_transfer();
    deliver(0);
    check_osd2_ok();
}

// --- tests: quiet transfers (background polls) ----------------------------------------------

static void test_quiet_hides_transfer_lines(void) {
    reset();
    begin_quiet("osd2", sizeof(out));
    put_osd2_transfer();
    put_str("[COM] Input changed\n");
    deliver(62);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(memcmp(out, osd2, 4096) == 0);
    CHECK(term_equals("[COM] Input changed\n")); // only what came after the transfer
}

static void test_quiet_hides_refusal(void) {
    reset();
    begin_quiet("osd", sizeof(out));
    put_str("[COM] osd: nothing shown\n[COM] later\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_ERR_DEVICE);
    CHECK(term_equals("[COM] later\n"));
}

static void test_quiet_keeps_other_lines(void) {
    // A terminal command's reply landing during a background poll must still be shown.
    static const size_t chunks[] = {0, 1, 62};
    for (size_t k = 0; k < 3; k++) {
        reset();
        begin_quiet("osd2", sizeof(out));
        put_str("[COM] RT4KPRO, FW Version: 1.87.0\n");   // before the ready line
        put_str(READY_OSD2);
        put_frame(NONCE, 3, 0, osd2, 2048);
        put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
        put_frame(NONCE, 2, 2, osd2_digest, 32);
        put_str("[COM] Build tag: b0922t\n");             // between the last frame and the closing line
        put_str("[COM] osd done\n");
        deliver(chunks[k]);
        CHECK(rtl1_core_result() == RTL1_OK);
        CHECK(term_equals("[COM] RT4KPRO, FW Version: 1.87.0\n[COM] Build tag: b0922t\n"));
    }
}

static void test_quiet_then_loud(void) {
    // The flag belongs to one transfer.
    reset();
    begin_quiet("osd2", sizeof(out));
    put_osd2_transfer();
    deliver(0);
    rtl1_core_end();
    term_len = 0;
    finished_calls = 0;
    memset(&info, 0, sizeof(info));
    begin("osd2", sizeof(out));
    put_osd2_transfer();
    deliver(0);
    check_osd2_ok();
}

// --- runner ------------------------------------------------------------------------------------

#define T(fn) {#fn, fn}
static const struct { const char *name; void (*fn)(void); } tests[] = {
    T(test_sha256_reference),
    T(test_fixture_digest_matches_rt4k),
    T(test_crc_matches_rt4k_frames),
    T(test_ok_whole),
    T(test_ok_byte_by_byte),
    T(test_ok_usb_packets),
    T(test_ok_random_chunks),
    T(test_ok_with_chatter_around),
    T(test_ok_font_closes_with_get_done),
    T(test_ok_ignores_noise_between_frames),
    T(test_ok_without_closing_line),
    T(test_text_passes_through_when_idle),
    T(test_refused_nothing_shown),
    T(test_refused_busy),
    T(test_refused_bad_command),
    T(test_refused_unknown_command),
    T(test_ready_without_nonce),
    T(test_ready_for_another_command_is_ignored),
    T(test_no_ready_line),
    T(test_short_ready_timeout),
    T(test_stall_mid_transfer),
    T(test_bad_crc),
    T(test_wrong_nonce),
    T(test_sequence_gap),
    T(test_nak),
    T(test_more_data_than_buffer),
    T(test_digest_mismatch),
    T(test_digest_wrong_size),
    T(test_unexpected_frame_type),
    T(test_drain_gives_up),
    T(test_oversized_length_resyncs),
    T(test_next_transfer_after_failure),
    T(test_quiet_hides_transfer_lines),
    T(test_quiet_hides_refusal),
    T(test_quiet_keeps_other_lines),
    T(test_quiet_then_loud),
};

int main(void) {
    load_fixtures();
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
