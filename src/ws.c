#include "ws.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "lwip/sockets.h"
#include "pico/time.h"

#include "rt4k.h"
#include "http.h"
#include "log.h"
#include "rtl1.h"
#include "clients.h"
#include "console.h"
#include "net.h"
#include "svs.h"
#include "power.h"
#include "ws_proto.h"

#define MAX_CLIENTS        CLIENTS_MAX // slots; the shared budget (clients.h) caps pages + RFC 2217
#define WS_TASK_STACK      1536   // words
#define WS_TASK_PRIORITY   (tskIDLE_PRIORITY + 2)
#define MIRROR_TASK_STACK  1024
#define MIRROR_PRIORITY    (tskIDLE_PRIORITY + 2)
#define RX_MAX             512    // biggest client message (console commands are short)
#define TERM_CHUNK         1024
#define PING_EVERY_MS      10000  // keepalive; browsers answer pings on their own
#define SILENT_DROP_MS     25000  // nothing received for this long: the peer is gone

#define MSG_TERM   0x01
#define MSG_PLANE  0x02
#define MSG_FONT   0x03
#define MSG_LOG    0x04   // Cruller's own log (what /log serves)
#define MSG_STATUS 0x05   // the /status JSON, every STATUS_EVERY_MS
#define MSG_DEBUG  0x06   // [0x06, kind, text...] to pages showing Debug, every DEBUG_EVERY_MS (see ws.h)
#define MSG_IN_VISIBILITY 0x10 // client -> server, binary: [0x10, 1 visible | 0 hidden]
#define MSG_IN_DEBUG      0x11 // client -> server, binary: [0x11, 1 showing Debug | 0 not]
#define STATUS_EVERY_MS 5000
#define PUT_STATUS_EVERY_MS 500 // while an upload to the RT4K runs: its progress is in the status
#define DEBUG_EVERY_MS  2000

#define POLL_IDLE_MS     250      // OSD poll period
#define POLL_ACTIVE_MS   60       // right after a key press
#define ACTIVE_WINDOW_MS 1500
#define POLL_READY_TIMEOUT_MS 600  // a lost poll request frees the link quickly (keys wait for it)
#define OFFLINE_BACKOFF_MS 2000   // RT4K not answering (standby)

typedef struct {
    int fd;                       // -1 = free
    uint8_t rx[RX_MAX + 16];
    size_t rx_len;
    uint32_t term_pos;
    uint32_t font_sent;           // versions already sent
    uint32_t plane_sent[2];
    uint32_t last_rx_ms;          // any frame received (pongs included)
    uint32_t last_ping_ms;
    uint32_t log_pos;
    uint32_t last_status_ms;      // 0 = send one right away
    int status_power;             // power state in the last status sent: a change is pushed at once
    uint32_t status_setup;        // the setup wizard's progress and the SVS input in the last status sent (likewise)
    bool hidden;                  // the page says it's not on screen (background tab)
    bool debug;                   // the page shows its Debug tab: gets MSG_DEBUG
    uint32_t last_debug_ms;       // 0 = send right away
    uint32_t con_seq;             // Debug's serial log: the next console line to send (console_read)
} client_t;

typedef struct {
    uint8_t data[4096];
    size_t len;                   // 0 = nothing shown
    char ready[160];
    uint32_t version;             // 0 = never polled
} plane_t;

static client_t clients[MAX_CLIENTS];
static volatile int client_count;
static QueueHandle_t adopt_q;
static TaskHandle_t mirror_task_h;

static SemaphoreHandle_t snap_lock; // planes and font
static plane_t planes[2];
static uint8_t font[4096];
static uint32_t font_version;
static volatile uint32_t last_key_ms;
// Key -> screen: from a remote key going out to the first menu change the mirror sees after it.
static volatile uint32_t key_pending_ms; // 0: none waiting
static struct { uint32_t last, max, total, count; } key_lat;

static uint8_t tx[4096 + 256];    // ws task only

// Debug: where each task is (see /debug/tasks).
static volatile const char *ws_where = "start", *mirror_where = "start";
static volatile rtl1_result_t last_result[2] = {RTL1_OK, RTL1_OK};
static char last_detail[2][96];
static uint32_t poll_last_ms[2], poll_max_ms[2], poll_errors[2][5]; // debug: per plane, errors by result

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// --- sending (ws task) -------------------------------------------------------------------------

