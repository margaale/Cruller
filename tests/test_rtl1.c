// Host unit tests for the RTL1 engine (src/core/rtl1_core.c). Run with tests/run.sh.
//
// The RT4K side is simulated: tests build the text lines and frames it would send, feed them to
// the engine (whole, in chunks, byte by byte), advance a fake clock, and check the outcome, the
// terminal text and the frames sent back. Reference data comes from a real RT4K Pro (fw 1.87.0).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "rtl1_core.h"
#include "sha256_ref.h"

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

static rtl1_hooks_t hooks = {hook_write, hook_text, hook_sha, hook_finished, true};

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

// The RT4K's frames for "osd2", and its full answer, as captured.
static void put_osd2_frames(void) {
    put_frame(NONCE, 3, 0, osd2, 2048);
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
}

static void put_osd2_transfer(void) {
    put_str(READY_OSD2);
    put_osd2_frames();
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
    // The payload already in place (an upload reads it straight into the frame): the same frame.
    uint8_t g[42];
    memcpy(g + 8, osd2_digest, 32);
    CHECK(rtl1_encode_frame(g, 0xdb65, 2, 2, g + 8, 32) == 42 && !memcmp(f, g, 42));
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

static void test_ok_whole_bytes_packets(void) {
    static const size_t chunks[] = {0, 1, 62}; // whole, byte by byte, FTDI packets (62 data bytes each)
    for (size_t k = 0; k < 3; k++) {
        reset();
        begin("osd2", sizeof(out));
        put_osd2_transfer();
        deliver(chunks[k]);
        check_osd2_ok();
    }
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
    put_osd2_frames();
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_DONE);
    CHECK(!rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS - 10));
    CHECK(rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS + 10));
    CHECK(rtl1_core_result() == RTL1_OK);
}

