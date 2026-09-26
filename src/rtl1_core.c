#include "rtl1_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TYPE_RESPONSE 2
#define TYPE_DATA     3
#define TYPE_NAK      5
#define TYPE_ABORT    6

static const rtl1_hooks_t *hooks;

static struct {
    rtl1_phase_t phase;
    char name[16];              // first word of the command ("osd2", "font", "get")
    uint8_t *out;
    size_t max;
    rtl1_info_t *info;
    uint16_t nonce;
    uint8_t expect_seq;
    rtl1_result_t result;
    uint32_t start_ms;          // command sent
    uint32_t last_ms;           // last progress
    uint32_t now;               // time of the bytes being fed
    char line[192];             // text line being assembled
    size_t line_len;
    // frame decoder
    int st;
    uint16_t f_nonce, f_len, f_idx, crc, rx_crc;
    uint8_t f_type, f_seq;
    uint8_t payload[RTL1_MAX_PAYLOAD];
} e;

static uint16_t crc16(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

size_t rtl1_encode_frame(uint8_t *out, uint16_t nonce, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len) {
    out[0] = 0xa5;
    out[1] = 0x5a;
    out[2] = (uint8_t)nonce;
    out[3] = (uint8_t)(nonce >> 8);
    out[4] = (uint8_t)len;
    out[5] = (uint8_t)(len >> 8);
    out[6] = type;
    out[7] = seq;
    if (len) memcpy(out + 8, payload, len);
    uint16_t crc = 0xffff;
    for (size_t i = 2; i < 8u + len; i++) crc = crc16(crc, out[i]);
    out[8 + len] = (uint8_t)crc;
    out[9 + len] = (uint8_t)(crc >> 8);
    return 10u + len;
}

static void set_detail(const char *fmt, ...) {
    if (!e.info) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.info->detail, sizeof(e.info->detail), fmt, ap);
    va_end(ap);
}

static void finish(void) {
    e.phase = RTL1_PH_IDLE;
    if (hooks->finished) hooks->finished();
}

// Stops a transfer that went wrong: tell the RT4K (when it's still sending) and swallow the rest.
static void fail_binary(bool send_abort, const char *fmt, ...) {
    e.result = RTL1_ERR_PROTOCOL;
    if (e.info) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e.info->detail, sizeof(e.info->detail), fmt, ap);
        va_end(ap);
    }
    if (send_abort) {
        uint8_t f[10];
        rtl1_encode_frame(f, e.nonce, TYPE_ABORT, e.expect_seq, NULL, 0);
        hooks->write(f, sizeof(f));
    }
    e.phase = RTL1_PH_DRAIN;
    e.last_ms = e.now;
}

static bool contains_ci(const char *s, const char *word) {
    const size_t n = strlen(word);
    for (; *s; s++) {
        if (strncasecmp(s, word, n) == 0) return true;
    }
    return false;
}

static bool ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static void on_line(char *line) {
    if (!strncmp(line, "[COM] ", 6)) line += 6;
    const size_t name_len = strlen(e.name);
    switch (e.phase) {
        case RTL1_PH_READY:
            if (!strncmp(line, e.name, name_len) && !strncmp(line + name_len, " ready", 6)) {
                const char *n = strstr(line, "nonce=0x");
                if (!n) {
                    e.result = RTL1_ERR_PROTOCOL;
                    set_detail("ready line without a nonce");
                    finish();
                    return;
                }
                e.nonce = (uint16_t)strtoul(n + 8, NULL, 16);
                if (e.info) {
                    const size_t n = strnlen(line, sizeof(e.info->ready) - 1);
                    memcpy(e.info->ready, line, n);
                    e.info->ready[n] = 0;
                }
                e.expect_seq = 0;
                e.st = 0;
                e.phase = RTL1_PH_BINARY;
                e.last_ms = e.now;
            } else if (contains_ci(line, "busy") || contains_ci(line, "bad command") ||
                       contains_ci(line, "unknown command") || contains_ci(line, "nothing shown") ||
                       contains_ci(line, "error") || contains_ci(line, "failed")) {
                e.result = RTL1_ERR_DEVICE;
                set_detail("%s", line);
                finish();
            }
            break;
        case RTL1_PH_DONE:
        case RTL1_PH_DRAIN:
            if (ends_with(line, " done") || contains_ci(line, "aborted") || contains_ci(line, "failed")) finish();
            break;
        default:
            break;
    }
}