static bool send_all(int fd, const uint8_t *p, size_t len) {
    while (len) {
        ws_where = "send";
        const int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// Frames tx[16 .. 16 + len) (the payload is built there) and sends it.
static bool send_tx(client_t *c, uint8_t opcode, size_t len) {
    uint8_t hdr[10];
    const size_t h = ws_frame_header(hdr, opcode, len);
    memcpy(tx + 16 - h, hdr, h);
    return send_all(c->fd, tx + 16 - h, h + len);
}

static void drop(client_t *c) {
    closesocket(c->fd);
    c->fd = -1;
    client_count--;
    clients_give();
    printf("ws: client left (%d connected)\n", client_count);
}

// The page used to poll /log and /status: both come over the socket now.
static bool push_log_status(client_t *c) {
    for (int i = 0; i < 4; i++) {
        tx[16] = MSG_LOG;
        const size_t n = log_read(&c->log_pos, (char *)tx + 17, TERM_CHUNK);
        if (!n) break;
        if (!send_tx(c, WS_OP_BINARY, 1 + n)) return false;
    }
    const uint32_t t = now_ms();
    const int power = (int)power_state();
    char p[4];
    uint32_t sent, size;
    const uint32_t every = rtl1_put_progress(p, sizeof(p), &sent, &size) ? PUT_STATUS_EVERY_MS : STATUS_EVERY_MS;
    const uint32_t setup = net_setup_version() + svs_version(); // the wizard's progress, the switch's input
    if (c->last_status_ms && t - c->last_status_ms < every && power == c->status_power && setup == c->status_setup) return true;
    c->last_status_ms = t | 1;
    c->status_power = power;
    c->status_setup = setup;
    tx[16] = MSG_STATUS;
    http_status_json((char *)tx + 17, 1536);
    return send_tx(c, WS_OP_BINARY, 1 + strlen((char *)tx + 17));
}

static size_t mirror_json(char *out, size_t size);

// Debug tab: one JSON report, pushed instead of polled: {"status","serial","mirror","console","memory"}.
// Only cheap numbers: the task list (uxTaskGetSystemState) suspends the scheduler and stays on request
// (/debug/tasks?stacks).
static bool push_debug(client_t *c) {
    if (!c->debug) return true;
    // Every line from the RT4K, with who it was for: [0x06, 6, "owner\ttext\n"...] (see ws.h).
    for (int batch = 0; batch < 4; batch++) {
        size_t o = 0;
        int owner;
        char line[160];
        while (o < 1024 && console_read(&c->con_seq, &owner, line, sizeof(line))) {
            o += (size_t)snprintf((char *)tx + 18 + o, sizeof(tx) - 18 - o, "%d\t%s\n", owner, line);
        }
        if (!o) break;
        tx[16] = MSG_DEBUG;
        tx[17] = 6;
        if (!send_tx(c, WS_OP_BINARY, 2 + o)) return false;
    }
    const uint32_t t = now_ms();
    if (c->last_debug_ms && t - c->last_debug_ms < DEBUG_EVERY_MS) return true;
    c->last_debug_ms = t | 1;
    char *out = (char *)tx + 18;
    const size_t size = sizeof(tx) - 18;
    size_t o = (size_t)snprintf(out, size, "{\"status\":");
    http_status_json(out + o, size - o);
    o += strlen(out + o);
    static const char *const keys[] = {",\"serial\":", ",\"mirror\":", ",\"console\":", ",\"memory\":"};
    for (int k = 0; k < 4; k++) {
        const size_t kl = strlen(keys[k]);
        if (o + kl + 2 >= size) return true; // doesn't fit: skip this round rather than send half
        memcpy(out + o, keys[k], kl + 1);
        o += kl;
        const size_t n = k == 0 ? rt4k_debug_json(out + o, size - o) : k == 1 ? mirror_json(out + o, size - o)
            : k == 2 ? console_debug_json(out + o, size - o) : http_debug_memory_json(out + o, size - o);
        if (!n) return true;
        o += n;
    }
    if (o + 2 >= size) return true;
    out[o++] = '}';
    tx[16] = MSG_DEBUG;
    tx[17] = 5; // kind 5: the JSON report (see ws.h)
    return send_tx(c, WS_OP_BINARY, 2 + o);
}

static bool push_terminal(client_t *c) {
    for (int i = 0; i < 8; i++) {
        tx[16] = MSG_TERM;
        const size_t n = rt4k_rx_read(&c->term_pos, (char *)tx + 17, TERM_CHUNK);
        if (!n) return true;
        if (!send_tx(c, WS_OP_BINARY, 1 + n)) return false;
    }
    return true;
}

static bool push_mirror(client_t *c) {
    size_t len = 0;
    xSemaphoreTake(snap_lock, portMAX_DELAY);
    if (font_version && c->font_sent != font_version) {
        tx[16] = MSG_FONT;
        memcpy(tx + 17, font, sizeof(font));
        len = 1 + sizeof(font);
        c->font_sent = font_version;
    }
    xSemaphoreGive(snap_lock);
    if (len && !send_tx(c, WS_OP_BINARY, len)) return false;

    for (int p = 0; p < 2; p++) {
        len = 0;
        xSemaphoreTake(snap_lock, portMAX_DELAY);
        const plane_t *pl = &planes[p];
        if (pl->version && c->plane_sent[p] != pl->version) {
            const size_t rl = strlen(pl->ready);
            tx[16] = MSG_PLANE;
            tx[17] = (uint8_t)(p + 1);
            tx[18] = (uint8_t)rl;
            memcpy(tx + 19, pl->ready, rl);
            memcpy(tx + 19 + rl, pl->data, pl->len);
            len = 3 + rl + pl->len;
            c->plane_sent[p] = pl->version;
        }
        xSemaphoreGive(snap_lock);
        if (len && !send_tx(c, WS_OP_BINARY, len)) return false;
    }
    return true;
}

// --- receiving (ws task) -----------------------------------------------------------------------

static bool on_frame(client_t *c, const ws_frame_t *f) {
    switch (f->opcode) {
        case WS_OP_TEXT: {
            char cmd[241];
            size_t n = f->len < sizeof(cmd) - 1 ? f->len : sizeof(cmd) - 1;
            memcpy(cmd, f->payload, n);
            while (n && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n' || cmd[n - 1] == ' ')) n--;
            cmd[n] = 0;
            if (n) {
                for (size_t i = 0; i < n; i++) if (cmd[i] == '\r' || cmd[i] == '\n') cmd[i] = ' ';
                ws_where = "rt4k_command";
                rt4k_command(cmd); // keys: console.c calls ws_key_sent() once it's out
            }
            return true;
        }
        case WS_OP_PING:
            memmove(tx + 16, f->payload, f->len);
            return send_tx(c, WS_OP_PONG, f->len);
        case WS_OP_CLOSE: {
            const size_t n = f->len >= 2 ? 2 : 0; // echo the status code
            memmove(tx + 16, f->payload, n);
            send_tx(c, WS_OP_CLOSE, n);
            return false;
        }
        case WS_OP_BINARY:
            // [0x10, 1|0]: the page is visible / hidden (a background tab needs no mirror).
            if (f->len >= 2 && f->payload[0] == MSG_IN_VISIBILITY) {
                const bool was_hidden = c->hidden;
                c->hidden = f->payload[1] == 0;
                if (was_hidden && !c->hidden && mirror_task_h) xTaskNotifyGive(mirror_task_h);
            }
            // [0x11, 1|0]: the page shows its Debug tab on screen / doesn't.
            if (f->len >= 2 && f->payload[0] == MSG_IN_DEBUG) {
                const bool was = c->debug;
                c->debug = f->payload[1] != 0;
                c->last_debug_ms = 0;
                if (c->debug && !was) { // the serial log starts with the last lines kept
                    const uint32_t h = console_head();
                    c->con_seq = h > 40 ? h - 40 : 0;
                }
            }
            return true;
        default:
            return true; // pong: nothing to do
    }
}

// Clients whose page is on screen: the mirror polls only for them.
static int visible_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0 && !clients[i].hidden;
    return n;
}

static bool on_readable(client_t *c) {
    ws_where = "recv";
    const int n = recv(c->fd, c->rx + c->rx_len, RX_MAX - c->rx_len, 0);
    if (n <= 0) return false;
    c->rx_len += (size_t)n;
    c->last_rx_ms = now_ms();
    for (;;) {
        ws_frame_t f;
        const long used = ws_parse(c->rx, c->rx_len, RX_MAX - 14, &f);
        if (used < 0) return false;
        if (used == 0) return c->rx_len < RX_MAX;
        if (!on_frame(c, &f)) return false;
        memmove(c->rx, c->rx + used, c->rx_len - (size_t)used);
        c->rx_len -= (size_t)used;
    }
}

// --- tasks -------------------------------------------------------------------------------------

static void add_client(int fd) {
    if (!clients_take()) {
        // The shared budget is full: the newcomer wins over the least recently heard-from page (often
        // a reloaded page whose old connection never closed properly), but never over another kind.
        client_t *quietest = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && (!quietest || clients[i].last_rx_ms < quietest->last_rx_ms)) quietest = &clients[i];
        }
        if (!quietest) {
            printf("ws: no room: all %d client slots are taken by RFC 2217 clients\n", CLIENTS_MAX);
            closesocket(fd);
            return;
        }
        printf("ws: full, replacing the quietest page\n");
        drop(quietest);
        clients_take(); // the slot it gave back
    }
    client_t *slot = NULL;
    for (int i = 0; i < MAX_CLIENTS && !slot; i++) {
        if (clients[i].fd < 0) slot = &clients[i];
    }
    memset(slot, 0, sizeof(*slot));
    slot->fd = fd;
    slot->last_rx_ms = slot->last_ping_ms = now_ms();
    client_count++;
    printf("ws: client joined (%d connected)\n", client_count);
    if (mirror_task_h) xTaskNotifyGive(mirror_task_h);
}

