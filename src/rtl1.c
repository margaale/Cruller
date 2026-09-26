// FreeRTOS side of the RTL1 engine: locking and the blocking transfer API around rtl1_core.

#include "rtl1.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/sha256.h"

#include "rt4k.h"
#include "rtl1_core.h"

#define LINK_TIMEOUT_MS 2000
// The RT4K ignores a transfer request sent right behind a console command: with no gap, 10 of 10 polls
// after a key press got no ready line (and no FT232R overrun, so nothing was lost on our side).
// Enforced here, after taking the link, so no caller can race past it. Back-to-back transfers need no
// gap (a 40 ms one covered the FT232R overruns, fixed since).
#define AFTER_COMMAND_MS 100

static volatile uint32_t paused_until_ms; // debug: no transfers while raw bytes go out (POST /debug/raw)

static SemaphoreHandle_t feed_lock; // every rtl1_core call
static SemaphoreHandle_t done_sem;  // given when a transfer ends
static SemaphoreHandle_t xfer_lock; // one transfer at a time

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static bool hw_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    pico_sha256_state_t sha;
    sha256_result_t digest;
    if (pico_sha256_start_blocking(&sha, SHA256_BIG_ENDIAN, false) != PICO_OK) return false;
    pico_sha256_update_blocking(&sha, data, len);
    pico_sha256_finish(&sha, &digest);
    memcpy(out, digest.bytes, 32);
    return true;
}

static void finished(void) {
    xSemaphoreGive(done_sem);
}

static const rtl1_hooks_t hooks = {
    .write = rt4k_write,
    .text = rt4k_text_push,
    .sha256 = hw_sha256,
    .finished = finished,
    .abort_on_error = false, // over USB the RT4K streams without waiting: an ABORT arrives after the
                             // transfer and the RT4K reads it as a bad text command
};

// Debug: the raw bytes of the current transfer, kept when it fails (GET /debug/lastfail).
#define CAPTURE_MAX 6144
static uint8_t capture[CAPTURE_MAX], last_fail[CAPTURE_MAX];
static size_t capture_len, last_fail_len;
static volatile bool capturing;

void rtl1_pause(uint32_t ms) {
    paused_until_ms = now_ms() + ms;
}

size_t rtl1_last_failure(const uint8_t **data) {
    *data = last_fail;
    return last_fail_len;
}

void rtl1_init(void) {
    feed_lock = xSemaphoreCreateMutex();
    done_sem = xSemaphoreCreateBinary();
    xfer_lock = xSemaphoreCreateMutex();
    rtl1_core_init(&hooks);
}

void rtl1_feed(const uint8_t *data, size_t len) {
    xSemaphoreTake(feed_lock, portMAX_DELAY);
    if (capturing) {
        const size_t n = len < CAPTURE_MAX - capture_len ? len : CAPTURE_MAX - capture_len;
        memcpy(capture + capture_len, data, n);
        capture_len += n;
    }
    rtl1_core_feed(data, len, now_ms());
    xSemaphoreGive(feed_lock);
}

