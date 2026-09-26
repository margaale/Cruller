#include "rtl1.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/sha256.h"

#include "rt4k.h"

#define MAX_PAYLOAD       2048
#define TYPE_RESPONSE     2
#define TYPE_DATA         3
#define TYPE_NAK          5
#define TYPE_ABORT        6

#define READY_TIMEOUT_MS  3000  // command sent -> ready line
#define STALL_TIMEOUT_MS  3000  // no frame while binary
#define TAIL_TIMEOUT_MS   1000  // after the last frame: waiting for "... done" (or draining)
#define LINK_TIMEOUT_MS   2000

typedef enum {
    PH_IDLE,    // no transfer: text only
    PH_READY,   // command sent, waiting for "<name> ready ... nonce=0x..."
    PH_BINARY,  // frames
    PH_DONE,    // digest checked, waiting for the closing text line
    PH_DRAIN,   // failed or aborted: swallow bytes until the closing text line
} phase_t;

static struct {
    volatile phase_t phase;
    char name[16];              // first word of the command ("osd2", "font", "get")
    uint8_t *out;
    size_t max;
    rtl1_info_t *info;
    uint16_t nonce;
    uint8_t expect_seq;
    rtl1_result_t result;
    volatile uint32_t last_ms;  // last progress, for the timeouts
    char line[192];             // text line being assembled
    size_t line_len;
    // frame decoder
    int st;
    uint16_t f_nonce, f_len, f_idx, crc, rx_crc;
    uint8_t f_type, f_seq;
    uint8_t payload[MAX_PAYLOAD];
} e;

static SemaphoreHandle_t feed_lock; // rtl1_feed() and the caller's phase changes
static SemaphoreHandle_t done_sem;  // given when a transfer ends
static SemaphoreHandle_t xfer_lock; // one transfer at a time

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint16_t crc16(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

static void set_detail(const char *fmt, ...) {
    if (!e.info) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.info->detail, sizeof(e.info->detail), fmt, ap);
    va_end(ap);
}

static void finish(void) {
    e.phase = PH_IDLE;
    xSemaphoreGive(done_sem);
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
        uint8_t f[10] = {0xa5, 0x5a, (uint8_t)e.nonce, (uint8_t)(e.nonce >> 8), 0, 0, TYPE_ABORT, e.expect_seq, 0, 0};
        uint16_t crc = 0xffff;
        for (int i = 2; i < 8; i++) crc = crc16(crc, f[i]);
        f[8] = (uint8_t)crc;
        f[9] = (uint8_t)(crc >> 8);
        rt4k_write(f, sizeof(f));
    }
    e.phase = PH_DRAIN;
    e.last_ms = now_ms();
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
        case PH_READY:
            if (!strncmp(line, e.name, name_len) && !strncmp(line + name_len, " ready", 6)) {
                const char *n = strstr(line, "nonce=0x");
                if (!n) {
                    e.result = RTL1_ERR_PROTOCOL;
                    set_detail("ready line without a nonce");
                    finish();
                    return;
                }
                e.nonce = (uint16_t)strtoul(n + 8, NULL, 16);
                if (e.info) snprintf(e.info->ready, sizeof(e.info->ready), "%s", line);
                e.expect_seq = 0;
                e.st = 0;
                e.phase = PH_BINARY;
                e.last_ms = now_ms();
            } else if (contains_ci(line, "busy") || contains_ci(line, "bad command") ||
                       contains_ci(line, "unknown command") || contains_ci(line, "nothing shown") ||
                       contains_ci(line, "error") || contains_ci(line, "failed")) {
                e.result = RTL1_ERR_DEVICE;
                set_detail("%s", line);
                finish();
            }
            break;
        case PH_DONE:
        case PH_DRAIN:
            if (ends_with(line, " done") || contains_ci(line, "aborted") || contains_ci(line, "failed")) finish();
            break;
        default:
            break;
    }
}