// Pings now and then; drops clients that stopped answering.
static bool keepalive(client_t *c) {
    const uint32_t t = now_ms();
    if (t - c->last_rx_ms > SILENT_DROP_MS) {
        printf("ws: client silent for %lu s\n", (unsigned long)((t - c->last_rx_ms) / 1000));
        return false;
    }
    if (t - c->last_ping_ms < PING_EVERY_MS) return true;
    c->last_ping_ms = t;
    return send_tx(c, WS_OP_PING, 0);
}

static void ws_task(void *param) {
    (void)param;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    for (;;) {
        int fd;
        ws_where = "queue";
        while (xQueueReceive(adopt_q, &fd, client_count ? 0 : portMAX_DELAY) == pdTRUE) {
            add_client(fd);
            if (client_count >= MAX_CLIENTS) break;
        }

        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &rd);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }
        if (maxfd < 0) continue;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 30000};
        ws_where = "select";
        const int ready = select(maxfd + 1, &rd, NULL, NULL, &tv);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0) continue;
            bool ok = true;
            if (ready > 0 && FD_ISSET(c->fd, &rd)) ok = on_readable(c);
            ws_where = "push";
            if (ok) ok = push_terminal(c);
            if (ok) ok = push_mirror(c);
            if (ok) ok = push_log_status(c);
            if (ok) ok = push_debug(c);
            if (ok) ok = keepalive(c);
            if (!ok && c->fd >= 0) drop(c);
        }
    }
}