static void on_frame(void) {
    e.last_ms = e.now;
    if (e.crc != e.rx_crc) return fail_binary(true, "bad CRC on frame %u", e.f_seq);
    if (e.f_nonce != e.nonce) return fail_binary(true, "frame for another session (nonce 0x%04x)", e.f_nonce);
    if (e.f_type == TYPE_NAK) return fail_binary(false, "RT4K NAK, reason %u", e.f_len ? e.payload[0] : 0);
    if (e.f_seq != e.expect_seq) return fail_binary(true, "sequence gap: got %u, expected %u", e.f_seq, e.expect_seq);
    if (e.f_type == TYPE_DATA) {
        if (e.info->len + e.f_len > e.max) return fail_binary(true, "more data than the %u-byte buffer", (unsigned)e.max);
        memcpy(e.out + e.info->len, e.payload, e.f_len);
        e.info->len += e.f_len;
        e.expect_seq++;
    } else if (e.f_type == TYPE_RESPONSE) {
        if (e.f_len != 32) return fail_binary(true, "digest of %u bytes", e.f_len);
        uint8_t digest[32];
        if (!hooks->sha256(e.out, e.info->len, digest)) return fail_binary(false, "SHA-256 engine unavailable");
        if (memcmp(digest, e.payload, 32) != 0) return fail_binary(false, "SHA-256 mismatch over %u bytes", (unsigned)e.info->len);
        e.expect_seq++;
        e.result = RTL1_OK;
        e.phase = RTL1_PH_DONE;
    } else {
        fail_binary(true, "unexpected frame type %u", e.f_type);
    }
}

static void decode(uint8_t b) {
    switch (e.st) {
        case 0: e.st = b == 0xa5 ? 1 : 0; break;
        case 1:
            if (b == 0x5a) {
                e.st = 2;
                e.crc = 0xffff;
            } else {
                e.st = b == 0xa5 ? 1 : 0;
            }
            break;
        case 2: e.f_nonce = b; e.crc = crc16(e.crc, b); e.st = 3; break;
        case 3: e.f_nonce |= (uint16_t)(b << 8); e.crc = crc16(e.crc, b); e.st = 4; break;
        case 4: e.f_len = b; e.crc = crc16(e.crc, b); e.st = 5; break;
        case 5:
            e.f_len |= (uint16_t)(b << 8);
            e.crc = crc16(e.crc, b);
            e.st = e.f_len > RTL1_MAX_PAYLOAD ? (b == 0xa5 ? 1 : 0) : 6;
            break;
        case 6: e.f_type = b; e.crc = crc16(e.crc, b); e.st = 7; break;
        case 7:
            e.f_seq = b;
            e.crc = crc16(e.crc, b);
            e.f_idx = 0;
            e.st = e.f_len ? 8 : 9;
            break;
        case 8:
            e.payload[e.f_idx++] = b;
            e.crc = crc16(e.crc, b);
            if (e.f_idx >= e.f_len) e.st = 9;
            break;
        case 9: e.rx_crc = b; e.st = 10; break;
        case 10:
            e.rx_crc |= (uint16_t)(b << 8);
            e.st = 0;
            on_frame();
            break;
    }
}

