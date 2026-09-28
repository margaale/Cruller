#include "console.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "platform.h"
#include "power.h"
#include "rt4k.h"
#include "ws.h"

#define CONSOLE_TASK_STACK    512
#define CONSOLE_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define QUEUE_DEPTH           8
#define CMD_MAX               200
#define LINE_MAX_LEN          160
#define HISTORY               64   // routed lines kept for console_read_line (power of two)

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
static plat_lock_t lock;     // console_core and the history
static entry_t history[HISTORY];
static uint32_t head;               // lines ever routed
static char line[LINE_MAX_LEN];     // being assembled (rt4k task only)
static size_t line_len;

static uint32_t now_ms(void) {
    return plat_ms();
}

static void route(const char *text) {
    plat_lock_enter(&lock);
    const int owner = console_core_line(text, now_ms());
    entry_t *e = &history[head & (HISTORY - 1)];
    e->owner = owner;
    snprintf(e->text, sizeof(e->text), "%s", text);
    head++;
    plat_lock_exit(&lock);
    if (owner != CON_BROADCAST && console_task_h) xTaskNotifyGive(console_task_h); // may close its window
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
    plat_lock_enter(&lock);
    const bool waiting = console_core_waiting_reply();
    plat_lock_exit(&lock);
    return waiting;
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
    // wakes us up early.
    for (const uint32_t t0 = now_ms(); now_ms() - t0 < (QUEUE_DEPTH + 1) * CON_MAX_MS;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
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
        const uint32_t t0 = now_ms();
        char text[LINE_MAX_LEN];
        int owner;
        while (!found && now_ms() - t0 < timeout_ms + QUEUE_DEPTH * CON_NO_REPLY_MS) {
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
    size_t o = (size_t)snprintf(out, size, "[");
    const uint32_t n = cmd_log_n < CMD_LOG ? cmd_log_n : CMD_LOG;
    for (uint32_t k = cmd_log_n - n; k != cmd_log_n && o < size; k++) {
        const uint32_t i = k & (CMD_LOG - 1);
        char cmd[sizeof(cmd_log[i].cmd)];
        size_t c = 0;
        for (const char *p = cmd_log[i].cmd; *p && c < sizeof(cmd) - 1; p++) {
            cmd[c++] = *p == '"' || *p == '\\' || (unsigned char)*p < 0x20 ? '?' : *p; // no escapes needed
        }
        cmd[c] = 0;
        o += (size_t)snprintf(out + o, size - o, "%s{\"cmd\":\"%s\",\"owner\":%d,\"reply_ms\":%d,\"window_ms\":%u}",
            k == cmd_log_n - n ? "" : ",", cmd, cmd_log[i].owner, cmd_log[i].reply_ms, cmd_log[i].window_ms);
    }
    if (o < size) o += (size_t)snprintf(out + o, size - o, "]");
    return o < size ? o : 0;
}

// The last line of each command's reply, when known (console_core_begin's done_when), so the window
// closes on it instead of after the quiet wait: measured, that wait was ~50 ms of every command's
// 60-80 ms. NULL: unknown, the quiet wait decides. *no_reply: never answered (SVS): don't wait at all.
static const char *final_line(const char *cmd, bool *no_reply) {
    static const struct {
        const char *prefix; // ending in ' ': the command's first word(s); else the exact command
        const char *done;
    } table[] = {
        {"remote ", "Serial Remote:"},
        {"ver", "Build tag:"},
        {"model", "model="},
        {"banner", "banner="},
        {"prof get", "prof loaded="},
        {"prof ", "prof load ok|prof save ok|prof:|prof err"},
        {"ls", "ls end|ls err|ls:"},
        {"ls ", "ls end|ls err|ls:"},
        {"stat ", "stat t=|stat err|stat:"},
        {"mkdir ", "mkdir ok|mkdir err|mkdir:"},
        {"rm ", "rm ok|rm err|rm:"},
        {"mv ", "mv ok|mv err|mv:"},
        {"pwr on", "Power On Requested"}, // "Bad Command: pwr on" when it's already on
        {"input ", "input"},
        {"output ", "output"},
        {"fwup ", "fwup"},
    };
    *no_reply = !strncmp(cmd, "SVS ", 4);
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const char *p = table[i].prefix;
        const size_t n = strlen(p);
        if (p[n - 1] == ' ' ? !strncmp(cmd, p, n) : !strcmp(cmd, p)) return table[i].done;
    }
    return NULL;
}

static void console_task(void *param) {
    (void)param;
    static request_t req;
    for (;;) {
        xQueueReceive(queue, &req, portMAX_DELAY);
        const uint32_t t0 = now_ms();
        const bool sent = rt4k_send_command(req.cmd); // false: no RT4K, or the link stayed busy
        bool no_reply = false;
        const char *done_when = final_line(req.cmd, &no_reply);
        if (sent && !no_reply) {
            ulTaskNotifyTake(pdTRUE, 0); // wake-ups from lines of an earlier window
            plat_lock_enter(&lock);
            console_core_begin(req.owner, req.expect, req.timeout_ms, done_when, now_ms());
            plat_lock_exit(&lock);
            if (!strncmp(req.cmd, "remote ", 7)) ws_key_sent(); // any sender: the mirror refreshes and times it
            for (;;) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)); // route() wakes us on each reply line
                plat_lock_enter(&lock);
                const bool over = console_core_poll(now_ms());
                plat_lock_exit(&lock);
                if (over) break;
            }
            const uint32_t i = cmd_log_n++ & (CMD_LOG - 1);
            snprintf(cmd_log[i].cmd, sizeof(cmd_log[i].cmd), "%.*s", (int)sizeof(cmd_log[i].cmd) - 1, req.cmd); // its start
            cmd_log[i].owner = (int8_t)req.owner;
            cmd_log[i].reply_ms = (int16_t)console_core_first_reply_ms();
            cmd_log[i].window_ms = (uint16_t)(now_ms() - t0);
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