static bool offline(rtl1_result_t r) {
    return r == RTL1_ERR_TIMEOUT || r == RTL1_ERR_NO_LINK; // no answer (standby, unplugged, link busy)
}

// Polls one plane into the snapshot.
static rtl1_result_t poll_plane(int p, uint8_t *buf) {
    static rtl1_info_t info;
    mirror_where = p ? "osd2" : "osd";
    const uint32_t t0 = now_ms();
    const rtl1_result_t r = rtl1_transfer(p ? "osd2" : "osd", buf, 4096, &info, true, POLL_READY_TIMEOUT_MS);
    if (r == RTL1_OK || r == RTL1_ERR_DEVICE || r == RTL1_ERR_PROTOCOL) power_alive(); // it answered
    else if (r == RTL1_ERR_TIMEOUT) power_silent();
    poll_last_ms[p] = now_ms() - t0;
    if (poll_last_ms[p] > poll_max_ms[p]) poll_max_ms[p] = poll_last_ms[p];
    if (r != RTL1_OK && (unsigned)r < 5) poll_errors[p][r]++;
    mirror_where = "compare";
    last_result[p] = r;
    if (r != RTL1_OK) memcpy(last_detail[p], info.detail, sizeof(last_detail[p])); // keep the last error
    // "nothing shown" is an empty plane; any other refusal (busy...) keeps what we had: blanking the
    // plane on "busy" made the menu flicker while navigating.
    const bool empty = r == RTL1_ERR_DEVICE && strstr(info.detail, "nothing shown");
    if (r != RTL1_OK && !empty) return r;
    const size_t len = r == RTL1_OK ? info.len : 0;
    // The nonce changes every time: compare the ready line only up to it.
    char *nonce = strstr(info.ready, " nonce=");
    if (nonce) *nonce = 0;
    xSemaphoreTake(snap_lock, portMAX_DELAY);
    plane_t *pl = &planes[p];
    if (!pl->version || pl->len != len || memcmp(pl->data, buf, len) || strcmp(pl->ready, info.ready)) {
        memcpy(pl->data, buf, len);
        pl->len = len;
        snprintf(pl->ready, sizeof(pl->ready), "%s", info.ready);
        pl->version++;
        const uint32_t k = key_pending_ms;
        if (k && pl->version > 1) { // a key was waiting for its effect: this is it
            const uint32_t lat = now_ms() - k;
            key_lat.last = lat;
            if (lat > key_lat.max) key_lat.max = lat;
            key_lat.total += lat;
            key_lat.count++;
            key_pending_ms = 0;
        }
    }
    xSemaphoreGive(snap_lock);
    return r;
}

