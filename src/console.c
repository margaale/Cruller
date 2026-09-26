#include "console.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "pico/sync.h"
#include "pico/time.h"

#include "power.h"
#include "rt4k.h"
#include "ws.h"

#define CONSOLE_TASK_STACK    512
#define CONSOLE_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define QUEUE_DEPTH           8
#define CMD_MAX               200
#define LINE_MAX_LEN          160
#define HISTORY               64   // routed lines kept for console_read (power of two)

typedef struct {
    int owner;
    char cmd[CMD_MAX];
    char expect[CON_EXPECT_MAX];
    uint32_t timeout_ms;
    TaskHandle_t notify;           // console_run: told when the reply window has closed
    volatile int *status;          // set first: 1 sent and done, -1 not sent
} request_t;

typedef struct {
    int owner;
    char text[LINE_MAX_LEN];
} entry_t;

static QueueHandle_t queue;
static SemaphoreHandle_t query_lock;
static critical_section_t lock;     // console_core and the history
static entry_t history[HISTORY];
static uint32_t head;               // lines ever routed
static char line[LINE_MAX_LEN];     // being assembled (rt4k task only)
static size_t line_len;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void route(const char *text) {
    critical_section_enter_blocking(&lock);
    const int owner = console_core_line(text, now_ms());
    entry_t *e = &history[head & (HISTORY - 1)];
    e->owner = owner;
    snprintf(e->text, sizeof(e->text), "%s", text);
    head++;
    critical_section_exit(&lock);
    power_feed_line(text);
    if (owner != CON_POWER && owner != CON_QUERY) { // the web terminal: all but Cruller's own checks
        rt4k_term_push((const uint8_t *)text, strlen(text));
        rt4k_term_push((const uint8_t *)"\n", 1);
    }
}

void console_feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = (char)data[i];
        if (c == '\n' || line_len == sizeof(line) - 1) {
            if (line_len && line[line_len - 1] == '\r') line_len--;
            line[line_len] = 0;
            line_len = 0;
            route(line);
            if (c == '\n') continue;
        }
        line[line_len++] = c;
    }
}

bool console_reply_pending(void) {
    critical_section_enter_blocking(&lock);
    const bool waiting = console_core_waiting_reply();
    critical_section_exit(&lock);
    return waiting;
}

uint32_t console_head(void) {
    critical_section_enter_blocking(&lock);
    const uint32_t h = head;
    critical_section_exit(&lock);
    return h;
}

bool console_read(uint32_t *seq, int *owner, char *out, size_t size) {
    critical_section_enter_blocking(&lock);
    if (head - *seq > HISTORY) *seq = head - HISTORY; // fell behind: the oldest still kept
    const bool have = *seq != head;
    if (have) {
        const entry_t *e = &history[*seq & (HISTORY - 1)];
        *owner = e->owner;
        snprintf(out, size, "%s", e->text);
        (*seq)++;
    }
    critical_section_exit(&lock);
    return have;
}

static bool enqueue_req(request_t *req, const char *cmd, const char *expect) {
    if (strlen(cmd) >= sizeof(req->cmd)) return false;
    snprintf(req->cmd, sizeof(req->cmd), "%s", cmd);
    snprintf(req->expect, sizeof(req->expect), "%s", expect ? expect : "");
    return xQueueSend(queue, req, pdMS_TO_TICKS(100)) == pdTRUE; // copied into the queue
}

static bool enqueue(int owner, const char *cmd, const char *expect, uint32_t timeout_ms) {
    request_t req = {.owner = owner, .timeout_ms = timeout_ms};
    return enqueue_req(&req, cmd, expect);
}

// console_run's done flags, one per owner (not on the caller's stack: a caller that gave up must not
// be written to later).
#define MAX_OWNERS 8
static volatile int run_status[MAX_OWNERS];

bool console_run(int owner, const char *cmd, void (*on_line)(const char *line, void *ctx), void *ctx) {
    if (owner < 0 || owner >= MAX_OWNERS) return false;
    volatile int *const flag = &run_status[owner];
    *flag = 0;
    request_t req = {.owner = owner, .notify = xTaskGetCurrentTaskHandle(), .status = flag};
    uint32_t seq = console_head(); // only what comes after this command
    ulTaskNotifyTake(pdTRUE, 0);   // a wake-up left over from an earlier run (it saw the flag first)
    if (!enqueue_req(&req, cmd, NULL)) return false;
    char text[LINE_MAX_LEN];
    int o;
    // Commands queued before ours go first: allow for them. The flag decides; the notification only
    // wakes us up early.
    for (const uint32_t t0 = now_ms(); now_ms() - t0 < (QUEUE_DEPTH + 1) * CON_MAX_MS;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        const bool done = *flag != 0;
        while (console_read(&seq, &o, text, sizeof(text))) {
            if (o == owner && on_line) on_line(text, ctx);
        }
        if (done) break;
    }
    return *flag == 1;
}

bool console_send(int owner, const char *cmd) {
    return enqueue(owner, cmd, NULL, 0);
}

bool console_query(const char *cmd, const char *expect, char *out, size_t size, uint32_t timeout_ms) {
    if (xSemaphoreTake(query_lock, pdMS_TO_TICKS(timeout_ms + 2000)) != pdTRUE) return false;
    uint32_t seq = console_head(); // only what comes after this command
    bool found = false;
    if (enqueue(CON_QUERY, cmd, expect, timeout_ms)) {
        // The queue may hold other commands first: allow for them on top of our own timeout.
        const uint32_t t0 = now_ms();
        char text[LINE_MAX_LEN];
        int owner;
        while (!found && now_ms() - t0 < timeout_ms + QUEUE_DEPTH * CON_NO_REPLY_MS) {
            if (!console_read(&seq, &owner, text, sizeof(text))) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (owner != CON_QUERY || !strstr(text, expect)) continue;
            snprintf(out, size, "%s", strncmp(text, "[COM] ", 6) ? text : text + 6);
            found = true;
        }
    }
    xSemaphoreGive(query_lock);
    return found;
}

static void console_task(void *param) {
    (void)param;
    static request_t req;
    for (;;) {
        xQueueReceive(queue, &req, portMAX_DELAY);
        const bool sent = rt4k_send_command(req.cmd); // false: no RT4K, or the link stayed busy
        if (sent) {
            critical_section_enter_blocking(&lock);
            // A remote key's whole reply is one line, "[COM] Serial Remote: <key>": done at once.
            const bool key = !strncmp(req.cmd, "remote ", 7);
            console_core_begin(req.owner, req.expect, req.timeout_ms, key ? "Serial Remote:" : NULL, now_ms());
            critical_section_exit(&lock);
            if (key) ws_key_sent(); // from any sender: the mirror refreshes soon, and times it
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(10));
                critical_section_enter_blocking(&lock);
                const bool over = console_core_poll(now_ms());
                critical_section_exit(&lock);
                if (over) break;
            }
        }
        if (req.notify) {
            *req.status = sent ? 1 : -1;
            xTaskNotifyGive(req.notify);
        }
    }
}

void console_start(void) {
    critical_section_init(&lock);
    queue = xQueueCreate(QUEUE_DEPTH, sizeof(request_t));
    query_lock = xSemaphoreCreateMutex();
    xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIORITY, NULL);
}