void rtl1_core_feed(const uint8_t *data, size_t len, uint32_t now_ms) {
    e.now = now_ms;
    size_t text_from = 0; // start of the pending run of terminal text
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        const rtl1_phase_t ph = e.phase;
        if (ph == RTL1_PH_BINARY) {
            decode(b);
            if (e.phase != RTL1_PH_BINARY) text_from = i + 1; // back to text after the last frame
            continue;
        }
        if (ph == RTL1_PH_DRAIN || ph == RTL1_PH_DONE) e.last_ms = now_ms;
        if (b == '\n') {
            e.line[e.line_len] = 0;
            if (e.line_len && e.line[e.line_len - 1] == '\r') e.line[e.line_len - 1] = 0;
            e.line_len = 0;
            on_line(e.line);
            if (e.phase == RTL1_PH_BINARY) {
                // The ready line goes to the terminal; the frames right after it don't.
                hooks->text(data + text_from, i + 1 - text_from);
                text_from = i + 1;
            }
        } else if (ph == RTL1_PH_DRAIN && (b >= 0x80 || (b < 0x20 && b != '\r' && b != '\t'))) {
            e.line_len = 0; // binary leftovers: start the line over so the closing line is seen whole
        } else {
            if (e.line_len >= sizeof(e.line) - 1) e.line_len = 0; // overlong: keep the latest text
            e.line[e.line_len++] = (char)b;
        }
        if (ph == RTL1_PH_DRAIN) text_from = i + 1; // drained bytes never reach the terminal
    }
    if (e.phase != RTL1_PH_BINARY && e.phase != RTL1_PH_DRAIN && text_from < len) hooks->text(data + text_from, len - text_from);
}

void rtl1_core_init(const rtl1_hooks_t *h) {
    hooks = h;
    memset(&e, 0, sizeof(e));
}

void rtl1_core_begin(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info, uint32_t now_ms) {
    size_t n = strcspn(cmd, " ");
    if (n >= sizeof(e.name)) n = sizeof(e.name) - 1;
    memcpy(e.name, cmd, n);
    e.name[n] = 0;
    e.out = out;
    e.max = max;
    e.info = info;
    e.nonce = 0;
    e.result = RTL1_ERR_TIMEOUT;
    e.line_len = 0;
    e.start_ms = e.last_ms = e.now = now_ms;
    e.phase = RTL1_PH_READY;
}

bool rtl1_core_poll(uint32_t now_ms) {
    e.now = now_ms;
    switch (e.phase) {
        case RTL1_PH_READY:
            if (now_ms - e.start_ms > RTL1_READY_TIMEOUT_MS) {
                set_detail("no ready line from the RT4K");
                e.phase = RTL1_PH_IDLE;
            }
            break;
        case RTL1_PH_BINARY:
            if (now_ms - e.last_ms > RTL1_STALL_TIMEOUT_MS) {
                fail_binary(true, "transfer stalled after %u bytes", e.info ? (unsigned)e.info->len : 0u);
            }
            break;
        case RTL1_PH_DONE:
        case RTL1_PH_DRAIN:
            if (now_ms - e.last_ms > RTL1_TAIL_TIMEOUT_MS) e.phase = RTL1_PH_IDLE; // verified, or given up
            break;
        default:
            break;
    }
    return e.phase == RTL1_PH_IDLE;
}

void rtl1_core_end(void) {
    e.phase = RTL1_PH_IDLE;
    e.info = NULL;
    e.out = NULL;
}

rtl1_phase_t rtl1_core_phase(void) {
    return e.phase;
}

rtl1_result_t rtl1_core_result(void) {
    return e.result;
}

const char *rtl1_result_name(rtl1_result_t r) {
    switch (r) {
        case RTL1_OK: return "ok";
        case RTL1_ERR_NO_LINK: return "no link";
        case RTL1_ERR_TIMEOUT: return "timeout";
        case RTL1_ERR_DEVICE: return "refused by the RT4K";
        case RTL1_ERR_PROTOCOL: return "protocol error";
    }
    return "?";
}
