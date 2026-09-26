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
