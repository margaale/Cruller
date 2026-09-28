// Host unit tests for the WebSocket protocol pieces (src/core/ws_proto.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "ws_proto.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)

static void hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

// --- handshake -----------------------------------------------------------------------------------

static void test_sha1_vectors(void) {
    uint8_t d[20];
    char h[41];
    ws_sha1((const uint8_t *)"", 0, d);
    hex(d, 20, h);
    CHECK(!strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    ws_sha1((const uint8_t *)"abc", 3, d);
    hex(d, 20, h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ws_sha1((const uint8_t *)two_blocks, strlen(two_blocks), d);
    hex(d, 20, h);
    CHECK(!strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

static void test_base64_vectors(void) {
    char out[16];
    CHECK(ws_base64((const uint8_t *)"", 0, out) == 0 && !strcmp(out, ""));
    CHECK(ws_base64((const uint8_t *)"f", 1, out) == 4 && !strcmp(out, "Zg=="));
    CHECK(ws_base64((const uint8_t *)"fo", 2, out) == 4 && !strcmp(out, "Zm8="));
    CHECK(ws_base64((const uint8_t *)"foo", 3, out) == 4 && !strcmp(out, "Zm9v"));
    CHECK(ws_base64((const uint8_t *)"foobar", 6, out) == 8 && !strcmp(out, "Zm9vYmFy"));
}

static void test_accept_key_rfc6455(void) {
    char acc[29];
    ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc); // RFC 6455, section 1.3
    CHECK(!strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

// --- server frames -----------------------------------------------------------------------------

static void test_header_short(void) {
    uint8_t h[10];
    CHECK(ws_frame_header(h, WS_OP_BINARY, 125) == 2);
    CHECK(h[0] == 0x82 && h[1] == 125);
    CHECK(ws_frame_header(h, WS_OP_TEXT, 0) == 2);
    CHECK(h[0] == 0x81 && h[1] == 0);
}

static void test_header_16bit(void) {
    uint8_t h[10];
    CHECK(ws_frame_header(h, WS_OP_BINARY, 126) == 4);
    CHECK(h[1] == 126 && h[2] == 0x00 && h[3] == 126);
    CHECK(ws_frame_header(h, WS_OP_BINARY, 4099) == 4);
    CHECK(h[2] == 0x10 && h[3] == 0x03);
    CHECK(ws_frame_header(h, WS_OP_BINARY, 65535) == 4);
    CHECK(h[2] == 0xff && h[3] == 0xff);
}

static void test_header_64bit(void) {
    uint8_t h[10];
    CHECK(ws_frame_header(h, WS_OP_BINARY, 65536) == 10);
    CHECK(h[1] == 127 && h[2] == 0 && h[7] == 1 && h[8] == 0 && h[9] == 0);
}

// --- client frames -----------------------------------------------------------------------------

// Builds a masked client frame.
static size_t client_frame(uint8_t *out, uint8_t first_byte, const uint8_t *payload, size_t len) {
    static const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    size_t p = 0;
    out[p++] = first_byte;
    if (len < 126) {
        out[p++] = (uint8_t)(0x80 | len);
    } else {
        out[p++] = 0x80 | 126;
        out[p++] = (uint8_t)(len >> 8);
        out[p++] = (uint8_t)len;
    }
    memcpy(out + p, mask, 4);
    p += 4;
    for (size_t i = 0; i < len; i++) out[p + i] = payload[i] ^ mask[i & 3];
    return p + len;
}

static void test_parse_rfc_masked_hello(void) {
    uint8_t buf[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}; // RFC 6455, 5.7
    ws_frame_t f;
    CHECK(ws_parse(buf, sizeof(buf), 512, &f) == (long)sizeof(buf));
    CHECK(f.opcode == WS_OP_TEXT && f.len == 5 && !memcmp(f.payload, "Hello", 5));
}

static void test_parse_incomplete(void) {
    uint8_t buf[64];
    const size_t n = client_frame(buf, 0x81, (const uint8_t *)"remote menu", 11);
    ws_frame_t f;
    for (size_t cut = 0; cut < n; cut++) CHECK(ws_parse(buf, cut, 512, &f) == 0);
    CHECK(ws_parse(buf, n, 512, &f) == (long)n);
    CHECK(f.len == 11 && !memcmp(f.payload, "remote menu", 11));
}

static void test_parse_two_frames_in_one_buffer(void) {
    uint8_t buf[64];
    size_t n = client_frame(buf, 0x81, (const uint8_t *)"remote up", 9);
    n += client_frame(buf + n, 0x89, (const uint8_t *)"hi", 2); // ping
    ws_frame_t f;
    const long a = ws_parse(buf, n, 512, &f);
    CHECK(a > 0 && f.opcode == WS_OP_TEXT && f.len == 9 && !memcmp(f.payload, "remote up", 9));
    const long b = ws_parse(buf + a, n - (size_t)a, 512, &f);
    CHECK(b > 0 && (size_t)(a + b) == n && f.opcode == WS_OP_PING && f.len == 2 && !memcmp(f.payload, "hi", 2));
}

static void test_parse_16bit_length(void) {
    uint8_t payload[300], buf[400];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    const size_t n = client_frame(buf, 0x82, payload, sizeof(payload));
    ws_frame_t f;
    CHECK(ws_parse(buf, n, 512, &f) == (long)n);
    CHECK(f.opcode == WS_OP_BINARY && f.len == 300 && !memcmp(f.payload, payload, 300));
}

static void test_parse_close_and_empty(void) {
    uint8_t buf[16];
    const size_t n = client_frame(buf, 0x88, NULL, 0);
    ws_frame_t f;
    CHECK(ws_parse(buf, n, 512, &f) == (long)n);
    CHECK(f.opcode == WS_OP_CLOSE && f.len == 0);
}

static void test_parse_rejects_unmasked(void) {
    uint8_t buf[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    ws_frame_t f;
    CHECK(ws_parse(buf, sizeof(buf), 512, &f) == -1);
}

static void test_parse_rejects_fragments(void) {
    uint8_t buf[32];
    ws_frame_t f;
    size_t n = client_frame(buf, 0x01, (const uint8_t *)"Hel", 3); // FIN=0
    CHECK(ws_parse(buf, n, 512, &f) == -1);
    n = client_frame(buf, 0x80, (const uint8_t *)"lo", 2);          // continuation
    CHECK(ws_parse(buf, n, 512, &f) == -1);
}

static void test_parse_rejects_reserved_bits(void) {
    uint8_t buf[32];
    const size_t n = client_frame(buf, 0xc1, (const uint8_t *)"x", 1); // RSV1 (compression)
    ws_frame_t f;
    CHECK(ws_parse(buf, n, 512, &f) == -1);
}

static void test_parse_rejects_too_large(void) {
    uint8_t payload[300], buf[400];
    memset(payload, 'a', sizeof(payload));
    const size_t n = client_frame(buf, 0x81, payload, sizeof(payload));
    ws_frame_t f;
    CHECK(ws_parse(buf, n, 256, &f) == -1);
    CHECK(ws_parse(buf, 4, 256, &f) == -1); // known from the header alone
}

// --- runner ------------------------------------------------------------------------------------

#define T(fn) {#fn, fn}
static const struct { const char *name; void (*fn)(void); } tests[] = {
    T(test_sha1_vectors),
    T(test_base64_vectors),
    T(test_accept_key_rfc6455),
    T(test_header_short),
    T(test_header_16bit),
    T(test_header_64bit),
    T(test_parse_rfc_masked_hello),
    T(test_parse_incomplete),
    T(test_parse_two_frames_in_one_buffer),
    T(test_parse_16bit_length),
    T(test_parse_close_and_empty),
    T(test_parse_rejects_unmasked),
    T(test_parse_rejects_fragments),
    T(test_parse_rejects_reserved_bits),
    T(test_parse_rejects_too_large),
};

int main(void) {
    const size_t n = sizeof(tests) / sizeof(tests[0]);
    unsigned failed_tests = 0;
    for (size_t i = 0; i < n; i++) {
        current = tests[i].name;
        const int before = failures;
        tests[i].fn();
        const int ok = failures == before;
        if (!ok) failed_tests++;
        printf("%s %s\n", ok ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%u tests, %d checks, %u failed\n", (unsigned)n, checks, failed_tests);
    return failed_tests ? 1 : 0;
}