static void test_text_passes_through_when_idle(void) {
    // Outside a transfer text lines go through whole, even with bytes like a frame's magic in them.
    reset();
    put_str("[COM] hello\n");
    put("\xa5\x5a\n", 3);
    deliver(1);
    CHECK(term_len == 15);
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

static void test_refused_with_a_reason(void) {
    check_refused("[COM] busy\n");
    check_refused("[COM] Bad Command: osd\n");
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

// One flipped bit in a payload: drained, the closing line ends it; an ABORT sent too unless in USB mode
// (where it would arrive after the transfer).
static void bad_crc(bool abort_on_error) {
    hooks.abort_on_error = abort_on_error;
    reset();
    begin("osd2", sizeof(out));
    put_str(READY_OSD2);
    const size_t at = wire_len;
    put_osd2_frames();
    wire[at + 500] ^= 0x01; // in the first frame's payload
    put_str("[COM] osd done\n[COM] after\n");
    deliver(62);
    check_failure_drained("bad CRC", abort_on_error, 0);
    hooks.abort_on_error = true;
}

static void test_bad_crc(void) {
    bad_crc(true);
}

static void test_bad_crc_without_abort(void) {
    bad_crc(false);
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
    put_osd2_frames();
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
        put_osd2_frames();
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

// --- tests: frames nobody is decoding ---------------------------------------------------------------
//
// Whatever goes wrong, a transfer's bytes (from its ready line to its closing line) never reach the
// terminal: console.c cuts lines at their first NUL, so leaked frames showed as one line of binary
// and the closing line, "<end of the digest and CRC>[COM] osd done", kept by every page loaded later.

static void test_ready_line_lost(void) {
    // A mirror poll whose ready line lost its end (the FT232R overran while Wi-Fi was busy, here a
    // download from GitHub): the frames came while the transfer still waited for its ready line.
    static const size_t chunks[] = {0, 1, 62};
    for (size_t k = 0; k < 3; k++) {
        reset();
        rtl1_core_begin("osd2", out, sizeof(out), &info, true, 600, clock_ms);
        put_str("[COM] osd2 ready on=1 osk=0 rows=4 co"); // the rest of the line never came
        put_osd2_frames();
        put_str("[COM] osd done\n[COM] after\n");
        deliver(chunks[k]);
        CHECK(!term_has_binary());
        CHECK(term_equals("[COM] after\n")); // a quiet poll: its closing line stays hidden too
        CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL); // the RT4K did answer
        CHECK_STR_HAS(info.detail, "frames without a ready line");
        CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
        CHECK(finished_calls == 1);
    }
}

static void test_ready_line_lost_whole(void) {
    // All of the ready line lost, and a transfer shown in the terminal: still no binary.
    reset();
    begin("osd2", sizeof(out));
    put_osd2_frames();
    put_str("[COM] osd done\n[COM] after\n");
    deliver(62);
    CHECK(!term_has_binary());
    CHECK(term_equals("[COM] after\n"));
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
}

static void test_ready_line_after_the_wait(void) {
    // The ready line came after the poll gave up waiting: its frames and closing line are drained,
    // and the ready line itself stays hidden like the rest of a quiet poll.
    reset();
    rtl1_core_begin("osd2", out, sizeof(out), &info, true, 600, clock_ms);
    CHECK(rtl1_core_poll(clock_ms + 601));
    CHECK(rtl1_core_result() == RTL1_ERR_TIMEOUT);
    rtl1_core_end();
    clock_ms += 700;
    put_osd2_transfer();
    put_str("[COM] after\n");
    deliver(62);
    CHECK(!term_has_binary());
    CHECK(term_equals("[COM] after\n"));
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
}

static void test_rest_after_giving_up(void) {
    // The transfer stops mid-frame (the USB host parked while flash is written, say), the caller gives
    // up, and the rest arrives much later: still drained, still no binary.
    reset();
    begin_quiet("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_frame(NONCE, 3, 0, osd2, 2048);
    const size_t cut = wire_len + 1000;
    put_frame(NONCE, 3, 1, osd2 + 2048, 2048);
    put_frame(NONCE, 2, 2, osd2_digest, 32);
    put_str("[COM] osd done\n[COM] after\n");
    static uint8_t rest[4096];
    const size_t rest_len = wire_len - cut;
    memcpy(rest, wire + cut, rest_len);
    wire_len = cut;
    deliver(62);
    const uint32_t t = clock_ms;
    CHECK(!rtl1_core_poll(t + RTL1_STALL_TIMEOUT_MS + 10));
    CHECK(rtl1_core_poll(t + RTL1_STALL_TIMEOUT_MS + RTL1_TAIL_TIMEOUT_MS + 20));
    rtl1_core_end();
    clock_ms = t + 30000;
    put(rest, rest_len);
    deliver(62);
    CHECK(!term_has_binary());
    CHECK(term_equals("[COM] after\n"));
}

static void test_tail_of_frames_without_control_bytes(void) {
    // The last bytes of a digest (the line seen on the page, minus its unprintable bytes) glued to the
    // closing line: no control character to give them away, still not console text.
    reset();
    put_str("\xaa" "0\x99\xbf/\xf0q\xc8" "C\xbb" "e\xb5\xf4\xe2!>V!9\xbf\xf8[COM] osd done\n[COM] after\n");
    deliver(0);
    CHECK(term_equals("[COM] after\n"));
}

static void test_frames_while_idle(void) {
    // Frames with no transfer at all: drained; once the RT4K has gone quiet, text shows again.
    reset();
    put_osd2_frames();
    deliver(62);
    CHECK(term_len == 0);
    CHECK(rtl1_core_phase() == RTL1_PH_DRAIN);
    clock_ms += RTL1_TAIL_TIMEOUT_MS + 10;
    put_str("[COM] after\n");
    deliver(0);
    CHECK(term_equals("[COM] after\n"));
}

static void test_transfer_typed_in_terminal(void) {
    // "osd2" typed in the terminal: its ready line shows, its frames don't.
    reset();
    put_osd2_transfer();
    put_str("[COM] after\n");
    deliver(62);
    CHECK(!term_has_binary());
    CHECK(term_equals(READY_OSD2 "[COM] after\n"));
}

static void test_closing_line_after_the_wait(void) {
    // Data verified, the closing line came after the tail timeout: a quiet poll's, so still hidden.
    reset();
    begin_quiet("osd2", sizeof(out));
    put_str(READY_OSD2);
    put_osd2_frames();
    deliver(0);
    CHECK(rtl1_core_poll(clock_ms + RTL1_TAIL_TIMEOUT_MS + 10));
    CHECK(rtl1_core_result() == RTL1_OK);
    rtl1_core_end();
    clock_ms += RTL1_TAIL_TIMEOUT_MS + 500;
    put_str("[COM] osd done\n[COM] after\n");
    deliver(0);
    CHECK(term_equals("[COM] after\n"));
}

static void test_line_in_progress_when_transfer_starts(void) {
    // A console line half received when a poll starts is shown whole.
    reset();
    put_str("[COM] RT4KPRO, FW");
    deliver(0);
    begin_quiet("osd2", sizeof(out));
    put_str(" Version: 1.87.0\n");
    put_osd2_transfer();
    deliver(1);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(term_equals("[COM] RT4KPRO, FW Version: 1.87.0\n"));
}

// --- uploads (put) -----------------------------------------------------------------------------------

#define PUT_CMD   "put -a 8 0123abcd x.txt"
#define PUT_NONCE 0x6630

static void begin_put_ready(void) {
    rtl1_core_begin_put(PUT_CMD, &info, clock_ms);
    put_str("[COM] put ready nonce=0x6630\n");
    deliver(0);
}

static void test_put_ready_enters_send(void) {
    reset();
    begin_put_ready();
    CHECK(rtl1_core_phase() == RTL1_PH_SEND);
    rtl1_put_state_t s;
    rtl1_core_put_state(&s);
    CHECK(s.nonce == PUT_NONCE);
    CHECK(s.acks == 0 && s.naks == 0);
    CHECK(term_len == 0); // always quiet
}

static void test_put_acks_then_done(void) {
    reset();
    begin_put_ready();
    put_frame(PUT_NONCE, 4, 0, NULL, 0);
    put_frame(PUT_NONCE, 4, 1, NULL, 0);
    deliver(0);
    rtl1_put_state_t s;
    rtl1_core_put_state(&s);
    CHECK(s.acks == 2);
    CHECK(s.last_ack == 1);
    CHECK(rtl1_core_phase() == RTL1_PH_SEND);
    put_str("[COM] put done\n");
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(finished_calls == 1);
    CHECK(term_len == 0);
}

static void test_put_byte_by_byte(void) {
    reset();
    rtl1_core_begin_put(PUT_CMD, &info, clock_ms);
    put_str("[COM] put ready nonce=0x6630\n");
    put_frame(PUT_NONCE, 4, 0, NULL, 0);
    put_frame(PUT_NONCE, 4, 1, NULL, 0);
    put_str("[COM] put done\n");
    deliver(1);
    rtl1_put_state_t s;
    rtl1_core_put_state(&s);
    CHECK(s.acks == 2);
    CHECK(rtl1_core_result() == RTL1_OK);
    CHECK(term_len == 0);
}

static void test_put_usage_refused(void) {
    reset();
    rtl1_core_begin_put(PUT_CMD, &info, clock_ms);
    put_str("[COM] put: usage 'put <size> <sha256hex> <path>'\n");
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
    CHECK(rtl1_core_result() == RTL1_ERR_DEVICE);
    CHECK_STR_HAS(info.detail, "usage");
}

static void test_put_err_refused(void) {
    reset();
    rtl1_core_begin_put(PUT_CMD, &info, clock_ms);
    put_str("[COM] put err: no space\n");
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_ERR_DEVICE);
    CHECK_STR_HAS(info.detail, "no space");
}

static void test_put_nak_recorded(void) {
    reset();
    begin_put_ready();
    const uint8_t reason = 2;
    put_frame(PUT_NONCE, 5, 3, &reason, 1);
    deliver(0);
    rtl1_put_state_t s;
    rtl1_core_put_state(&s);
    CHECK(s.naks == 1);
    CHECK(s.nak_seq == 3);
    CHECK(s.nak_reason == 2);
    CHECK(rtl1_core_phase() == RTL1_PH_SEND); // the sender decides: resend or give up
}

static void test_put_abort_fails(void) {
    reset();
    begin_put_ready();
    put_frame(PUT_NONCE, 6, 4, NULL, 0);
    deliver(0);
    CHECK(rtl1_core_result() == RTL1_ERR_PROTOCOL);
    CHECK(rtl1_core_phase() == RTL1_PH_DRAIN);
    CHECK_STR_HAS(info.detail, "aborted");
}

static void test_put_damaged_ack_ignored(void) {
    reset();
    begin_put_ready();
    put_frame(PUT_NONCE, 4, 0, NULL, 0);
    wire[wire_len - 1] ^= 0xff; // CRC
    put_frame(0x1111, 4, 0, NULL, 0); // another session
    deliver(0);
    rtl1_put_state_t s;
    rtl1_core_put_state(&s);
    CHECK(s.acks == 0);
    CHECK(rtl1_core_phase() == RTL1_PH_SEND);
}

static void test_put_closing_error(void) {
    reset();
    begin_put_ready();
    put_str("[COM] put timeout\n");
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_IDLE);
    CHECK(rtl1_core_result() == RTL1_ERR_DEVICE);
    CHECK_STR_HAS(info.detail, "put timeout");
}

static void test_put_other_lines_reach_terminal(void) {
    reset();
    begin_put_ready();
    put_str("[COM] Serial Remote: menu\n");
    put_frame(PUT_NONCE, 4, 0, NULL, 0);
    deliver(0);
    CHECK(term_len > 0);
    term[term_len] = 0;
    CHECK_STR_HAS((const char *)term, "Serial Remote: menu");
    CHECK(!term_has_binary());
}

static void test_put_no_timeout_while_sending(void) {
    reset();
    begin_put_ready();
    clock_ms += 60000; // a slow sender: the caller owns this timeout
    CHECK(!rtl1_core_poll(clock_ms));
    CHECK(rtl1_core_phase() == RTL1_PH_SEND);
}

static void test_transfer_after_put(void) {
    reset();
    begin_put_ready();
    put_str("[COM] put done\n");
    deliver(0);
    rtl1_core_end();
    begin("osd", sizeof(out));
    put_str("[COM] osd ready rows=32 stride=64 width=40 cells=2048 nonce=0x1234\n");
    deliver(0);
    CHECK(rtl1_core_phase() == RTL1_PH_BINARY); // not an upload any more
}

// --- runner ------------------------------------------------------------------------------------

int main(void) {
    load_fixtures();
    RUN(test_sha256_reference);
    RUN(test_fixture_digest_matches_rt4k);
    RUN(test_crc_matches_rt4k_frames);
    RUN(test_ok_whole_bytes_packets);
    RUN(test_ok_random_chunks);
    RUN(test_ok_with_chatter_around);
    RUN(test_ok_font_closes_with_get_done);
    RUN(test_ok_ignores_noise_between_frames);
    RUN(test_ok_without_closing_line);
    RUN(test_text_passes_through_when_idle);
    RUN(test_refused_nothing_shown);
    RUN(test_refused_with_a_reason);
    RUN(test_ready_without_nonce);
    RUN(test_ready_for_another_command_is_ignored);
    RUN(test_no_ready_line);
    RUN(test_short_ready_timeout);
    RUN(test_stall_mid_transfer);
    RUN(test_bad_crc);
    RUN(test_bad_crc_without_abort);
    RUN(test_wrong_nonce);
    RUN(test_sequence_gap);
    RUN(test_nak);
    RUN(test_more_data_than_buffer);
    RUN(test_digest_mismatch);
    RUN(test_digest_wrong_size);
    RUN(test_unexpected_frame_type);
    RUN(test_drain_gives_up);
    RUN(test_oversized_length_resyncs);
    RUN(test_next_transfer_after_failure);
    RUN(test_quiet_hides_transfer_lines);
    RUN(test_quiet_hides_refusal);
    RUN(test_quiet_keeps_other_lines);
    RUN(test_quiet_then_loud);
    RUN(test_ready_line_lost);
    RUN(test_ready_line_lost_whole);
    RUN(test_ready_line_after_the_wait);
    RUN(test_rest_after_giving_up);
    RUN(test_tail_of_frames_without_control_bytes);
    RUN(test_frames_while_idle);
    RUN(test_transfer_typed_in_terminal);
    RUN(test_closing_line_after_the_wait);
    RUN(test_line_in_progress_when_transfer_starts);
    RUN(test_put_ready_enters_send);
    RUN(test_put_acks_then_done);
    RUN(test_put_byte_by_byte);
    RUN(test_put_usage_refused);
    RUN(test_put_err_refused);
    RUN(test_put_nak_recorded);
    RUN(test_put_abort_fails);
    RUN(test_put_damaged_ack_ignored);
    RUN(test_put_closing_error);
    RUN(test_put_other_lines_reach_terminal);
    RUN(test_put_no_timeout_while_sending);
    RUN(test_transfer_after_put);
    return tests_done();
}