rtl1_result_t rtl1_transfer(const char *cmd, uint8_t *out, size_t max, rtl1_info_t *info, bool quiet,
    uint32_t ready_timeout_ms) {
    memset(info, 0, sizeof(*info));
    if (!rt4k_connected()) {
        snprintf(info->detail, sizeof(info->detail), "RT4K not connected");
        return RTL1_ERR_NO_LINK;
    }
    if ((int32_t)(paused_until_ms - now_ms()) > 0) {
        snprintf(info->detail, sizeof(info->detail), "paused (raw access)");
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
    const uint32_t since_cmd = rt4k_ms_since_command();
    if (since_cmd < AFTER_COMMAND_MS) vTaskDelay(pdMS_TO_TICKS(AFTER_COMMAND_MS - since_cmd));

    xSemaphoreTake(feed_lock, portMAX_DELAY);
    xSemaphoreTake(done_sem, 0); // stale
    rtl1_core_begin(cmd, out, max, info, quiet, ready_timeout_ms, now_ms());
    capture_len = 0;
    capturing = true;
    xSemaphoreGive(feed_lock);

    char line[200];
    const int len = snprintf(line, sizeof(line), "\r%s\r\n", cmd);
    rtl1_result_t result;
    if (len <= 0 || len >= (int)sizeof(line) || !rt4k_write(line, (size_t)len)) {
        snprintf(info->detail, sizeof(info->detail), "could not queue the command");
        result = RTL1_ERR_NO_LINK;
    } else {
        // Wake on the end of the transfer, or every 50 ms for the timeouts.
        for (;;) {
            const bool signalled = xSemaphoreTake(done_sem, pdMS_TO_TICKS(50)) == pdTRUE;
            xSemaphoreTake(feed_lock, portMAX_DELAY);
            const bool over = rtl1_core_poll(now_ms());
            xSemaphoreGive(feed_lock);
            if (signalled || over) break;
        }
        result = rtl1_core_result();
        xSemaphoreTake(feed_lock, portMAX_DELAY);
        capturing = false;
        if (result == RTL1_ERR_PROTOCOL) {
            memcpy(last_fail, capture, capture_len);
            last_fail_len = capture_len;
        }
        xSemaphoreGive(feed_lock);
    }

    xSemaphoreTake(feed_lock, portMAX_DELAY);
    rtl1_core_end();
    xSemaphoreGive(feed_lock);
    rt4k_link_unlock();
    xSemaphoreGive(xfer_lock);
    return result;
}

// --- uploads (put) ----------------------------------------------------------------------------------

// Two modes. Streaming (plain put) when RTS/CTS flow control is on at the FT232R: frames go out back
// to back and the RT4K's CTS paces them. Otherwise acknowledged (put -a): each frame waits for the
// RT4K's ACK, so its SD card writes set the pace (~61 KB/s).
#define PUT_ACK_TIMEOUT_MS    1000
#define PUT_ATTEMPTS          4      // sends of one frame before giving up
#define PUT_DONE_TIMEOUT_MS   15000  // after the last frame (the RT4K checks the whole file's SHA-256)
#define PUT_WRITE_TIMEOUT_MS  10000  // room in the TX queue (CTS may hold it while the RT4K writes)

static bool write_all(const uint8_t *data, size_t len) {
    const uint32_t t0 = now_ms();
    size_t sent = 0;
    while (sent < len) {
        const size_t n = len - sent < 512 ? len - sent : 512; // the TX queue is smaller than a frame
        if (rt4k_write(data + sent, n)) sent += n;
        else if (now_ms() - t0 > PUT_WRITE_TIMEOUT_MS) return false;
        else vTaskDelay(1);
    }
    return true;
}

static rtl1_phase_t put_state(rtl1_put_state_t *s) {
    xSemaphoreTake(feed_lock, portMAX_DELAY);
    rtl1_core_put_state(s);
    const rtl1_phase_t ph = rtl1_core_phase();
    xSemaphoreGive(feed_lock);
    return ph;
}

// Sends one frame until the RT4K acknowledges it. False if it never does, or the upload ended
// (the RT4K closed or aborted it: the core has the result).
static bool send_acked(const uint8_t *frame, size_t len, uint8_t seq, rtl1_info_t *info) {
    for (int attempt = 0; attempt < PUT_ATTEMPTS; attempt++) {
        rtl1_put_state_t before, now;
        if (put_state(&before) != RTL1_PH_SEND) return false;
        if (!write_all(frame, len)) {
            snprintf(info->detail, sizeof(info->detail), "could not queue frame %u", seq);
            return false;
        }
        for (const uint32_t t0 = now_ms(); now_ms() - t0 < PUT_ACK_TIMEOUT_MS; vTaskDelay(1)) {
            if (put_state(&now) != RTL1_PH_SEND) return false;
            if (now.acks != before.acks && now.last_ack == seq) return true;
            if (now.naks != before.naks) break; // refused: send it again
        }
    }
    snprintf(info->detail, sizeof(info->detail), "no acknowledgement for frame %u", seq);
    return false;
}

// Streaming: queues one frame; the RT4K only speaks up to refuse one (NAK) or give up.
static bool send_streamed(const uint8_t *frame, size_t len, uint8_t seq, uint32_t naks_before, rtl1_info_t *info) {
    rtl1_put_state_t now;
    if (put_state(&now) != RTL1_PH_SEND) return false;
    if (now.naks != naks_before) {
        snprintf(info->detail, sizeof(info->detail), "the RT4K refused frame %u (reason %u)", now.nak_seq,
            now.nak_reason);
        return false;
    }
    if (!write_all(frame, len)) {
        snprintf(info->detail, sizeof(info->detail), "could not queue frame %u (CTS held off?)", seq);
        return false;
    }
    return true;
}

rtl1_result_t rtl1_put(const char *path, uint32_t size, const char *sha256_hex, rtl1_read_fn read, void *ctx,
    rtl1_info_t *info) {
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
    const uint32_t since_cmd = rt4k_ms_since_command();
    if (since_cmd < AFTER_COMMAND_MS) vTaskDelay(pdMS_TO_TICKS(AFTER_COMMAND_MS - since_cmd));

    // Stream if RTS/CTS can be switched on and the RT4K asserts CTS. Only for the upload: in standby
    // the RT4K may drop CTS, and with flow control on nothing (not even "pwr on") would reach it.
    const bool flow_was_on = rt4k_flow_control();
    if (!flow_was_on) {
        rt4k_set_flow_control(true);
        for (int i = 0; i < 50 && !rt4k_flow_control(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool acked = !rt4k_flow_control() || !(rt4k_modem_status() & 0x10);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "put%s %lu %s %s", acked ? " -a" : "", (unsigned long)size, sha256_hex, path);
    xSemaphoreTake(feed_lock, portMAX_DELAY);
    xSemaphoreTake(done_sem, 0); // stale
    rtl1_core_begin_put(cmd, info, 0, now_ms());
    xSemaphoreGive(feed_lock);

    char line[208];
    const int n = snprintf(line, sizeof(line), "\r%s\r\n", cmd);
    rtl1_result_t result = RTL1_ERR_NO_LINK;
    bool ok = n > 0 && n < (int)sizeof(line) && write_all((const uint8_t *)line, (size_t)n);
    if (!ok) snprintf(info->detail, sizeof(info->detail), "could not queue the command");

    // The ready line (or a refusal).
    rtl1_put_state_t st;
    while (ok) {
        xSemaphoreTake(feed_lock, portMAX_DELAY);
        const bool over = rtl1_core_poll(now_ms());
        const rtl1_phase_t ph = rtl1_core_phase();
        xSemaphoreGive(feed_lock);
        if (ph == RTL1_PH_SEND) break;
        if (over) ok = false;
        else xSemaphoreTake(done_sem, pdMS_TO_TICKS(20));
    }

    static uint8_t chunk[RTL1_MAX_PAYLOAD], frame[RTL1_MAX_PAYLOAD + 10];
    uint32_t sent = 0;
    uint8_t seq = 0;
    uint32_t read_ms = 0, send_ms = 0, check_ms = 0; // where the time goes (reported on success)
    if (ok) put_state(&st);
    while (ok && sent < size) {
        const size_t want = size - sent < RTL1_MAX_PAYLOAD ? size - sent : RTL1_MAX_PAYLOAD;
        size_t got = 0;
        const uint32_t tr = now_ms();
        while (got < want) {
            const size_t r = read(ctx, chunk + got, want - got);
            if (!r) break;
            got += r;
        }
        read_ms += now_ms() - tr;
        if (got < want) {
            snprintf(info->detail, sizeof(info->detail), "the upload stopped after %lu of %lu bytes",
                (unsigned long)(sent + got), (unsigned long)size);
            ok = false;
            break;
        }
        const size_t len = rtl1_encode_frame(frame, st.nonce, 3, seq, chunk, (uint16_t)got);
        const uint32_t ts = now_ms();
        ok = acked ? send_acked(frame, len, seq, info) : send_streamed(frame, len, seq, st.naks, info);
        send_ms += now_ms() - ts;
        sent += (uint32_t)got;
        seq++; // one byte on the wire: wraps after 256 frames
    }
    if (ok) {
        // An empty data frame marks the end of the file.
        const size_t len = rtl1_encode_frame(frame, st.nonce, 3, seq, NULL, 0);
        if (acked) send_acked(frame, len, seq, info);
        else send_streamed(frame, len, seq, st.naks, info);
        // Then "put done" (or why not), once the RT4K has checked the file.
        const uint32_t t0 = now_ms();
        while (put_state(&st) == RTL1_PH_SEND && now_ms() - t0 < PUT_DONE_TIMEOUT_MS) {
            xSemaphoreTake(done_sem, pdMS_TO_TICKS(50));
        }
        check_ms = now_ms() - t0;
    }

    xSemaphoreTake(feed_lock, portMAX_DELAY);
    const rtl1_phase_t ph = rtl1_core_phase();
    const rtl1_result_t core_result = rtl1_core_result();
    xSemaphoreGive(feed_lock);
    if (ph == RTL1_PH_IDLE || ph == RTL1_PH_DRAIN) {
        result = core_result; // the RT4K closed it: done, refused, or aborted
        if (result == RTL1_OK) {
            snprintf(info->detail, sizeof(info->detail), "%s; waited %lu ms for data, %lu ms to send, %lu ms for the check",
                acked ? "acknowledged" : "streamed, RTS/CTS", (unsigned long)read_ms, (unsigned long)send_ms,
                (unsigned long)check_ms);
        }
    } else if (ph == RTL1_PH_SEND) {
        // We gave up while the RT4K still waits for frames: tell it, then a bare line to resync.
        const size_t len = rtl1_encode_frame(frame, st.nonce, 6, seq, NULL, 0);
        write_all(frame, len);
        write_all((const uint8_t *)"\r\n", 2);
        if (!info->detail[0]) snprintf(info->detail, sizeof(info->detail), "no \"put done\" from the RT4K");
        result = RTL1_ERR_TIMEOUT;
    } // else still READY: the command never went out (RTL1_ERR_NO_LINK)

    xSemaphoreTake(feed_lock, portMAX_DELAY);
    rtl1_core_end();
    xSemaphoreGive(feed_lock);
    if (!flow_was_on) rt4k_set_flow_control(false);
    rt4k_link_unlock();
    xSemaphoreGive(xfer_lock);
    return result;
}