int ws_clients(int *max) {
    if (max) *max = MAX_CLIENTS;
    return client_count;
}

void ws_debug_reset(void) {
    memset(&key_lat, 0, sizeof(key_lat));
    memset(poll_errors, 0, sizeof(poll_errors));
    poll_max_ms[0] = poll_max_ms[1] = 0;
}

void ws_key_sent(void) {
    const uint32_t t = now_ms();
    last_key_ms = t;
    key_pending_ms = t | 1;
    if (mirror_task_h) xTaskNotifyGive(mirror_task_h); // refresh the OSD soon
}

static void mirror_task(void *param) {
    (void)param;
    static uint8_t buf[4096];
    for (;;) {
        if (!visible_count() || !rt4k_connected()) {
            mirror_where = client_count ? "idle (no page on screen)" : "idle";
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        // A key whose effect didn't show (end of a menu...) mustn't be matched to a later change.
        if (key_pending_ms && now_ms() - key_pending_ms > ACTIVE_WINDOW_MS) key_pending_ms = 0;
        const power_state_t pw = power_state();
        if (pw == PWR_STANDBY || pw == PWR_BOOTING) {
            mirror_where = "rt4k asleep"; // nothing to show; power.c notices when it's back
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        rtl1_result_t r = RTL1_OK;
        if (!font_version) {
            static rtl1_info_t info;
            mirror_where = "font";
            r = rtl1_transfer("font", buf, sizeof(buf), &info, true, 0);
            if (r == RTL1_OK && info.len == sizeof(font)) {
                xSemaphoreTake(snap_lock, portMAX_DELAY);
                memcpy(font, buf, sizeof(font));
                font_version++;
                xSemaphoreGive(snap_lock);
            }
        }
        if (!offline(r)) r = poll_plane(0, buf);
        // The menu is in the main plane: while keys are being pressed, poll only that one.
        const bool navigating = now_ms() - last_key_ms < ACTIVE_WINDOW_MS;
        if (!offline(r) && !navigating) r = poll_plane(1, buf);
        // Back off only when the RT4K doesn't answer; a refusal or a bad frame is retried soon.
        const uint32_t wait = offline(r) ? OFFLINE_BACKOFF_MS : navigating ? POLL_ACTIVE_MS : POLL_IDLE_MS;
        mirror_where = "wait";
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait)); // a key press cuts the wait short
    }
}

