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
#include "ws_proto.h"

#define MAX_CLIENTS        3
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
#define STATUS_EVERY_MS 5000

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
    if (c->last_status_ms && t - c->last_status_ms < STATUS_EVERY_MS) return true;
    c->last_status_ms = t | 1;
    tx[16] = MSG_STATUS;
    http_status_json((char *)tx + 17, 1024);
    return send_tx(c, WS_OP_BINARY, 1 + strlen((char *)tx + 17));
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
                rt4k_command(cmd);
                if (!strncmp(cmd, "remote ", 7)) {
                    last_key_ms = now_ms();
                    if (mirror_task_h) xTaskNotifyGive(mirror_task_h); // refresh the OSD soon
                }
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
        default:
            return true; // binary and pong: nothing to do
    }
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
    client_t *slot = NULL;
    for (int i = 0; i < MAX_CLIENTS && !slot; i++) {
        if (clients[i].fd < 0) slot = &clients[i];
    }
    if (!slot) {
        // Full: the newcomer wins over the least recently heard-from client (often a reloaded page
        // whose old connection never closed properly).
        slot = &clients[0];
        for (int i = 1; i < MAX_CLIENTS; i++) {
            if (clients[i].last_rx_ms < slot->last_rx_ms) slot = &clients[i];
        }
        printf("ws: full, evicting the quietest client\n");
        drop(slot);
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
    }
    xSemaphoreGive(snap_lock);
    return r;
}

static void mirror_task(void *param) {
    (void)param;
    static uint8_t buf[4096];
    for (;;) {
        if (!client_count || !rt4k_connected()) {
            mirror_where = "idle";
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
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
    if (n > 0 && (size_t)n < size) rt4k_debug(out + n, size - (size_t)n);
}

bool ws_has_room(void) {
    return uxQueueMessagesWaiting(adopt_q) < MAX_CLIENTS; // a full house evicts (see add_client)
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