static void on_frame(void) {
    e.last_ms = now_ms();
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
        pico_sha256_state_t sha;
        sha256_result_t digest;
        if (pico_sha256_start_blocking(&sha, SHA256_BIG_ENDIAN, false) != PICO_OK) {
            return fail_binary(false, "SHA-256 engine unavailable");
        }
        pico_sha256_update_blocking(&sha, e.out, e.info->len);
        pico_sha256_finish(&sha, &digest);
        if (memcmp(digest.bytes, e.payload, 32) != 0) return fail_binary(false, "SHA-256 mismatch over %u bytes", (unsigned)e.info->len);
        e.expect_seq++;
        e.result = RTL1_OK;
        e.phase = PH_DONE;
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
            e.st = e.f_len > MAX_PAYLOAD ? (b == 0xa5 ? 1 : 0) : 6;
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

void rtl1_feed(const uint8_t *data, size_t len) {
    xSemaphoreTake(feed_lock, portMAX_DELAY);
    size_t text_from = 0; // start of the pending run of terminal text
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        const phase_t ph = e.phase;
        if (ph == PH_BINARY) {
            decode(b);
            if (e.phase != PH_BINARY) text_from = i + 1; // back to text after the last frame
            continue;
        }
        if (ph == PH_DRAIN || ph == PH_DONE) e.last_ms = now_ms();
        if (b == '\n') {
            e.line[e.line_len] = 0;
            if (e.line_len && e.line[e.line_len - 1] == '\r') e.line[e.line_len - 1] = 0;
            e.line_len = 0;
            on_line(e.line);
            if (e.phase == PH_BINARY) {
                // The ready line goes to the terminal; the frames right after it don't.
                if (ph != PH_DRAIN) rt4k_text_push(data + text_from, i + 1 - text_from);
                text_from = i + 1;
            }
        } else if (e.line_len < sizeof(e.line) - 1) {
            e.line[e.line_len++] = (char)b;
        }
        if (ph == PH_DRAIN) text_from = i + 1; // drained bytes never reach the terminal
    }
    if (e.phase != PH_BINARY && e.phase != PH_DRAIN && text_from < len) rt4k_text_push(data + text_from, len - text_from);
    xSemaphoreGive(feed_lock);
}

void rtl1_init(void) {
    feed_lock = xSemaphoreCreateMutex();
    done_sem = xSemaphoreCreateBinary();
    xfer_lock = xSemaphoreCreateMutex();
}

rtl1_result_t rtl1_transfer(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (!rt4k_connected()) {
        snprintf(info->detail, sizeof(info->detail), "RT4K not connected");
        return RTL1_ERR_NO_LINK;
    }
    if (xSemaphoreTake(xfer_lock, pdMS_TO_TICKS(LINK_TIMEOUT_MS)) != pdTRUE) {
        snprintf(info->detail, sizeof(info->detail), "another transfer is running");
        return RTL1_ERR_NO_LINK;
    }
    if (!rt4k_link_lock(LINK_TIMEOUT_MS)) {
        xSemaphoreGive(xfer_lock);
        snprintf(info->detail, sizeof(info->detail), "link busy");
        return RTL1_ERR_NO_LINK;
    }

    xSemaphoreTake(feed_lock, portMAX_DELAY);
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
    e.last_ms = now_ms();
    xSemaphoreTake(done_sem, 0); // stale
    e.phase = PH_READY;
    xSemaphoreGive(feed_lock);

    char line[200];
    const int len = snprintf(line, sizeof(line), "\r%s\r\n", cmd);
    const uint32_t start = now_ms();
    if (len <= 0 || len >= (int)sizeof(line) || !rt4k_write(line, (size_t)len)) {
        xSemaphoreTake(feed_lock, portMAX_DELAY);
        e.phase = PH_IDLE;
        xSemaphoreGive(feed_lock);
        snprintf(info->detail, sizeof(info->detail), "could not queue the command");
        rt4k_link_unlock();
        xSemaphoreGive(xfer_lock);
        return RTL1_ERR_NO_LINK;
    }

    while (xSemaphoreTake(done_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
        xSemaphoreTake(feed_lock, portMAX_DELAY);
        const uint32_t t = now_ms();
        bool over = false;
        switch (e.phase) {
            case PH_READY:
                if (t - start > READY_TIMEOUT_MS) {
                    set_detail("no ready line from the RT4K");
                    over = true;
                }
                break;
            case PH_BINARY:
                if (t - e.last_ms > STALL_TIMEOUT_MS) fail_binary(true, "transfer stalled after %u bytes", (unsigned)info->len);
                break;
            case PH_DONE:
            case PH_DRAIN:
                if (t - e.last_ms > TAIL_TIMEOUT_MS) over = true; // data already verified, or given up
                break;
            default:
                over = true;
                break;
        }
        if (over) e.phase = PH_IDLE;
        xSemaphoreGive(feed_lock);
        if (over) break;
    }

    xSemaphoreTake(feed_lock, portMAX_DELAY);
    const rtl1_result_t result = e.result;
    e.phase = PH_IDLE;
    e.info = NULL;
    e.out = NULL;
    xSemaphoreGive(feed_lock);
    rt4k_link_unlock();
    xSemaphoreGive(xfer_lock);
    return result;
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