void ws_debug(char *out, size_t size) {
    int n = snprintf(out, size, "ws=%s mirror=%s clients=%d queued=%u\n", ws_where, mirror_where, client_count,
        (unsigned)uxQueueMessagesWaiting(adopt_q));
    for (int p = 0; p < 2 && n > 0 && (size_t)n < size; p++) {
        n += snprintf(out + n, size - (size_t)n,
            "%s: last %s, last error '%s' | poll last %lu ms max %lu ms | errors nolink %lu timeout %lu device %lu protocol %lu | version %lu\n",
            p ? "osd2" : "osd", rtl1_result_name(last_result[p]), last_detail[p], (unsigned long)poll_last_ms[p],
            (unsigned long)poll_max_ms[p], (unsigned long)poll_errors[p][RTL1_ERR_NO_LINK],
            (unsigned long)poll_errors[p][RTL1_ERR_TIMEOUT], (unsigned long)poll_errors[p][RTL1_ERR_DEVICE],
            (unsigned long)poll_errors[p][RTL1_ERR_PROTOCOL], (unsigned long)planes[p].version);
    }
    if (n > 0 && (size_t)n < size) {
        n += snprintf(out + n, size - (size_t)n, "key -> screen: last %lu ms, avg %lu ms, max %lu ms (%lu keys)\n",
            (unsigned long)key_lat.last, (unsigned long)(key_lat.count ? key_lat.total / key_lat.count : 0),
            (unsigned long)key_lat.max, (unsigned long)key_lat.count);
    }
    if (n > 0 && (size_t)n < size) rt4k_debug(out + n, size - (size_t)n);
}

// The screen mirror for the Debug report: where it is, each plane's polls, key -> screen times.
static size_t mirror_json(char *out, size_t size) {
    int n = snprintf(out, size, "{\"state\":\"%s\",\"visible_pages\":%d,\"planes\":[", mirror_where, visible_count());
    for (int p = 0; p < 2 && n > 0 && (size_t)n < size; p++) {
        n += snprintf(out + n, size - (size_t)n,
            "%s{\"name\":\"%s\",\"frames\":%lu,\"last\":\"%s\",\"poll_last_ms\":%lu,\"poll_max_ms\":%lu,"
            "\"no_link\":%lu,\"timeout\":%lu,\"device\":%lu,\"protocol\":%lu}",
            p ? "," : "", p ? "osd2" : "osd", (unsigned long)planes[p].version, rtl1_result_name(last_result[p]),
            (unsigned long)poll_last_ms[p], (unsigned long)poll_max_ms[p],
            (unsigned long)poll_errors[p][RTL1_ERR_NO_LINK], (unsigned long)poll_errors[p][RTL1_ERR_TIMEOUT],
            (unsigned long)poll_errors[p][RTL1_ERR_DEVICE], (unsigned long)poll_errors[p][RTL1_ERR_PROTOCOL]);
    }
    if (n > 0 && (size_t)n < size) {
        n += snprintf(out + n, size - (size_t)n, "],\"key\":{\"last\":%lu,\"avg\":%lu,\"max\":%lu,\"count\":%lu}}",
            (unsigned long)key_lat.last, (unsigned long)(key_lat.count ? key_lat.total / key_lat.count : 0),
            (unsigned long)key_lat.max, (unsigned long)key_lat.count);
    }
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

bool ws_has_room(void) {
    // A free slot in the shared budget, or a page to replace (see add_client); and handovers moving.
    return (clients_used() < CLIENTS_MAX || client_count > 0) && uxQueueMessagesWaiting(adopt_q) < MAX_CLIENTS;
}

bool ws_adopt(int fd) {
    if (!ws_has_room()) return false;
    // Blocking sends with a bound, so one stalled browser can't hold the others up for long.
    const struct timeval snd = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    const struct timeval rcv = {.tv_sec = 0, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv)); // select() says when to read
    return xQueueSend(adopt_q, &fd, 0) == pdTRUE;
}

void ws_start(void) {
    adopt_q = xQueueCreate(MAX_CLIENTS, sizeof(int));
    snap_lock = xSemaphoreCreateMutex();
    xTaskCreate(ws_task, "ws", WS_TASK_STACK, NULL, WS_TASK_PRIORITY, NULL);
    xTaskCreate(mirror_task, "mirror", MIRROR_TASK_STACK, NULL, MIRROR_PRIORITY, &mirror_task_h);
}
