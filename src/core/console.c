#include "console.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "json.h"
#include "platform.h"
#include "power.h"
#include "ring.h"
#include "rt4k.h"
#include "rt4k_info.h"
#include "ws.h"

#define CONSOLE_TASK_STACK    512
#define CONSOLE_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define QUEUE_DEPTH           8
#define CMD_MAX               336  // "mv <path>|<path>" with two of rtl1.h's longest paths
#define LINE_MAX_LEN          CON_LINE_MAX
#define HISTORY              64   // routed lines kept for console_read_line (power of two)
#define TERM_SIZE            8192u // the web terminal's text kept (power of two)
#define LINK_WAIT_MS         2000 // for an rtl1 transfer holding the link

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
static TaskHandle_t console_task_h;
static SemaphoreHandle_t query_lock;
static plat_lock_t lock;     // console_core, the history and the terminal's text
static entry_t history[HISTORY];
static uint32_t head;               // lines ever routed
static char term[TERM_SIZE];        // the web terminal's share of the lines (console_term_read)
static uint32_t term_head;          // bytes ever written there
static console_cmd_stats_t cmd_stats;
static volatile uint32_t last_cmd_ms;
static char line[LINE_MAX_LEN];     // being assembled (rt4k task only)
static size_t line_len;

static void route(const char *text) {
    plat_lock_enter(&lock);
    const int owner = console_core_line(text, plat_ms());
    entry_t *e = &history[head & (HISTORY - 1)];
    e->owner = owner;
    snprintf(e->text, sizeof(e->text), "%s", text);
    head++;
    if (owner != CON_POWER && owner != CON_QUERY && owner != CON_FILES) { // the web terminal: all but Cruller's own
        ring_put(term, TERM_SIZE, &term_head, text, strlen(text));
        ring_put(term, TERM_SIZE, &term_head, "\n", 1);
    }
    plat_lock_exit(&lock);
    if (owner != CON_BROADCAST && console_task_h) xTaskNotifyGive(console_task_h); // may close its window
    power_feed_line(text);
    rt4k_info_line(text);
}

size_t console_term_read(uint32_t *pos, char *out, size_t max) {
    plat_lock_enter(&lock);
    const size_t n = ring_read(term, TERM_SIZE, term_head, pos, out, max);
    plat_lock_exit(&lock);
    return n;
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
    plat_lock_enter(&lock);
    const bool waiting = console_core_waiting_reply();
    plat_lock_exit(&lock);
    return waiting;
}

bool console_reply_coming(void) {
    plat_lock_enter(&lock);
    const bool coming = console_core_reply_coming();
    plat_lock_exit(&lock);
    return coming;
}

uint32_t console_head(void) {
    plat_lock_enter(&lock);
    const uint32_t h = head;
    plat_lock_exit(&lock);
    return h;
}

