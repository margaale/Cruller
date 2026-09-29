#include "rtl1_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TYPE_RESPONSE 2
#define TYPE_DATA     3
#define TYPE_ACK      4
#define TYPE_NAK      5
#define TYPE_ABORT    6

// What the RT4K may still send of a transfer given up on (rtl1_core_poll's timeouts).
#define OWED_NONE  0
#define OWED_READY 1            // its ready line (and frames): it came after the wait for it
#define OWED_CLOSE 2            // its closing line

static const rtl1_hooks_t *hooks;

static struct {
    rtl1_phase_t phase;
    bool quiet;                 // this transfer's text stays out of the terminal (kept after it: see owed)
    char name[16];              // first word of the command ("osd2", "font", "get")
    int owed;                   // OWED_*
    uint8_t *out;
    size_t max;
    rtl1_info_t *info;
    uint16_t nonce;
    uint8_t expect_seq;
    bool put;                   // an upload (rtl1_core_begin_put)
    rtl1_put_state_t replies;   // the RT4K's ACK/NAK frames during an upload
    rtl1_result_t result;
    uint32_t start_ms;          // command sent
    uint32_t ready_timeout_ms;
    uint32_t last_ms;           // last progress
    uint32_t now;               // time of the bytes being fed
    char line[192];             // text line being assembled: it goes on whole, once judged
    size_t line_len;
    bool line_bad;              // it had a control character: not console text, never shown
    bool line_out;              // its start went on already (longer than line[])
    uint8_t prev;               // the last text byte
    uint8_t after_magic;        // text bytes since an A5 5A (0: none lately)
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
    if (send_abort && hooks->abort_on_error) {
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

// A transfer's last line: "osd done", "get done" (font), or "... aborted" / "... failed".
static bool closing_line(const char *line) {
    return ends_with(line, " done") || contains_ci(line, "aborted") || contains_ci(line, "failed");
}

// "<command> ready ... nonce=0x<hex>": binary frames follow.
static bool ready_line(const char *line) {
    const size_t w = strcspn(line, " ");
    return w && !strncmp(line + w, " ready", 6) && strstr(line, "nonce=0x");
}

// The end of a transfer's frames with its closing line glued on ("...[COM] osd done"): console lines
// start with "[COM] ", so what comes before it is binary.
static bool frames_tail(const char *line) {
    const char *com = strstr(line, "[COM] ");
    return com && com != line && closing_line(com + 6);
}

// Never in console text. Every frame header has some: the length's high byte (0-8) and the type (1-7).
static bool not_text(uint8_t b) {
    return b < 0x20 && b != '\t' && b != '\r' && b != '\n';
}

// Returns true when the line belongs to the transfer (its ready, refusal or closing line).
static bool on_line(char *line) {
    if (!strncmp(line, "[COM] ", 6)) line += 6;
    const size_t name_len = strlen(e.name);
    switch (e.phase) {
        case RTL1_PH_IDLE: {
            // Nobody waits, but a transfer's lines still come: a ready line (late, after its transfer
            // gave up waiting; or a command typed in the terminal) starts a drain of its frames, and
            // the closing line of one that gave up waiting for it is still its own.
            const bool ours = !strncmp(line, e.name, name_len) && line[name_len] == ' ';
            if (ready_line(line)) {
                const bool late = e.owed == OWED_READY && ours;
                e.owed = OWED_NONE;
                e.phase = RTL1_PH_DRAIN;
                e.last_ms = e.now;
                return late;
            }
            if (e.owed == OWED_CLOSE && closing_line(line)) {
                e.owed = OWED_NONE;
                return true;
            }
            return false;
        }
        case RTL1_PH_READY:
            if (!strncmp(line, e.name, name_len) && !strncmp(line + name_len, " ready", 6)) {
                const char *n = strstr(line, "nonce=0x");
                if (!n) {
                    e.result = RTL1_ERR_PROTOCOL;
                    set_detail("ready line without a nonce");
                    finish();
                    return true;
                }
                e.nonce = (uint16_t)strtoul(n + 8, NULL, 16);
                if (e.info) {
                    const size_t len = strnlen(line, sizeof(e.info->ready) - 1);
                    memcpy(e.info->ready, line, len);
                    e.info->ready[len] = 0;
                }
                e.expect_seq = 0;
                e.st = 0;
                e.replies.nonce = e.nonce;
                e.phase = e.put ? RTL1_PH_SEND : RTL1_PH_BINARY;
                e.last_ms = e.now;
                return true;
            }
            // "put: usage ...", "put err: ...": the command's own refusal.
            const bool own_refusal = !strncmp(line, e.name, name_len) &&
                (line[name_len] == ':' || !strncmp(line + name_len, " err", 4));
            if (own_refusal || contains_ci(line, "busy") || contains_ci(line, "bad command") ||
                contains_ci(line, "unknown command") || contains_ci(line, "nothing shown") ||
                contains_ci(line, "error") || contains_ci(line, "failed")) {
                e.result = RTL1_ERR_DEVICE;
                set_detail("%s", line);
                finish();
                return true;
            }
            return false;
        case RTL1_PH_DONE:
        case RTL1_PH_DRAIN:
            if (closing_line(line)) {
                finish();
                return true;
            }
            return false;
        case RTL1_PH_SEND:
            // The closing line: "put done", or why not ("put: ...", "put err: ...", "put timeout").
            if (strncmp(line, e.name, name_len) || (line[name_len] != ' ' && line[name_len] != ':')) return false;
            if (!strcmp(line + name_len, " done")) {
                e.result = RTL1_OK;
            } else {
                e.result = RTL1_ERR_DEVICE;
                set_detail("%s", line);
            }
            finish();
            return true;
        default:
            return false;
    }
}

// Upload: the RT4K acknowledges each frame (put -a), refuses one, or gives up.
static void on_reply_frame(void) {
    if (e.crc != e.rx_crc || e.f_nonce != e.nonce) return; // damaged or stale: the sender retries on timeout
    if (e.f_type == TYPE_ACK) {
        e.replies.last_ack = e.f_seq;
        e.replies.acks++;
    } else if (e.f_type == TYPE_NAK) {
        e.replies.nak_seq = e.f_seq;
        e.replies.nak_reason = e.f_len ? e.payload[0] : 0;
        e.replies.naks++;
    } else if (e.f_type == TYPE_ABORT) {
        e.result = RTL1_ERR_PROTOCOL;
        set_detail("the RT4K aborted the upload at frame %u", e.f_seq);
        e.phase = RTL1_PH_DRAIN;
        e.last_ms = e.now;
    }
}

static void on_frame(void) {
    e.last_ms = e.now;
    if (e.phase == RTL1_PH_SEND) return on_reply_frame();
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

// A drain ending without its closing line: what it had of a line is binary leftovers.
static void drop_leftovers(void) {
    e.line_len = 0;
    e.line_bad = false;
}

// A frame header where text was expected: frames whose ready line was lost (bytes the FT232R
// dropped) or came after the wait for it, or more of a transfer given up on. Drained to the closing line.
static void frames_outside_transfer(rtl1_phase_t ph) {
    if (ph == RTL1_PH_READY) {
        e.result = RTL1_ERR_PROTOCOL;
        set_detail("frames without a ready line");
    }
    e.phase = RTL1_PH_DRAIN;
    e.last_ms = e.now;
}

// A whole text line: to the transfer (on_line), then to the terminal unless it's hidden. A quiet
// transfer's own lines are; other lines (replies to terminal commands, console messages) still show.
static void end_line(rtl1_phase_t ph) {
    if (e.line_len && e.line[e.line_len - 1] == '\r') e.line_len--;
    e.line[e.line_len] = 0;
    const size_t len = e.line_len;
    const bool bad = e.line_bad;
    e.line_len = 0;
    e.line_bad = false;
    const bool own = on_line(e.line);
    if (ph != RTL1_PH_DRAIN && !bad && !(own && e.quiet) && !frames_tail(e.line)) {
        e.line[len] = '\n';
        hooks->text((const uint8_t *)e.line, len + 1);
    } else if (e.line_out) {
        hooks->text((const uint8_t *)"\n", 1); // ends the part that went on
    }
    e.line_out = false;
}

// Text reaches the terminal a whole line at a time (console.c routes whole lines anyway), so a line
// that turns out to be binary is never shown, not even its first bytes ("A5 5A" and the nonce, before
// the frame header's first control character).
static void text_byte(uint8_t b) {
    const rtl1_phase_t ph = e.phase;
    if (ph == RTL1_PH_DRAIN || ph == RTL1_PH_DONE) e.last_ms = e.now;
    const bool after_magic = e.after_magic != 0;
    e.after_magic = e.prev == 0xa5 && b == 0x5a ? 1 : after_magic && e.after_magic < 6 ? e.after_magic + 1 : 0;
    e.prev = b;
    if (b == '\n') {
        end_line(ph);
    } else if (not_text(b) || (ph == RTL1_PH_DRAIN && b >= 0x80)) {
        // Binary: the line is never shown, and starts over so a closing line glued to the end of the
        // frames is still seen whole.
        e.line_len = 0;
        e.line_bad = true;
        if (after_magic && (ph == RTL1_PH_IDLE || ph == RTL1_PH_READY || ph == RTL1_PH_DONE)) {
            frames_outside_transfer(ph);
        }
    } else {
        if (e.line_len >= sizeof(e.line) - 2) { // overlong: no transfer's line, pass on what we have
            if (ph != RTL1_PH_DRAIN && !e.line_bad) {
                hooks->text((const uint8_t *)e.line, e.line_len);
                e.line_out = true;
            }
            e.line_len = 0;
        }
        e.line[e.line_len++] = (char)b;
    }
}

void rtl1_core_feed(const uint8_t *data, size_t len, uint32_t now_ms) {
    e.now = now_ms;
    if (e.phase == RTL1_PH_DRAIN && !e.info && now_ms - e.last_ms > RTL1_TAIL_TIMEOUT_MS) {
        drop_leftovers(); // a drain nobody waits for went quiet: its closing line isn't coming
        e.phase = RTL1_PH_IDLE;
    }
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        if (e.phase == RTL1_PH_BINARY || (e.phase == RTL1_PH_SEND && (e.st != 0 || b == 0xa5))) {
            decode(b); // frames; while uploading, a reply frame (text lines never contain 0xa5)
        } else {
            text_byte(b);
        }
    }
}

void rtl1_core_init(const rtl1_hooks_t *h) {
    hooks = h;
    memset(&e, 0, sizeof(e));
}

void rtl1_core_begin(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info, bool quiet,
    uint32_t ready_timeout_ms, uint32_t now_ms) {
    e.ready_timeout_ms = ready_timeout_ms ? ready_timeout_ms : RTL1_READY_TIMEOUT_MS;
    e.quiet = quiet;
    size_t n = strcspn(cmd, " ");
    if (n >= sizeof(e.name)) n = sizeof(e.name) - 1;
    memcpy(e.name, cmd, n);
    e.name[n] = 0;
    e.out = out;
    e.max = max;
    e.info = info;
    e.nonce = 0;
    e.put = false;
    e.owed = OWED_NONE;
    e.result = RTL1_ERR_TIMEOUT;
    if (e.phase == RTL1_PH_DRAIN) drop_leftovers(); // a text line in progress goes on
    e.start_ms = e.last_ms = e.now = now_ms;
    e.phase = RTL1_PH_READY;
}

void rtl1_core_begin_put(const char *cmd, rtl1_info_t *info, uint32_t ready_timeout_ms, uint32_t now_ms) {
    rtl1_core_begin(cmd, NULL, 0, info, true, ready_timeout_ms, now_ms);
    e.put = true;
    memset(&e.replies, 0, sizeof(e.replies));
    e.st = 0;
}

void rtl1_core_put_state(rtl1_put_state_t *out) {
    *out = e.replies;
}

bool rtl1_core_poll(uint32_t now_ms) {
    e.now = now_ms;
    switch (e.phase) {
        case RTL1_PH_READY:
            if (now_ms - e.start_ms > e.ready_timeout_ms) {
                set_detail("no ready line from the RT4K");
                e.phase = RTL1_PH_IDLE;
                e.owed = OWED_READY;
            }
            break;
        case RTL1_PH_BINARY:
            if (now_ms - e.last_ms > RTL1_STALL_TIMEOUT_MS) {
                fail_binary(true, "transfer stalled after %u bytes", e.info ? (unsigned)e.info->len : 0u);
            }
            break;
        case RTL1_PH_DONE:
        case RTL1_PH_DRAIN:
            if (now_ms - e.last_ms > RTL1_TAIL_TIMEOUT_MS) { // verified, or given up
                if (e.phase == RTL1_PH_DRAIN) drop_leftovers();
                e.phase = RTL1_PH_IDLE;
                e.owed = OWED_CLOSE;
            }
            break;
        default:
            break;
    }
    return e.phase == RTL1_PH_IDLE;
}

void rtl1_core_end(void) {
    if (e.phase != RTL1_PH_DRAIN) e.phase = RTL1_PH_IDLE; // a drain goes on without the caller
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