bool console_read_line(uint32_t *seq, int *owner, char *out, size_t size) {
    plat_lock_enter(&lock);
    if (head - *seq > HISTORY) *seq = head - HISTORY; // fell behind: the oldest still kept
    const bool have = *seq != head;
    if (have) {
        const entry_t *e = &history[*seq & (HISTORY - 1)];
        *owner = e->owner;
        snprintf(out, size, "%s", e->text);
        (*seq)++;
    }
    plat_lock_exit(&lock);
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
    // wakes us up early. Read often: "ls" sends a line every ~0.7 ms, and the history keeps only
    // HISTORY lines (~45 ms of them).
    for (const uint32_t t0 = plat_ms(); plat_ms() - t0 < (QUEUE_DEPTH + 1) * CON_MAX_MS;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        const bool done = *flag != 0;
        while (console_read_line(&seq, &o, text, sizeof(text))) {
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
        const uint32_t t0 = plat_ms();
        char text[LINE_MAX_LEN];
        int owner;
        while (!found && plat_ms() - t0 < timeout_ms + QUEUE_DEPTH * CON_NO_REPLY_MS) {
            if (!console_read_line(&seq, &owner, text, sizeof(text))) {
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

// Debug: the last commands, with when their reply started and how long their window stayed open
// (GET /debug/console).
#define CMD_LOG 16
static struct {
    char cmd[28];
    int8_t owner;
    int16_t reply_ms;   // first reply line after the command (-1: none)
    uint16_t window_ms; // command sent -> window closed
} cmd_log[CMD_LOG];
static uint32_t cmd_log_n;

size_t console_debug(char *out, size_t size) {
    size_t o = (size_t)snprintf(out, size, "last commands (owner, first reply, window):\n");
    const uint32_t n = cmd_log_n < CMD_LOG ? cmd_log_n : CMD_LOG;
    for (uint32_t k = cmd_log_n - n; k != cmd_log_n && o < size; k++) {
        const uint32_t i = k & (CMD_LOG - 1);
        o += (size_t)snprintf(out + o, size - o, "%-28s owner %d  reply %4d ms  window %4u ms\n", cmd_log[i].cmd,
            cmd_log[i].owner, cmd_log[i].reply_ms, cmd_log[i].window_ms);
    }
    return o < size ? o : size - 1;
}

size_t console_debug_json(char *out, size_t size) {
    size_t o = 0;
    bool ok = json_put(out, size, &o, "[");
    const uint32_t n = cmd_log_n < CMD_LOG ? cmd_log_n : CMD_LOG;
    for (uint32_t k = cmd_log_n - n; ok && k != cmd_log_n; k++) {
        const uint32_t i = k & (CMD_LOG - 1);
        ok = json_put(out, size, &o, k == cmd_log_n - n ? "{\"cmd\":" : ",{\"cmd\":") && json_put_str(out, size, &o, cmd_log[i].cmd) &&
            json_putf(out, size, &o, ",\"owner\":%d,\"reply_ms\":%d,\"window_ms\":%u}", cmd_log[i].owner, cmd_log[i].reply_ms, cmd_log[i].window_ms);
    }
    return ok && json_put(out, size, &o, "]") ? o : 0;
}

// The way the RT4K reads a line, "\r<cmd>\r\n", once no rtl1 transfer holds the link. False when it
// didn't go out: no RT4K, the link stayed busy, the queue to it was full.
static bool send_command(const char *cmd) {
    char text[CMD_MAX + 4];
    const int n = snprintf(text, sizeof(text), "\r%s\r\n", cmd);
    if (n < 0 || n >= (int)sizeof(text)) return false;
    const uint32_t t0 = plat_ms();
    const bool locked = rt4k_link_lock(LINK_WAIT_MS);
    cmd_stats.last_wait_ms = plat_ms() - t0;
    if (cmd_stats.last_wait_ms > cmd_stats.max_wait_ms) cmd_stats.max_wait_ms = cmd_stats.last_wait_ms;
    const bool ok = locked && rt4k_write(text, (size_t)n);
    if (locked) rt4k_link_unlock();
    if (!ok) {
        cmd_stats.dropped++;
        return false;
    }
    cmd_stats.sent++;
    last_cmd_ms = plat_ms();
    if (!strcmp(cmd, "pwr on")) power_woken();
    return true;
}

uint32_t console_ms_since_command(void) {
    return plat_ms() - last_cmd_ms;
}

void console_cmd_stats(console_cmd_stats_t *out) {
    *out = cmd_stats;
}

static void console_task(void *param) {
    (void)param;
    static request_t req;
    for (;;) {
        xQueueReceive(queue, &req, portMAX_DELAY);
        const uint32_t t0 = plat_ms();
        const bool sent = send_command(req.cmd);
        bool no_reply = false;
        const char *done_when = console_core_final_line(req.cmd, &no_reply);
        if (sent && !no_reply) {
            ulTaskNotifyTake(pdTRUE, 0); // wake-ups from lines of an earlier window
            plat_lock_enter(&lock);
            console_core_begin(req.owner, req.expect, req.timeout_ms, done_when, plat_ms());
            plat_lock_exit(&lock);
            if (!strncmp(req.cmd, "remote ", 7)) ws_key_sent(); // any sender: the mirror refreshes and times it
            for (;;) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)); // route() wakes us on each reply line
                plat_lock_enter(&lock);
                const bool over = console_core_poll(plat_ms());
                plat_lock_exit(&lock);
                if (over) break;
            }
            const uint32_t i = cmd_log_n++ & (CMD_LOG - 1);
            snprintf(cmd_log[i].cmd, sizeof(cmd_log[i].cmd), "%.*s", (int)sizeof(cmd_log[i].cmd) - 1, req.cmd); // its start
            cmd_log[i].owner = (int8_t)req.owner;
            cmd_log[i].reply_ms = (int16_t)console_core_first_reply_ms();
            cmd_log[i].window_ms = (uint16_t)(plat_ms() - t0);
        }
        if (req.notify) {
            *req.status = sent ? 1 : -1;
            xTaskNotifyGive(req.notify);
        }
    }
}

void console_start(void) {
    plat_lock_init(&lock);
    queue = xQueueCreate(QUEUE_DEPTH, sizeof(request_t));
    query_lock = xSemaphoreCreateMutex();
    xTaskCreate(console_task, "console", PLAT_STACK(CONSOLE_TASK_STACK), NULL, CONSOLE_TASK_PRIORITY, &console_task_h);
}
