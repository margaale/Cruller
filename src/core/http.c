#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "lwip/sockets.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h" // /debug/tcp: lwIP's PCB lists

#include "buttons.h"
#include "cfgfs.h"
#include "creds.h"
#include "gameid.h"
#include "gameid_run.h"
#include "health.h"
#include "freeze.h"
#include "json.h"
#include "log.h"
#include "clients.h"
#include "console.h"
#include "power.h"
#include "rfc2217.h"
#include "rt4k.h"
#include "rt4k_info.h"
#include "rtl1.h"
#include "settings.h"
#include "svs.h"
#include "ws.h"
#include "ws_proto.h"
#include "net.h"
#include "ota.h"
#include "ota_fetch.h"
#include "platform.h"
#include "version.h"

#define HTTP_TASK_STACK     3072
#define HTTP_TASK_PRIORITY  (tskIDLE_PRIORITY + 2)
#define HTTP_PORT           80
#define HTTP_BACKLOG        4    // connections waiting while one request is served
#define XFER_TASK_STACK     2048
#define XFER_QUEUE          3    // SD card transfers waiting for the one running
#define SD_PATH_MAX         RTL1_PATH_MAX
#define RECV_TIMEOUT_MS     15000
#define HEADER_MAX          1536
#define BODY_CHUNK          1024
#define RT4K_SETTINGS_MAX   24576 // sget: the live settings (22876 bytes on firmware 1.9x), with room to grow

static volatile bool listening = false;
bool http_listening(void) { return listening; }

typedef struct {
    int fd;
    char method[8];
    char path[512];       // with the query string (/rt4k/put carries a 64-digit SHA-256, SD paths come escaped)
    long content_length;
    char head[HEADER_MAX];
    size_t head_len;      // bytes in head[] (headers plus any body bytes read along with them)
    size_t body_start;    // offset of the body inside head[]
    bool adopted;         // the socket now belongs to someone else (WebSocket): don't close it
} request_t;

#define API_MAX_COMMANDS 8
#define API_CMD_MAX      200
#define LS_BUF           24576 // a folder listing, as sent (~50 bytes an entry)

// The handlers' buffers. The HTTP task serves one request at a time, so they share this one place, each
// handler its own member: as big as the biggest (the live settings, or a listing), where a static each
// took 68 KB of the Pico 2 W's RAM. Not the transfer task's (/rt4k/get and /rt4k/put): those use none of
// them.
static union {
    struct {
        char out[2048];
        TaskStatus_t tasks[24];
    } debug_tasks;
    char debug_memory[2048];
    struct {
        uint8_t buf[RT4K_SETTINGS_MAX];
        rtl1_info_t info;
    } xfer;                     // /rt4k/xfer: an OSD plane, the font, or the live settings (sget)
    char ls[LS_BUF];            // /rt4k/ls
    char stream[2048];          // /log, /rt4k/rx
    char form[384];             // /rt4k/ask (its "mv <path>|<path>"), /wifi, /settings
    uint8_t raw[4608 + 1];      // /debug/raw, POST /setup
    struct api_command_bufs {
        char body[4608];
        char cmds[API_MAX_COMMANDS][API_CMD_MAX];
        json_tok_t tok[32];     // its JSON: room for the commands and a few other keys
        char results[3800], out[4096];
    } api_command;
    // The POST body or the GET answer: room for the longest switch Cruller keeps, every name full of
    // quotes to escape; and the switch it's made from, or the message parsed.
    struct {
        char buf[8192];
        union {
            svs_switch_t sw;
            svs_msg_t m;
        };
    } svs;
    // gameID: the consoles' JSON in (POST) or out (GET), a game in, or each game out.
    struct {
        char buf[8192];
    } gameid;
#if CRULLER_DEBUG
    char usbtrace[12288];
#endif
    char console[1600];
    char freeze[4096];
    char scan[1600];
} scratch;

// --- helpers -----------------------------------------------------------------------------------

static bool send_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        const int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static const char *reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "";
    }
}

// A response's status line and headers, for len bytes of type (NULL: no body), extra headers (each
// ending in \r\n) before the two every response has. Nothing is cached, and the connection closes.
static bool send_head(int fd, int code, const char *type, size_t len, const char *extra) {
    char hdr[4 * RTL1_PATH_MAX + 320]; // (the longest: a download's names, rt4k/get)
    const int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\n%s%s%sContent-Length: %lu\r\n%sCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", code, reason(code), type ? "Content-Type: " : "", type ? type : "", type ? "\r\n" : "",
        (unsigned long)len, extra ? extra : "");
    return n > 0 && (size_t)n < sizeof(hdr) && send_all(fd, hdr, (size_t)n);
}

static void respond(int fd, int code, const char *type, const char *body) {
    const size_t len = body ? strlen(body) : 0;
    if (send_head(fd, code, type, len, NULL) && len) send_all(fd, body, len);
}

// {"ok":false,"error":why} (the API's refusals).
static void respond_error(int fd, int code, const char *why) {
    char out[256];
    size_t n = 0;
    if (!(json_put(out, sizeof(out), &n, "{\"ok\":false,\"error\":") && json_put_str(out, sizeof(out), &n, why) &&
        json_put(out, sizeof(out), &n, "}"))) {
        snprintf(out, sizeof(out), "{\"ok\":false}");
    }
    respond(fd, code, "application/json", out);
}

static void respond_bytes(int fd, const char *extra_headers, const uint8_t *body, size_t len) {
    if (send_head(fd, 200, "application/octet-stream", len, extra_headers) && len) send_all(fd, body, len);
}

// Web assets embedded from src/web at build time (src/web/embed.cmake), each gzipped: sent as it is, for
// the browser to unzip. no-store: with no-cache (and no validators) browsers still ran the previous
// app.js after an update.
#define WEB(name) extern const unsigned char web_##name[]; extern const size_t web_##name##_len;
WEB(index_html) WEB(app_js) WEB(sd_js) WEB(profiles_js) WEB(gameid_js) WEB(editor_js) WEB(mapper_js) WEB(fw_js)
WEB(sha256_js) WEB(rt4k_settings_json)
#undef WEB

#define JS "application/javascript"
static const struct {
    const char *path, *type;
    const unsigned char *data;
    const size_t *len;
} assets[] = {
    {"/", "text/html; charset=utf-8", web_index_html, &web_index_html_len},
    {"/app.js", JS, web_app_js, &web_app_js_len},
    {"/sd.js", JS, web_sd_js, &web_sd_js_len},
    {"/profiles.js", JS, web_profiles_js, &web_profiles_js_len},
    {"/gameid.js", JS, web_gameid_js, &web_gameid_js_len},
    {"/editor.js", JS, web_editor_js, &web_editor_js_len},
    {"/mapper.js", JS, web_mapper_js, &web_mapper_js_len},
    {"/fw.js", JS, web_fw_js, &web_fw_js_len},
    {"/sha256.js", JS, web_sha256_js, &web_sha256_js_len},
    {"/rt4k_settings.json", "application/json", web_rt4k_settings_json, &web_rt4k_settings_json_len}, // (mapper.js, editor.js)
};
#undef JS

// The asset at path, sent; false when there's none.
static bool respond_asset(int fd, const char *path) {
    for (size_t i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
        if (strcmp(path, assets[i].path)) continue;
        if (send_head(fd, 200, assets[i].type, *assets[i].len, "Content-Encoding: gzip\r\n")) send_all(fd, assets[i].data, *assets[i].len);
        return true;
    }
    return false;
}

// Whether the request came in through the setup access point (192.168.4.1), real or test portal.
static bool via_portal(int fd) {
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    return getsockname(fd, (struct sockaddr *)&local, &len) == 0 && local.sin_addr.s_addr == PP_HTONL(0xC0A80401u);
}

// Clients sending "Expect: 100-continue" (curl does above 1 MiB) wait for this before the body.
// Without it they paused ~1 s, and the board hung during that pause (root cause still unknown).
static bool get_header(const request_t *r, const char *name, char *out, size_t size);
static void continue_if_expected(const request_t *r) {
    char expect[24];
    if (get_header(r, "Expect", expect, sizeof(expect)) && !strcasecmp(expect, "100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(r->fd, cont, sizeof(cont) - 1);
    }
}

static bool read_request(request_t *r) {
    r->head_len = 0;
    r->content_length = 0;
    for (;;) {
        if (r->head_len >= sizeof(r->head) - 1) return false;
        const int n = recv(r->fd, r->head + r->head_len, sizeof(r->head) - 1 - r->head_len, 0);
        if (n <= 0) return false;
        r->head_len += (size_t)n;
        r->head[r->head_len] = 0;
        char *end = strstr(r->head, "\r\n\r\n");
        if (!end) continue;
        r->body_start = (size_t)(end + 4 - r->head);
        *end = 0; // headers are now a C string; body bytes (if any) follow after the terminator
        break;
    }
    if (sscanf(r->head, "%7s %511s", r->method, r->path) != 2) return false;
    for (char *line = strstr(r->head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (!strncasecmp(line + 2, "Content-Length:", 15)) r->content_length = strtol(line + 17, NULL, 10);
    }
    return true;
}

// Copies the value of header `name` (case-insensitive) into out; false if absent.
static bool get_header(const request_t *r, const char *name, char *out, size_t size) {
    const size_t n = strlen(name);
    for (const char *line = strstr(r->head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, name, n) || line[2 + n] != ':') continue;
        const char *v = line + 3 + n;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = strcspn(v, "\r");
        if (len >= size) len = size - 1;
        memcpy(out, v, len);
        out[len] = 0;
        return true;
    }
    return false;
}

// Streams the request body (content_length bytes) to sink(); false when the connection drops.
static bool read_body(request_t *r, bool (*sink)(const uint8_t *, size_t, void *), void *ctx) {
    long remaining = r->content_length;
    const size_t early = r->head_len - r->body_start;
    if (early) {
        const size_t take = early > (size_t)remaining ? (size_t)remaining : early;
        if (!sink((const uint8_t *)r->head + r->body_start, take, ctx)) return false;
        remaining -= (long)take;
    }
    static uint8_t buf[BODY_CHUNK];
    while (remaining > 0) {
        const int n = recv(r->fd, buf, remaining < (long)sizeof(buf) ? (size_t)remaining : sizeof(buf), 0);
        if (n <= 0) return false;
        if (!sink(buf, (size_t)n, ctx)) return false;
        remaining -= n;
    }
    return true;
}

typedef struct {
    uint8_t *buf;
    size_t len, size;
} body_buf_t;

static bool body_buf_sink(const uint8_t *data, size_t len, void *ctx) {
    body_buf_t *b = ctx;
    if (b->len + len >= b->size) return false;
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    return true;
}

static bool form_field(const char *body, const char *name, char *out, size_t size);

// The body read whole into buf (size bytes, its last kept for a 0 after it): its length, or -1 when
// there's none, it doesn't fit, or the connection dropped (the handler says which it takes).
static long body_into(request_t *r, void *buf, size_t size) {
    body_buf_t b = {buf, 0, size};
    if (r->content_length <= 0 || r->content_length >= (long)size || !read_body(r, body_buf_sink, &b)) return -1;
    b.buf[b.len] = 0;
    return (long)b.len;
}

// --- pages -------------------------------------------------------------------------------------

static const char *state_name(net_state_t s) {
    switch (s) {
        case NET_JOINING: return "joining";
        case NET_CONNECTED: return "connected";
        case NET_PORTAL: return "setup portal";
        default: return "starting";
    }
}

// Debug: GET /debug/tasks: where the ws/mirror tasks are and the link counters; with ?stacks, every
// task's state too (its stack's high-water mark in words on the Pico, bytes on the ESP32); ?json, only
// the tasks, as JSON ({"tasks":[{"name","state","prio","stack_free"}]}, stack_free in bytes: the Debug
// tab's). Only on request: uxTaskGetSystemState() suspends the scheduler on both cores while it scans
// every stack, for milliseconds, and the rt4k task missed USB packets (FT232R overruns).
static void handle_debug_tasks(int fd, const char *query) {
    char *const out = scratch.debug_tasks.out;
    const size_t size = sizeof(scratch.debug_tasks.out);
    const bool json = query && strstr(query, "json");
    if (!json) ws_debug(out, size);
    if (!json && (!query || !strstr(query, "stacks"))) {
        respond(fd, 200, "text/plain", out);
        return;
    }
    TaskStatus_t *const tasks = scratch.debug_tasks.tasks;
    const UBaseType_t n = uxTaskGetSystemState(tasks, sizeof(scratch.debug_tasks.tasks) / sizeof(tasks[0]), NULL);
    static const char *states[] = {"running", "ready", "blocked", "suspended", "deleted", "invalid"};
    size_t o = json ? 0 : strlen(out);
    bool ok = !json || json_put(out, size, &o, "{\"tasks\":[");
    for (UBaseType_t i = 0; ok && i < n; i++) {
        const unsigned st = tasks[i].eCurrentState <= eInvalid ? (unsigned)tasks[i].eCurrentState : 5u;
        if (json) {
            ok = json_putf(out, size, &o, "%s{\"name\":", i ? "," : "") && json_put_str(out, size, &o, tasks[i].pcTaskName) &&
                json_putf(out, size, &o, ",\"state\":\"%s\",\"prio\":%lu,\"stack_free\":%lu}", states[st], (unsigned long)tasks[i].uxCurrentPriority,
                    (unsigned long)(tasks[i].usStackHighWaterMark * sizeof(StackType_t)));
        } else {
            ok = json_putf(out, size, &o, "%-12s %-9s prio %lu stack free %lu\n", tasks[i].pcTaskName, states[st],
                (unsigned long)tasks[i].uxCurrentPriority, (unsigned long)tasks[i].usStackHighWaterMark);
        }
    }
    if (json) {
        if (ok && json_put(out, size, &o, "]}")) respond(fd, 200, "application/json", out);
        else respond_error(fd, 500, "too many tasks to list");
    } else {
        respond(fd, 200, "text/plain", out);
    }
}

// GET /debug/memory: clients against their limits, lwIP's pools and heap, the FreeRTOS heap, RAM.
static const struct {
    int id;
    const char *name;
} pools[] = {
    {MEMP_NETCONN, "sockets (NETCONN)"},
    {MEMP_TCP_PCB, "TCP connections"},
    {MEMP_TCP_PCB_LISTEN, "TCP listeners"},
    {MEMP_TCP_SEG, "TCP segments"},
    {MEMP_PBUF_POOL, "packet buffers"},
    {MEMP_NETBUF, "netbufs"},
    {MEMP_UDP_PCB, "UDP PCBs"},
    {MEMP_SYS_TIMEOUT, "timeouts"},
};

// lwIP's figures where it keeps them (MEMP_STATS, MEM_STATS): ESP-IDF's lwIP takes pools and heap from
// the system heap and has neither, so zeros there.
static struct stats_mem pool_stats(int id) {
#if MEMP_STATS
    return *lwip_stats.memp[id];
#else
    (void)id;
    return (struct stats_mem){0};
#endif
}

static struct stats_mem heap_stats(void) {
#if MEM_STATS
    return lwip_stats.mem;
#else
    return (struct stats_mem){0};
#endif
}

static void handle_debug_memory(int fd) {
    http_debug_memory(scratch.debug_memory, sizeof(scratch.debug_memory));
    respond(fd, 200, "text/plain", scratch.debug_memory);
}

void http_debug_memory(char *out, size_t size) {
    size_t o = 0;
    out[0] = 0;
#define ADD(...) o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__)
    const int ws_n = ws_clients(), rfc_n = rfc2217_count();
    ADD("clients                 %d of %d, shared (see clients.h)\n", clients_used(), CLIENTS_MAX);
    ADD("  web pages (WebSocket)   %d (when full, a new one replaces the quietest page)\n", ws_n);
    ADD("  API events (WebSocket)  %d of %d at most (a new one replaces the quietest)\n", ws_event_clients(),
        WS_EVENTS_MAX);
    ADD("  RFC 2217 (port 2217)    %d (when full, a new one replaces the oldest RFC 2217 client)\n", rfc_n);
    ADD("  HTTP                    1 request at a time, %d more waiting\n", HTTP_BACKLOG);
    ADD("  SD card transfers       1 at a time on their own task, %d more waiting\n", XFER_QUEUE);

    ADD("\nlwIP pools              used  peak  size  failed\n");
    for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
        const struct stats_mem m = pool_stats(pools[i].id);
        ADD("  %-21s %5u %5u %5u %7lu\n", pools[i].name, (unsigned)m.used, (unsigned)m.max, (unsigned)m.avail,
            (unsigned long)m.err);
    }
    const struct stats_mem lh = heap_stats();
    ADD("  %-21s %5u %5u %5u %7lu  (bytes)\n", "lwIP heap", (unsigned)lh.used, (unsigned)lh.max, (unsigned)lh.avail,
        (unsigned long)lh.err);

    plat_memory_t m;
    plat_memory(&m);
    ADD("\nFreeRTOS heap           %u KB: free %u, lowest ever %u, largest block %u (bytes)\n",
        (unsigned)(m.heap_size / 1024), (unsigned)m.heap_free, (unsigned)m.heap_lowest, (unsigned)m.heap_largest);
    const uint32_t rest = m.ram_total - m.ram_data - m.ram_bss;
    ADD("RAM                     %u KB: code and data %u KB, bss %u KB (FreeRTOS heap included), rest %u KB\n",
        (unsigned)(m.ram_total / 1024), (unsigned)(m.ram_data / 1024), (unsigned)(m.ram_bss / 1024),
        (unsigned)(rest / 1024));
#undef ADD
}

// GET /debug/tcp: every TCP connection lwIP holds (not the listeners), with its peer, and how many
// each peer has. Cruller closes first ("Connection: close"), so each HTTP request leaves its PCB in
// TIME_WAIT for 2 * TCP_MSL (2 minutes); lwIP reuses the oldest of those when the pool runs out.
#define TCP_LIST_MAX 24 // the Pico 2 W's whole pool

typedef struct {
    uint8_t state;
    uint16_t local_port, remote_port;
    ip_addr_t remote;
    uint32_t idle_ms; // since its last activity (in TIME_WAIT: since it closed)
} tcp_conn_t;

// Copies the PCBs out under the core lock; returns how many there are (can be more than max).
static size_t tcp_conns(tcp_conn_t *out, size_t max) {
    size_t n = 0;
    LOCK_TCPIP_CORE();
    struct tcp_pcb *const lists[] = {tcp_active_pcbs, tcp_tw_pcbs};
    for (size_t l = 0; l < 2; l++) {
        for (const struct tcp_pcb *p = lists[l]; p; p = p->next, n++) {
            if (n < max) {
                out[n] = (tcp_conn_t){(uint8_t)p->state, p->local_port, p->remote_port, p->remote_ip,
                    (tcp_ticks - p->tmr) * TCP_SLOW_INTERVAL};
            }
        }
    }
    UNLOCK_TCPIP_CORE();
    return n;
}

static void handle_debug_tcp(int fd) {
    static const char *const states[] = {"CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
        "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"};
    // On the stack, not static: the Pico 2 W's RAM has no 3 KB to spare, the HTTP task's stack does.
    tcp_conn_t conns[TCP_LIST_MAX];
    char out[2560];
    const size_t total = tcp_conns(conns, TCP_LIST_MAX);
    const size_t n = total < TCP_LIST_MAX ? total : TCP_LIST_MAX;
    size_t o = 0;
    out[0] = 0;
#define ADD(...) o += (size_t)snprintf(out + o, o < sizeof(out) ? sizeof(out) - o : 0, __VA_ARGS__)
    ADD("TCP connections: %u of %d (TIME_WAIT ones last 2 minutes after closing)\n", (unsigned)total, MEMP_NUM_TCP_PCB);

    // Per peer: all its connections and how many of them are in TIME_WAIT.
    ADD("\npeer              total  TIME_WAIT\n");
    bool counted[TCP_LIST_MAX] = {false};
    for (size_t i = 0; i < n; i++) {
        if (counted[i]) continue;
        unsigned all = 0, tw = 0;
        for (size_t j = i; j < n; j++) {
            if (counted[j] || !ip_addr_cmp(&conns[j].remote, &conns[i].remote)) continue;
            counted[j] = true;
            all++;
            tw += conns[j].state == TIME_WAIT;
        }
        char ip[IPADDR_STRLEN_MAX];
        ipaddr_ntoa_r(&conns[i].remote, ip, sizeof(ip));
        ADD("%-17s %5u %10u\n", ip, all, tw);
    }

    ADD("\nstate        local  peer                   idle\n");
    for (size_t i = 0; i < n; i++) {
        char ip[IPADDR_STRLEN_MAX], peer[IPADDR_STRLEN_MAX + 8];
        ipaddr_ntoa_r(&conns[i].remote, ip, sizeof(ip));
        snprintf(peer, sizeof(peer), "%s:%u", ip, conns[i].remote_port);
        const char *st = conns[i].state < sizeof(states) / sizeof(states[0]) ? states[conns[i].state] : "?";
        ADD("%-12s %5u  %-21s %5lu.%lu s\n", st, conns[i].local_port, peer, (unsigned long)(conns[i].idle_ms / 1000),
            (unsigned long)(conns[i].idle_ms % 1000 / 100));
    }
    if (total > n) ADD("... and %u more\n", (unsigned)(total - n));
#undef ADD
    respond(fd, 200, "text/plain", out);
}

size_t http_debug_memory_json(char *out, size_t size) {
    size_t o = 0;
#define ADD(...) o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__)
    ADD("{\"clients\":{\"used\":%d,\"max\":%d,\"web\":%d,\"events\":%d,\"rfc2217\":%d,\"http_waiting\":%d},\"pools\":[",
        clients_used(), CLIENTS_MAX, ws_clients(), ws_event_clients(), rfc2217_count(), HTTP_BACKLOG);
    for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
        const struct stats_mem m = pool_stats(pools[i].id);
        ADD("%s{\"name\":\"%s\",\"used\":%u,\"peak\":%u,\"size\":%u,\"failed\":%lu}", i ? "," : "", pools[i].name,
            (unsigned)m.used, (unsigned)m.max, (unsigned)m.avail, (unsigned long)m.err);
    }
    const struct stats_mem lh = heap_stats();
    plat_memory_t mem;
    plat_memory(&mem);
    ADD("],\"lwip_heap\":{\"used\":%u,\"peak\":%u,\"size\":%u,\"failed\":%lu},"
        "\"heap\":{\"size\":%u,\"free\":%u,\"lowest\":%u,\"largest\":%u}}",
        (unsigned)lh.used, (unsigned)lh.max, (unsigned)lh.avail, (unsigned long)lh.err, (unsigned)mem.heap_size, (unsigned)mem.heap_free,
        (unsigned)mem.heap_lowest, (unsigned)mem.heap_largest);
#undef ADD
    return o < size ? o : 0;
}

size_t http_debug_sensors_json(char *out, size_t size, uint32_t *seq) {
    health_sensors_t hs;
    if (!health_sensors(&hs) || !hs.supply) return (size_t)snprintf(out, size, "null") < size ? 4 : 0;
    health_sample_t s[24];
    const size_t n = health_sensor_samples(seq, s, sizeof(s) / sizeof(s[0]));
    size_t o = 0;
#define ADD(...) o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__)
    ADD("{\"supply_mv\":%lu,\"supply_min_mv\":%lu,\"usb_power\":%d,\"temperature_dc\":%ld,\"samples\":[",
        (unsigned long)hs.supply_mv, (unsigned long)hs.supply_min_mv, hs.usb_power, (long)hs.temperature_dc);
    // Oldest first, 100 ms apart: [average mV, lowest mV, temperature in tenths of °C].
    for (size_t i = 0; i < n; i++) ADD("%s[%u,%u,%d]", i ? "," : "", s[i].supply_mv, s[i].supply_min_mv, s[i].temperature_dc);
    ADD("]}");
#undef ADD
    return o < size ? o : 0;
}

// GET /ws (the page) or GET /api/v1/events[?types=state,...] (events: Home Assistant, scripts; see ws.h):
// the WebSocket upgrade; the connection then belongs to ws.c.
static void handle_ws(request_t *r, bool events, const char *query) {
    char upgrade[32], key[64], version[8];
    if (!get_header(r, "Upgrade", upgrade, sizeof(upgrade)) || strcasecmp(upgrade, "websocket") ||
        !get_header(r, "Sec-WebSocket-Key", key, sizeof(key)) ||
        !get_header(r, "Sec-WebSocket-Version", version, sizeof(version)) || strcmp(version, "13")) {
        respond(r->fd, 400, "text/plain", "WebSocket upgrade expected\n");
        return;
    }
    if (!ws_has_room(events)) {
        respond(r->fd, 503, "text/plain", "Too many WebSocket clients\n");
        return;
    }
    char accept[29], hdr[160];
    ws_accept_key(key, accept);
    const int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    send_all(r->fd, hdr, (size_t)n);
    char list[128];
    const uint32_t types = query && form_field(query, "types", list, sizeof(list)) ? ws_event_types(list) : WS_EVENT_STATE;
    r->adopted = ws_adopt(r->fd, events, types);
}

// GET /rt4k/xfer?cmd=osd|osd2|font|sget: one RTL1 transfer, verified (CRC, sequence, SHA-256), as the
// raw payload; the RT4K's ready line comes back in X-Ready. sget: the settings it runs on now, a profile
// without its 128-byte header ("sget ready size=22876 ver=..." on 1.9x).
static void handle_rt4k_xfer(int fd, const char *query) {
    char cmd[16] = "";
    if (!query || !form_field(query, "cmd", cmd, sizeof(cmd)) || (strcmp(cmd, "osd") && strcmp(cmd, "osd2") &&
        strcmp(cmd, "font") && strcmp(cmd, "sget"))) {
        respond(fd, 400, "text/plain", "cmd must be osd, osd2, font or sget\n");
        return;
    }
    rtl1_info_t *const info = &scratch.xfer.info;
    const rtl1_result_t r = rtl1_transfer(cmd, scratch.xfer.buf, sizeof(scratch.xfer.buf), info, false, 0);
    if (r != RTL1_OK) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: %s\n", rtl1_result_name(r), info->detail);
        respond(fd, 502, "text/plain", msg);
        return;
    }
    char headers[200];
    snprintf(headers, sizeof(headers), "X-Ready: %s\r\n", info->ready);
    respond_bytes(fd, headers, scratch.xfer.buf, info->len);
}

static void handle_status(int fd) {
    char body[1536];
    http_status_json(body, sizeof(body));
    respond(fd, 200, "application/json", body);
}

static void svs_json(char *out, size_t size, bool full);

#ifndef PLAT_NAME
#define PLAT_NAME "unknown" // a target that doesn't name itself in platform_target.h
#endif

// A part that didn't fit, taken off (from at): the JSON stays whole without it.
static void drop_from(char *out, size_t *n, size_t at) {
    *n = at;
    out[at] = 0;
}

// The status: written forward, each optional part whole or left out, room kept for the closing brace.
void http_status_json(char *body, size_t size) {
    const size_t room = size - 1;
    size_t n = 0, at;
    rt4k_status_t rt;
    rt4k_get_status(&rt);
    plat_memory_t mem;
    plat_memory(&mem);
    char rfc2217_ips[CLIENTS_MAX * 16 + 8];
    rfc2217_clients(rfc2217_ips, sizeof(rfc2217_ips));
    const bool base = json_putf(body, room, &n, "{\"version\":\"%s\",\"uptime_s\":%lu,\"net\":\"%s\",\"ssid\":", cruller_version,
            (unsigned long)(plat_ms() / 1000), state_name(net_state())) &&
        json_put_str(body, room, &n, net_ssid()) &&
        json_putf(body, room, &n, ",\"ip\":\"%s\",\"rssi\":%d,\"boot_partition\":%d,\"boot_type\":\"%s\",\"heap_free\":%u,\"heap_size\":%u,"
            "\"rt4k_usb\":\"%s\",\"rt4k_id\":\"%04x:%04x\",\"rt4k_baud\":%lu,\"rt4k_flow\":%s,"
            "\"rt4k_tx\":%lu,\"rt4k_rx\":%lu,\"rt4k_dropped\":%lu,\"rt4k_power\":\"%s\","
            "\"web_clients\":%d,\"event_clients\":%d,\"rfc2217_count\":%d,\"clients_max\":%d,\"rfc2217_clients\":\"%s\",\"gameid_seq\":%lu",
            net_ip(), net_rssi(), ota_boot_partition(), ota_last_boot_type(), (unsigned)mem.heap_free, (unsigned)mem.heap_size,
            rt.mounted ? "connected" : "not connected", rt.vid, rt.pid, (unsigned long)rt.baud, rt4k_flow_control() ? "true" : "false",
            (unsigned long)rt.tx_bytes, (unsigned long)rt.rx_bytes, (unsigned long)rt.tx_dropped,
            rt.mounted ? power_state_name(power_state()) : "unknown", ws_clients(), ws_event_clients(), rfc2217_count(),
            CLIENTS_MAX, rfc2217_ips, (unsigned long)gameid_state_seq());
    if (!base) {
        snprintf(body, size, "{}");
        return;
    }
    // An upload to the RT4K's SD card: "put":{"path","sent","size"} (the page's progress bar).
    char path[SD_PATH_MAX + 1];
    uint32_t sent, total;
    if (rtl1_put_progress(path, sizeof(path), &sent, &total)) {
        at = n;
        if (!(json_put(body, room, &n, ",\"put\":{\"path\":") && json_put_str(body, room, &n, path) &&
            json_putf(body, room, &n, ",\"sent\":%lu,\"size\":%lu}", (unsigned long)sent, (unsigned long)total))) drop_from(body, &n, at);
    }
    // A firmware update: "update":{"got","size"} while one is uploaded, or Cruller's own download
    // from GitHub (ota_fetch.h): the same with "from":"github" (and "done"), or its "failed".
    uint32_t got, want;
    char fetch[200];
    if (http_update_progress(&got, &want)) json_putf(body, room, &n, ",\"update\":{\"got\":%lu,\"size\":%lu}", (unsigned long)got, (unsigned long)want);
    else if (ota_fetch_json(fetch, sizeof(fetch))) json_putf(body, room, &n, ",\"update\":%s", fetch);
    // The platform: which image the firmware index has for it ("rp2": a .uf2). Whether the build has the
    // developer tools (CRULLER_DEBUG): the Debug tab shows their buttons only then.
    json_putf(body, room, &n, ",\"platform\":\"%s\",\"dev_tools\":%s", PLAT_NAME, CRULLER_DEBUG ? "true" : "false");
    // The RT4K's firmware and model as it last said them (rt4k_info.h), and whether it said them since
    // it last came on (else they're from before: it sleeps, or hasn't answered yet).
    rt4k_info_t info;
    const bool fresh = rt4k_info_get(&info);
    at = n;
    if (!(json_put(body, room, &n, ",\"rt4k_fw\":") && json_put_str(body, room, &n, info.version) && json_put(body, room, &n, ",\"rt4k_model\":") &&
        json_put_str(body, room, &n, info.model) && json_putf(body, room, &n, ",\"rt4k_fw_fresh\":%s", fresh ? "true" : "false"))) drop_from(body, &n, at);
    // The profile it has loaded (rt4k_info.h): "rt4k_profile", its path under /profile ("": none; null:
    // not known), when there's room (else the page asks itself).
    char profile[RT4K_INFO_PROFILE_MAX + 1];
    at = n;
    if (!rt4k_info_profile(profile, sizeof(profile))) json_put(body, room, &n, ",\"rt4k_profile\":null");
    else if (!(json_put(body, room, &n, ",\"rt4k_profile\":") && json_put_str(body, room, &n, profile))) drop_from(body, &n, at);
    // The switch's active input once its bridge has reported one, or the paired bridge:
    // "svs":{"input","name","paired",...}.
    svs_state_t s;
    settings_t set;
    settings_get(&set);
    if (svs_get(&s) || set.svs_bridge[0]) {
        char svs[640];
        svs_json(svs, sizeof(svs), false);
        json_putf(body, room, &n, ",\"svs\":%s", svs);
    }
    // Its name and host name, and the setup wizard's join while one runs or has run.
    at = n;
    if (!(json_put(body, room, &n, ",\"name\":") && json_put_str(body, room, &n, set.name) &&
        json_putf(body, room, &n, ",\"hostname\":\"%s\"", net_hostname()))) drop_from(body, &n, at);
    char setup[320];
    if (net_state() == NET_PORTAL && net_setup_json(setup, sizeof(setup)) && !strstr(setup, "\"idle\"")) json_putf(body, room, &n, ",\"setup\":%s", setup);
    body[n++] = '}';
    body[n] = 0;
}

// GET <path>?since=N: text written after position N, with the new position in X-Next.
static void handle_stream(int fd, const char *query, size_t (*reader)(uint32_t *, char *, size_t)) {
    char since[12], next[32];
    uint32_t pos = query && form_field(query, "since", since, sizeof(since)) ? (uint32_t)strtoul(since, NULL, 10) : 0;
    const size_t n = reader(&pos, scratch.stream, sizeof(scratch.stream));
    snprintf(next, sizeof(next), "X-Next: %lu\r\n", (unsigned long)pos);
    if (send_head(fd, 200, "text/plain; charset=utf-8", n, next) && n) send_all(fd, scratch.stream, n);
}

// The firmware upload in progress, for the page's progress bar (like "put": the browser's own upload
// progress only counts what it buffered).
static volatile bool update_active;
static volatile uint32_t update_got, update_size;

bool http_update_progress(uint32_t *got, uint32_t *size) {
    if (!update_active) return false;
    *got = update_got;
    *size = update_size;
    return true;
}

static bool ota_sink(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    update_got += (uint32_t)len;
    return ota_feed(data, len);
}

static void handle_update(request_t *r) {
    if (ota_fetch_running()) {
        respond(r->fd, 409, "text/plain", "Cruller is downloading an update from GitHub\n");
        return;
    }
    if (r->content_length <= 0) {
        respond(r->fd, 411, "text/plain", "Send the .uf2 file as the request body\n");
        return;
    }
    printf("http: firmware upload, %ld bytes\n", r->content_length);
    continue_if_expected(r);
    ota_begin();
    update_got = 0;
    update_size = (uint32_t)r->content_length;
    update_active = true;
    const bool received = read_body(r, ota_sink, NULL);
    update_active = false;
    if (!received || !ota_finish()) {
        ota_abort();
        char msg[96];
        snprintf(msg, sizeof(msg), "Update failed: %s\n", received ? ota_error() : "connection lost");
        respond(r->fd, 400, "text/plain", msg);
        return;
    }
    respond(r->fd, 200, "text/plain", "Update written, rebooting into it\n");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response leave
    ota_reboot_into_update();
}

// POST /update/fetch?url=<release asset>&sha=<SHA-256 hex>&size=<bytes>: Cruller downloads the image
// from GitHub itself and restarts into it (ota_fetch.h). Answers at once; the status has the progress.
static void handle_update_fetch(request_t *r, const char *query) {
    char url[384], sha[72], size[16];
    const char *why = "need ?url=, sha= and size=";
    if (query && form_field(query, "url", url, sizeof(url)) && form_field(query, "sha", sha, sizeof(sha)) &&
        form_field(query, "size", size, sizeof(size)) && ota_fetch_start(url, sha, (uint32_t)strtoul(size, NULL, 10), &why)) {
        respond(r->fd, 202, "text/plain", "Downloading\n");
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "%s\n", why);
    respond(r->fd, ota_fetch_running() ? 409 : 400, "text/plain", msg);
}

#if CRULLER_DEBUG
// Debug: POST /debug/raw[?pause=S]: the body goes to the RT4K as is (no framing), and transfers
// (the mirror's polls) are refused for S seconds (default 10) so they don't interleave. For working
// out protocols such as RTL1 put from a PC; replies show up in /rt4k/rx.
static void handle_debug_raw(request_t *r, const char *query) {
    char pause[12];
    rtl1_pause((query && form_field(query, "pause", pause, sizeof(pause)) ? (uint32_t)strtoul(pause, NULL, 10) : 10) * 1000);
    const long len = body_into(r, scratch.raw, sizeof(scratch.raw));
    if (len < 0) {
        respond(r->fd, 400, "text/plain", "Send up to 4608 bytes as the body\n");
        return;
    }
    if (!rt4k_link_lock(2000)) {
        respond(r->fd, 503, "text/plain", "link busy\n");
        return;
    }
    // In chunks: the TX queue (2048 bytes) is smaller than a full frame.
    size_t sent = 0;
    for (int tries = 0; sent < (size_t)len && tries < 500; tries++) {
        const size_t n = (size_t)len - sent < 512 ? (size_t)len - sent : 512;
        if (rt4k_write(scratch.raw + sent, n)) sent += n;
        else vTaskDelay(pdMS_TO_TICKS(2));
    }
    rt4k_link_unlock();
    char msg[48];
    snprintf(msg, sizeof(msg), "sent %u of %ld bytes\n", (unsigned)sent, len);
    respond(r->fd, sent == (size_t)len ? 200 : 503, "text/plain", msg);
}
#endif

// Decodes application/x-www-form-urlencoded field `name` into out.
static bool form_field(const char *body, const char *name, char *out, size_t size) {
    const size_t nlen = strlen(name);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, name, nlen) || p[nlen] != '=') continue;
        p += nlen + 1;
        size_t n = 0;
        while (*p && *p != '&') {
            char ch = *p++;
            if (ch == '+') ch = ' ';
            else if (ch == '%' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
                const char hex[3] = {p[0], p[1], 0};
                ch = (char)strtol(hex, NULL, 16);
                p += 2;
            }
            if (n + 1 >= size) return false;
            out[n++] = ch;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

// Pulls a request body, for rtl1_put(): first the bytes read along with the headers, then the socket.
typedef struct {
    request_t *r;
    long remaining;
    size_t early; // offset into the bytes that came with the headers
} body_reader_t;

static size_t body_read(void *ctx, uint8_t *buf, size_t max) {
    body_reader_t *b = ctx;
    if (b->remaining <= 0) return 0;
    if ((long)max > b->remaining) max = (size_t)b->remaining;
    const size_t early_left = b->r->head_len - b->r->body_start - b->early;
    if (early_left) {
        const size_t n = early_left < max ? early_left : max;
        memcpy(buf, b->r->head + b->r->body_start + b->early, n);
        b->early += n;
        b->remaining -= (long)n;
        return n;
    }
    const int n = recv(b->r->fd, buf, max, 0);
    if (n <= 0) return 0;
    b->remaining -= n;
    return (size_t)n;
}

// A path on the RT4K's SD card, relative to its root: no control characters, so it stays one console
// line (spaces and brackets are fine: firmware zips have "lumacode/NES/PVM Style D93 (FBX).lmc", and
// so are UTF-8 names), no "..", no backslash. A leading '/' is taken off ("/profile/x.rt4" is
// "profile/x.rt4"), in place.
static bool sd_path_ok(char *p) {
    const size_t lead = strspn(p, "/");
    if (lead) memmove(p, p + lead, strlen(p + lead) + 1);
    if (!*p || strstr(p, "..")) return false;
    for (; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || c == '\\') return false;
    }
    return true;
}

// A refused upload's body read and dropped (up to 256 KB) before the answer: closing with it unread
// resets the connection, and the client sees that instead of why.
static void drop_body(request_t *r) {
    body_reader_t reader = {r, r->content_length < 256 * 1024 ? r->content_length : 256 * 1024, 0};
    uint8_t buf[512];
    while (body_read(&reader, buf, sizeof(buf))) {}
}

// POST /rt4k/put?path=<sd path>&sha=<sha256 hex>: writes the body to the RT4K's SD card.
static void handle_rt4k_put(request_t *r, const char *query) {
    char path[SD_PATH_MAX + 1], sha[72];
    if (!query || !form_field(query, "path", path, sizeof(path)) || !sd_path_ok(path) ||
        !form_field(query, "sha", sha, sizeof(sha)) || strlen(sha) != 64 || strspn(sha, "0123456789abcdefABCDEF") != 64 ||
        r->content_length <= 0) {
        drop_body(r);
        respond(r->fd, 400, "text/plain", "Need ?path=<file>&sha=<sha256 hex> and the file as the body\n");
        return;
    }
    continue_if_expected(r);
    printf("http: RT4K upload %s, %ld bytes\n", path, r->content_length);
    body_reader_t reader = {r, r->content_length, 0};
    static rtl1_info_t info;
    const uint32_t t0 = plat_ms();
    const rtl1_result_t res = rtl1_put(path, (uint32_t)r->content_length, sha, body_read, &reader, &info);
    const uint32_t ms = plat_ms() - t0;
    char msg[320];
    if (res == RTL1_OK) {
        snprintf(msg, sizeof(msg), "ok %s %ld bytes in %lu ms (%s)\n", path, r->content_length, (unsigned long)ms,
            info.detail);
        respond(r->fd, 200, "text/plain", msg);
    } else {
        snprintf(msg, sizeof(msg), "%s: %s\n", rtl1_result_name(res), info.detail);
        respond(r->fd, res == RTL1_ERR_DEVICE ? 409 : 502, "text/plain", msg);
    }
    printf("http: RT4K upload %s: %s", path, msg);
}

// --- the RT4K's SD card, for the page's file browser -------------------------------------------------

#define GET_CHUNK   16384  // bytes per RTL1 get; each piece is verified before it goes out

// A folder listing being collected: console_run() hands over each reply line of "ls <dir>".
typedef struct {
    char *buf;
    size_t len, size;
    int entries;        // "ent" lines seen
    long total;         // from "ls end <n>" (-1: not seen)
    bool truncated;     // buf ran out
    bool cut;           // the last line was as long as a console line gets: the next may be its rest
    char err[64];       // "ls err=2 NOSUCH", "ls: ..."
} ls_t;

static void ls_append(ls_t *l, const char *s, size_t n) {
    if (l->len + n > l->size) {
        l->truncated = true;
        return;
    }
    memcpy(l->buf + l->len, s, n);
    l->len += n;
}

// "[COM] ent t=D sz=0 mt=1762343404 nm=Sony PS2" -> "D\t0\t1762343404\tSony PS2\n".
static void ls_line(const char *line, void *ctx) {
    ls_t *l = ctx;
    const bool was_cut = l->cut;
    l->cut = strlen(line) >= CON_LINE_MAX - 1;
    if (strncmp(line, "[COM] ", 6)) {
        // The rest of an entry whose line was cut (names over ~115 characters): its name goes on.
        if (was_cut && !l->truncated && l->len && l->buf[l->len - 1] == '\n') {
            l->len--;
            ls_append(l, line, strlen(line));
            ls_append(l, "\n", 1);
        }
        return;
    }
    line += 6;
    char type;
    unsigned long size, mtime;
    int name = 0;
    if (!strncmp(line, "ent ", 4)) {
        if (sscanf(line + 4, "t=%c sz=%lu mt=%lu nm=%n", &type, &size, &mtime, &name) != 3 || !name) return;
        l->entries++;
        if (l->truncated) return;
        char head[40];
        const int h = snprintf(head, sizeof(head), "%c\t%lu\t%lu\t", type, size, mtime);
        const size_t n = strlen(line + 4 + name);
        if (l->len + (size_t)h + n + 1 > l->size) {
            l->truncated = true;
            return;
        }
        ls_append(l, head, (size_t)h);
        ls_append(l, line + 4 + name, n);
        ls_append(l, "\n", 1);
    } else if (!strncmp(line, "ls end ", 7)) {
        l->total = strtol(line + 7, NULL, 10);
    } else if (!strncmp(line, "ls", 2)) {
        snprintf(l->err, sizeof(l->err), "%s", line);
    }
}

// GET /rt4k/ls?dir=<sd folder> (none: the root): its entries, one a line, "D|F\tsize\tunix time\tname".
// X-Total has the RT4K's own count: more than the lines when the listing didn't fit.
static void handle_rt4k_ls(request_t *r, const char *query) {
    char dir[SD_PATH_MAX + 1] = "";
    const bool given = query && strstr(query, "dir=");
    if (given && (!form_field(query, "dir", dir, sizeof(dir)) || (dir[strspn(dir, "/")] && !sd_path_ok(dir)))) {
        respond(r->fd, 400, "text/plain", "Need ?dir=<folder on the SD card>\n");
        return;
    }
    if (!dir[strspn(dir, "/")]) dir[0] = 0; // "/": the root
    ls_t l = {.buf = scratch.ls, .size = sizeof(scratch.ls)};
    char cmd[SD_PATH_MAX + 8];
    snprintf(cmd, sizeof(cmd), dir[0] ? "ls %s" : "ls", dir);
    bool sent = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        l.len = 0;
        l.entries = 0;
        l.total = -1;
        l.truncated = l.cut = false;
        l.err[0] = 0;
        sent = console_run(CON_FILES, cmd, ls_line, &l);
        // Lines missing (the console's history overran while this task was held up): list it again.
        if (!sent || l.err[0] || l.total < 0 || l.entries == l.total) break;
    }
    char msg[80];
    if (!sent) {
        respond(r->fd, 503, "text/plain", "RT4K not connected, or the link is busy\n");
    } else if (l.err[0]) {
        snprintf(msg, sizeof(msg), "%s\n", l.err);
        respond(r->fd, strstr(l.err, "NOSUCH") ? 404 : 409, "text/plain", msg);
    } else if (l.total < 0) {
        respond(r->fd, 504, "text/plain", "The RT4K didn't answer: is it on?\n");
    } else if (l.entries != l.total) {
        snprintf(msg, sizeof(msg), "The listing came incomplete (%d of %ld entries)\n", l.entries, l.total);
        respond(r->fd, 502, "text/plain", msg);
    } else {
        snprintf(msg, sizeof(msg), "X-Total: %ld\r\n", l.total);
        if (send_head(r->fd, 200, "text/plain; charset=utf-8", l.len, msg) && l.len) send_all(r->fd, l.buf, l.len);
    }
}

// GET /rt4k/get?path=<sd file>: the file, read in GET_CHUNK pieces (RTL1 "get -o <off> -l <len>"), each
// verified (CRC, sequence, SHA-256) before it goes out. The first piece's ready line gives the size;
// a piece failing later cuts the response short, which Content-Length tells the browser.
static void handle_rt4k_get(request_t *r, const char *query) {
    char path[SD_PATH_MAX + 1];
    if (!query || !form_field(query, "path", path, sizeof(path)) || !sd_path_ok(path)) {
        respond(r->fd, 400, "text/plain", "Need ?path=<file on the SD card>\n");
        return;
    }
    uint8_t *buf = pvPortMalloc(GET_CHUNK);
    if (!buf) {
        respond(r->fd, 503, "text/plain", "Out of memory\n");
        return;
    }
    static rtl1_info_t info;
    char cmd[SD_PATH_MAX + 40];
    unsigned long off = 0, total = 0;
    bool started = false;
    const uint32_t t0 = plat_ms();
    for (;;) {
        snprintf(cmd, sizeof(cmd), "get -o %lu -l %u -- %s", off, (unsigned)GET_CHUNK, path);
        const rtl1_result_t res = rtl1_transfer(cmd, buf, GET_CHUNK, &info, true, 0);
        const char *f = strstr(info.ready, "off=");
        unsigned long r_off = 0, r_len = 0, r_total = 0;
        const bool ok = res == RTL1_OK && f && sscanf(f, "off=%lu len=%lu total=%lu", &r_off, &r_len, &r_total) == 3 &&
            r_off == off && r_len == info.len && (!started || r_total == total);
        if (!ok && started) {
            printf("http: RT4K download %s stopped at %lu of %lu bytes: %s: %s\n", path, off, total,
                rtl1_result_name(res), res == RTL1_OK ? "unexpected ready line" : info.detail);
            break;
        }
        // An empty file has no piece at offset 0: "get err: offset past EOF (size=0)".
        const bool empty = !ok && res == RTL1_ERR_DEVICE && strstr(info.detail, "(size=0)");
        if (!ok && !empty) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: %s\n", rtl1_result_name(res), res == RTL1_OK ? "unexpected ready line" : info.detail);
            const bool missing = res == RTL1_ERR_DEVICE && strstr(info.detail, "cannot open");
            respond(r->fd, missing ? 404 : res == RTL1_ERR_DEVICE ? 409 : 502, "text/plain", msg);
            break;
        }
        if (!started) {
            total = empty ? 0 : r_total;
            // The name the browser saves it as: an ASCII stand-in, and the real one (UTF-8, escaped).
            const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            char name[SD_PATH_MAX + 1], utf8[3 * SD_PATH_MAX + 1];
            size_t n = 0, u = 0;
            for (; base[n] && n < sizeof(name) - 1; n++) {
                const unsigned char c = (unsigned char)base[n];
                name[n] = c == '"' || c >= 0x80 ? '_' : (char)c;
                if (isalnum(c) || strchr("-._~", c)) utf8[u++] = (char)c;
                else u += (size_t)snprintf(utf8 + u, sizeof(utf8) - u, "%%%02X", c);
            }
            name[n] = 0;
            utf8[u] = 0;
            char disposition[4 * SD_PATH_MAX + 72]; // (the two names, as long as they get, and the rest: 706)
            snprintf(disposition, sizeof(disposition), "Content-Disposition: attachment; filename=\"%s\"; filename*=UTF-8''%s\r\n", name, utf8);
            if (!send_head(r->fd, 200, "application/octet-stream", total, disposition)) break;
            started = true;
        }
        if (empty || !r_len) break;
        if (!send_all(r->fd, buf, info.len)) break; // the browser went away
        off += info.len;
        if (off >= total) {
            printf("http: RT4K download %s, %lu bytes in %lu ms\n", path, total, (unsigned long)(plat_ms() - t0));
            break;
        }
    }
    vPortFree(buf);
}

// POST /rt4k/ask?expect=<text>[&timeout=<ms>] with a console command as the body: the first reply
// line containing <text> (e.g. "ver" / "FW Version:", "fwup check" / "fwup").
static void handle_rt4k_ask(request_t *r, const char *query) {
    char *const cmd = scratch.form;
    char expect[48], tmo[12];
    if (!query || !form_field(query, "expect", expect, sizeof(expect)) || !expect[0] || body_into(r, cmd, sizeof(scratch.form)) < 0) {
        respond(r->fd, 400, "text/plain", "Need ?expect=<text> and a command as the body\n");
        return;
    }
    for (char *c = cmd; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    uint32_t timeout_ms = form_field(query, "timeout", tmo, sizeof(tmo)) ? (uint32_t)strtoul(tmo, NULL, 10) : 3000;
    if (timeout_ms > 20000) timeout_ms = 20000;
    char line[200];
    if (console_query(cmd, expect, line, sizeof(line), timeout_ms)) {
        strncat(line, "\n", sizeof(line) - strlen(line) - 1);
        respond(r->fd, 200, "text/plain", line);
    } else {
        respond(r->fd, 504, "text/plain", "no reply from the RT4K\n");
    }
}

// --- /api/v1: the API for Home Assistant and scripts (docs/API.md) ------------------------------------
//
// Versioned (HTTP_API_VERSION), so a client written for v1 keeps working as Cruller changes. The page
// has /status, which changes with it; these answer what a client outside Cruller needs. /api/command
// and /api/svs came before the version: they stay as other names of their v1 routes, for the SVS
// Bridges and scripts that use them.

// GET /api/v1/info: who this Cruller is, read once (Home Assistant's config flow).
static void handle_api_info(int fd) {
    char id[33], body[384];
    plat_board_id(id, sizeof(id));
    settings_t set;
    settings_get(&set);
    size_t n = 0;
    if (!(json_putf(body, sizeof(body), &n, "{\"id\":\"%s\",\"name\":", id) && json_put_str(body, sizeof(body), &n, set.name) &&
        json_putf(body, sizeof(body), &n, ",\"hostname\":\"%s\",\"sw_version\":\"%s\",\"platform\":\"%s\",\"api_version\":" HTTP_API_VERSION "}",
            net_hostname(), cruller_version, PLAT_NAME))) {
        respond(fd, 500, "application/json", "{}");
        return;
    }
    respond(fd, 200, "application/json", body);
}

// GET /api/v1/state: the RT4K and Cruller, as they change, for polling (gently: requests are served one
// at a time) or pushed (/api/v1/events, ws.h). Not the SVS: the SVS Bridge's own API tells it. Each
// optional part whole or left out, room kept for the closing braces.
void http_api_state_json(char *body, size_t size) {
    const size_t room = size - 2;
    size_t n = 0, at;
    rt4k_status_t rt;
    rt4k_get_status(&rt);
    if (!json_putf(body, room, &n, "{\"rt4k\":{\"connected\":%s,\"power\":\"%s\"", rt.mounted ? "true" : "false",
            rt.mounted ? power_state_name(power_state()) : "unknown")) {
        snprintf(body, size, "{}");
        return;
    }
    // Its firmware and model as it last said them (rt4k_info.h), kept while it sleeps; each only once known.
    rt4k_info_t info;
    rt4k_info_get(&info);
    at = n;
    if (info.version[0] && !(json_put(body, room, &n, ",\"firmware\":") && json_put_str(body, room, &n, info.version))) drop_from(body, &n, at);
    at = n;
    if (info.model[0] && !(json_put(body, room, &n, ",\"model\":") && json_put_str(body, room, &n, info.model))) drop_from(body, &n, at);
    // The profile it has loaded, its path under /profile ("": none; null: not known, it isn't on).
    char profile[RT4K_INFO_PROFILE_MAX + 1];
    at = n;
    if (!rt4k_info_profile(profile, sizeof(profile))) json_put(body, room, &n, ",\"profile\":null");
    else if (!(json_put(body, room, &n, ",\"profile\":") && json_put_str(body, room, &n, profile))) drop_from(body, &n, at);
    at = n;
    if (!json_putf(body, room, &n, "},\"cruller\":{\"sw_version\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d", cruller_version,
            (unsigned long)(plat_ms() / 1000), net_rssi())) {
        drop_from(body, &n, at);
        json_put(body, size, &n, "}}");
        return;
    }
    // The board's own sensors (health.h), each only where it has it.
    health_sensors_t hs;
    if (health_sensors(&hs)) {
        if (hs.supply) {
            const uint32_t cv = (hs.supply_mv + 5) / 10, min_cv = (hs.supply_min_mv + 5) / 10; // centivolts
            json_putf(body, room, &n, ",\"supply_v\":%lu.%02lu,\"supply_min_v\":%lu.%02lu", (unsigned long)(cv / 100),
                (unsigned long)(cv % 100), (unsigned long)(min_cv / 100), (unsigned long)(min_cv % 100));
        }
        if (hs.usb_power >= 0) json_putf(body, room, &n, ",\"usb_power\":%s", hs.usb_power ? "true" : "false");
        if (hs.temperature) {
            const int32_t t = hs.temperature_dc;
            json_putf(body, room, &n, ",\"temperature_c\":%s%ld.%ld", t < 0 ? "-" : "", (long)(t < 0 ? -t : t) / 10, (long)(t < 0 ? -t : t) % 10);
        }
    }
    json_put(body, size, &n, "}}");
}

static void handle_api_state(int fd) {
    char body[384 + 2 * RT4K_INFO_PROFILE_MAX]; // a long profile path, escaped (handle_status takes more)
    http_api_state_json(body, sizeof(body));
    respond(fd, 200, "application/json", body);
}

// --- POST /api/v1/command (also /api/command) ---------------------------------------------------------
//
// For automations (Home Assistant...): console commands in, their own replies out, through the same
// queue as everything else (console.c), so they never get another sender's replies. Body:
//   {"command": "remote menu"}   {"commands": ["remote menu", "remote down"]}   {"button": "menu"}
// or plain text, one command per line. "button" takes the remote's names as hass-RT4K sends them
// (buttons.h): "menu" -> "remote menu"; "diagnostics" -> "remote diag"; "power_on" -> "pwr on".
// Answer: {"ok":true,"power":"on","results":[{"command":"ver","sent":true,"reply":["[COM] ..."]}]}

typedef struct {
    char *out;
    size_t size, len;
    int lines;
} api_reply_t;

// A reply line, into the command's "reply" (whole, or left out once the replies fill the room).
static void api_line(const char *line, void *ctx) {
    api_reply_t *a = ctx;
    const size_t at = a->len;
    if ((a->lines && !json_put(a->out, a->size, &a->len, ",")) || !json_put_str(a->out, a->size, &a->len, line)) drop_from(a->out, &a->len, at);
    else a->lines++;
}

// The commands in a body: a JSON object's, or plain text's lines. How many (into cmds).
static int api_commands(char *text, size_t len, struct api_command_bufs *b) {
    char (*const cmds)[API_CMD_MAX] = b->cmds;
    int count = 0;
    if (*text != '{') {
        for (const char *p = text; *p && count < API_MAX_COMMANDS;) {
            const size_t n = strcspn(p, "\r\n");
            if (n && n < API_CMD_MAX) {
                memcpy(cmds[count], p, n);
                cmds[count++][n] = 0;
            }
            p += n;
            while (*p == '\r' || *p == '\n') p++;
        }
        return count;
    }
    json_tok_t *const tok = b->tok;
    if (json_parse(text, len, tok, (int)(sizeof(b->tok) / sizeof(b->tok[0]))) < 1 || tok[0].type != JSON_OBJECT) return 0;
    int i = json_get(text, tok, 0, "commands");
    if (i >= 0 && tok[i].type == JSON_ARRAY) {
        for (int k = 0; k < tok[i].size && count < API_MAX_COMMANDS; k++) count += json_str(text, tok, json_at(tok, i, k), cmds[count], API_CMD_MAX);
        return count;
    }
    if (i >= 0 || (i = json_get(text, tok, 0, "command")) >= 0) return json_str(text, tok, i, cmds[0], API_CMD_MAX);
    char button[API_CMD_MAX];
    if ((i = json_get(text, tok, 0, "button")) < 0 || !json_str(text, tok, i, button, sizeof(button))) return 0;
    buttons_command(button, cmds[0], API_CMD_MAX);
    return 1;
}

static void handle_api_command(request_t *r) {
    struct api_command_bufs *const b = &scratch.api_command;
    const long len = body_into(r, b->body, sizeof(b->body));
    if (len < 0) {
        respond(r->fd, 400, "application/json", "{\"ok\":false,\"error\":\"send commands as the body\"}");
        return;
    }
    const int count = api_commands(b->body, (size_t)len, b);
    if (!count) {
        respond(r->fd, 400, "application/json", "{\"ok\":false,\"error\":\"no command (\\\"command\\\", \\\"commands\\\" or \\\"button\\\")\"}");
        return;
    }
    // Each command's part: its replies only as far as room leaves for its end, the next only if it fits.
    const size_t room = sizeof(b->results) - 32;
    size_t n = 0;
    bool all_sent = true;
    for (int i = 0; i < count; i++) {
        char *const cmd = b->cmds[i];
        for (char *c = cmd; *c; c++) if ((unsigned char)*c < 0x20) *c = ' '; // one line each
        const size_t at = n;
        if (!(json_put(b->results, room, &n, i ? ",{\"command\":" : "{\"command\":") && json_put_str(b->results, room, &n, cmd) &&
            json_put(b->results, room, &n, ",\"reply\":["))) {
            drop_from(b->results, &n, at);
            break;
        }
        api_reply_t reply = {b->results, room, n, 0};
        const bool sent = console_run(CON_HTTP, cmd, api_line, &reply);
        n = reply.len;
        json_putf(b->results, sizeof(b->results), &n, "],\"sent\":%s}", sent ? "true" : "false");
        all_sent &= sent;
    }
    // results moved, not printed: GCC 15 can't tell two members of the shared buffers apart (-Wrestrict)
    const int h = snprintf(b->out, sizeof(b->out), "{\"ok\":%s,\"power\":\"%.16s\",\"results\":[",
        all_sent ? "true" : "false", power_state_name(power_state()));
    memmove(b->out + h, b->results, n);
    memcpy(b->out + h + n, "]}", 3);
    respond(r->fd, all_sent ? 200 : 503, "application/json", b->out);
}

// --- /api/v1/svs (also /api/svs): the Scalable Video Switch's active input, and the switch (docs/SVS.md)
//
// POST {"id": "svs-bridge-…", "current_input": 3, "total_inputs": 8, "inputs": [{"kind", "name"}, ...],
// "output": {"kind", "name"}} from the SVS Bridge on every change, when it finds Cruller, and every
// 60 s (svs_proto.h reads it); GET answers what Cruller last heard, the switch included.

#define SVS_BODY_MAX 6144 // 32 inputs with long names fit in about 4.5 KB

// The "svs" object. full: with the switch's description ("switch":{...}) and each input's profile
// ("profiles":{...}), for GET /api/v1/svs; else just their "switch_seq" and "profiles_seq" (the status,
// which the page gets every few seconds: it fetches the rest on a change). Each of those two whole or
// left out, room kept for the closing brace.
static void svs_json(char *out, size_t size, bool full) {
    const size_t room = size - 1;
    settings_t set;
    settings_get(&set);
    svs_state_t s;
    const bool known = svs_get(&s);
    size_t n = 0, at;
    bool ok = json_putf(out, room, &n, "{\"known\":%s,\"paired\":", known ? "true" : "false") && json_put_str(out, room, &n, set.svs_bridge) &&
        json_putf(out, room, &n, ",\"profiles_seq\":%lu", (unsigned long)svs_profiles_seq());
    if (ok && known) {
        const uint32_t now = plat_ms();
        ok = json_putf(out, room, &n, ",\"input\":%d,\"total\":%d,\"name\":", s.input, s.total) && json_put_str(out, room, &n, s.name) &&
            json_put(out, room, &n, ",\"id\":") && json_put_str(out, room, &n, s.id) &&
            json_putf(out, room, &n, ",\"heard_s\":%lu,\"since_s\":%lu,\"switch_seq\":%lu,\"history\":[", (unsigned long)((now - s.at_ms) / 1000),
                (unsigned long)((now - s.changed_ms) / 1000), (unsigned long)s.switch_seq);
        // The last input changes, newest first: [[input, seconds ago], ...].
        for (int i = 0; ok && i < s.history_n; i++) {
            ok = json_putf(out, room, &n, "%s[%d,%lu]", i ? "," : "", s.history[i].input, (unsigned long)((now - s.history[i].at_ms) / 1000));
        }
        ok = ok && json_put(out, room, &n, "]");
        svs_switch_t *const sw = &scratch.svs.sw; // (only the HTTP task asks for the full one)
        if (ok && full && svs_get_switch(sw)) {
            at = n;
            size_t w = 0;
            if (!json_put(out, room, &n, ",\"switch\":") || !(w = svs_switch_json(sw, out + n, room - n))) drop_from(out, &n, at);
            else n += w;
        }
    }
    if (!ok) {
        snprintf(out, size, "{\"known\":false}");
        return;
    }
    if (full) { // ,"profiles":{"1":"S1_PS1.rt4",...} (each input's profile, as kept)
        at = n;
        size_t w = 0;
        if (!json_put(out, room, &n, ",\"profiles\":") || !(w = svs_profiles_json(svs_profiles_text(), out + n, room - n))) drop_from(out, &n, at);
        else n += w;
    }
    out[n++] = '}';
    out[n] = 0;
}

// POST /api/v1/svs/unpair: forget the paired bridge; the next one to report is kept.
static void handle_svs_unpair(request_t *r) {
    settings_t s;
    settings_get(&s);
    s.svs_bridge[0] = 0;
    if (!settings_save(&s)) {
        respond_error(r->fd, 500, "could not save");
        return;
    }
    respond(r->fd, 200, "application/json", "{\"ok\":true}");
}

static void handle_svs(request_t *r, bool post) {
    char *const buf = scratch.svs.buf;
    if (!post) {
        svs_json(buf, sizeof(scratch.svs.buf), true);
        respond(r->fd, 200, "application/json", buf);
        return;
    }
    if (body_into(r, buf, SVS_BODY_MAX + 1) < 0) {
        respond_error(r->fd, r->content_length > SVS_BODY_MAX ? 413 : 400, "send the JSON as the body (6 KB at most)");
        return;
    }
    svs_msg_t *const m = &scratch.svs.m;
    const char *error;
    if (!svs_parse(buf, m, &error)) {
        respond_error(r->fd, 400, error);
        return;
    }
    // Pairing: the first bridge that reports (with an id) is kept; others are turned away, so a bridge
    // set up for another RT4K can't change this one's profiles. POST /api/v1/svs/unpair frees it.
    settings_t s;
    settings_get(&s);
    char out[160];
    size_t n = 0;
    if (s.svs_bridge[0] && strcmp(s.svs_bridge, m->id)) {
        if (!(json_put(out, sizeof(out), &n, "{\"ok\":false,\"error\":\"paired with another SVS Bridge\",\"paired\":") &&
            json_put_str(out, sizeof(out), &n, s.svs_bridge) && json_put(out, sizeof(out), &n, "}"))) {
            snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"paired with another SVS Bridge\"}");
        }
        respond(r->fd, 409, "application/json", out);
        return;
    }
    if (!s.svs_bridge[0] && m->id[0]) {
        snprintf(s.svs_bridge, sizeof(s.svs_bridge), "%s", m->id);
        printf("http: paired with SVS Bridge %s: %s\n", m->id, settings_save(&s) ? "saved" : "NOT saved");
    }
    respond(r->fd, 200, "application/json", svs_report(m) ? "{\"ok\":true,\"changed\":true}" : "{\"ok\":true,\"changed\":false}");
}

// POST /api/v1/svs/profiles: each input's profile as the page read the RT4K's card, a line each:
// "<input>\t<its file in /profile/SVS>" (svs_proto.h; none: no profile anywhere). Kept across restarts,
// the flash written only when they changed; GET /api/v1/svs gives them ("profiles") while the RT4K
// sleeps.
static void handle_svs_profiles(request_t *r) {
    char *const buf = scratch.svs.buf;
    buf[0] = 0; // (an empty body: no profile anywhere)
    if (r->content_length < 0 || (r->content_length > 0 && body_into(r, buf, SVS_PROFILES_MAX) < 0)) {
        respond_error(r->fd, r->content_length >= SVS_PROFILES_MAX ? 413 : 400, "send the profiles as the body, a line each (1 KB at most)");
        return;
    }
    char *text = buf + SVS_PROFILES_MAX; // in order, after the body (smaller than that)
    const char *error;
    bool changed;
    if (!svs_profiles_parse(buf, text, SVS_PROFILES_MAX, &error)) {
        respond_error(r->fd, 400, error);
    } else if (!svs_profiles_set(text, &changed)) {
        respond_error(r->fd, 500, "could not save");
    } else {
        respond(r->fd, 200, "application/json", changed ? "{\"ok\":true,\"changed\":true}" : "{\"ok\":true,\"changed\":false}");
    }
}

static void handle_wifi(request_t *r) {
    char *const form = scratch.form;
    wifi_creds_t creds = {0};
    if (body_into(r, form, sizeof(scratch.form)) < 0 || !form_field(form, "ssid", creds.ssid, sizeof(creds.ssid)) || !creds.ssid[0]) {
        respond(r->fd, 400, "text/plain", "Invalid network name\n");
        return;
    }
    if (!form_field(form, "pass", creds.pass, sizeof(creds.pass))) creds.pass[0] = 0;
    if (!creds_save(&creds)) {
        respond(r->fd, 500, "text/plain", "Could not save the credentials\n");
        return;
    }
    respond(r->fd, 200, "text/html",
        "<html><body style='font-family:sans-serif;background:#111;color:#eee'>"
        "<h3>Saved. Cruller is rebooting and joining the network.</h3></body></html>");
    vTaskDelay(pdMS_TO_TICKS(500));
    plat_reboot();
}

// --- the setup wizard (portal) and the name ------------------------------------------------------------

// POST /setup (form: ssid, pass, name), from the portal's wizard: saves the name, then the net task
// tries the network with the portal still up. GET /setup: how it's going (also pushed in the status).
static void handle_setup(request_t *r, bool post) {
    if (!post) {
        char out[320];
        net_setup_json(out, sizeof(out));
        respond(r->fd, 200, "application/json", out);
        return;
    }
    const char *const form = (const char *)scratch.raw;
    char ssid[CREDS_SSID_MAX + 1] = "", pass[CREDS_PASS_MAX + 1] = "", name[SETTINGS_NAME_MAX + 1] = "";
    if (body_into(r, scratch.raw, 1024) < 0) {
        respond_error(r->fd, 400, "send the form as the body");
        return;
    }
    if (!form_field(form, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        respond_error(r->fd, 400, "pick a network");
        return;
    }
    form_field(form, "pass", pass, sizeof(pass));
    if (form_field(form, "name", name, sizeof(name)) && name[0]) {
        if (!settings_name_ok(name)) {
            respond_error(r->fd, 400, "letters, numbers and spaces only");
            return;
        }
        settings_t s;
        settings_get(&s);
        snprintf(s.name, sizeof(s.name), "%s", name);
        if (!settings_save(&s)) {
            respond_error(r->fd, 500, "could not save the name");
            return;
        }
    }
    if (!net_setup_start(ssid, pass)) {
        respond_error(r->fd, 409, "not in setup, or already trying a network");
        return;
    }
    respond(r->fd, 202, "application/json", "{\"ok\":true}");
}

// POST /settings (form: name): renames Cruller; the new name (cruller-<name>.local) takes effect as it
// restarts, right after answering.
static void handle_settings(request_t *r) {
    char name[SETTINGS_NAME_MAX + 1] = "";
    if (body_into(r, scratch.form, sizeof(scratch.form)) < 0 || !form_field(scratch.form, "name", name, sizeof(name)) ||
        !settings_name_ok(name)) {
        respond(r->fd, 400, "text/plain", "The name takes letters, numbers and spaces (up to 32)\n");
        return;
    }
    settings_t s;
    settings_get(&s);
    snprintf(s.name, sizeof(s.name), "%s", name);
    if (!settings_save(&s)) {
        respond(r->fd, 500, "text/plain", "Could not save the name\n");
        return;
    }
    char host[48], msg[160];
    settings_hostname(name, host, sizeof(host));
    snprintf(msg, sizeof(msg), "Renamed; restarting as %s.local\n", host);
    respond(r->fd, 200, "text/plain", msg);
    printf("http: renamed to \"%s\" (%s.local), restarting\n", name, host);
    vTaskDelay(pdMS_TO_TICKS(500));
    plat_reboot();
}

// --- gameID: its consoles and its games (docs/API.md, src/core/gameid.h) --------------------------------

// The body read whole into scratch.gameid.buf, 0-terminated: its length, or -1 (answered already).
static long gameid_body(request_t *r) {
    const long n = body_into(r, scratch.gameid.buf, sizeof(scratch.gameid.buf));
    if (n < 0) {
        const bool big = r->content_length >= (long)sizeof(scratch.gameid.buf);
        respond_error(r->fd, big ? 413 : 400, big ? "too big (8 KB at most)" : "send it as JSON, the body");
    }
    return n;
}

static void handle_gameid_consoles(request_t *r, bool post) {
    char *const buf = scratch.gameid.buf;
    if (!post) {
        const size_t n = gameid_consoles_get_json(buf, sizeof(scratch.gameid.buf));
        respond(r->fd, n ? 200 : 500, "application/json", n ? buf : "{\"consoles\":[]}");
        return;
    }
    const long n = gameid_body(r);
    if (n < 0) return;
    const char *why;
    if (!gameid_consoles_put_json(buf, (size_t)n, &why)) { respond_error(r->fd, 400, why); return; }
    respond(r->fd, 200, "application/json", "{\"ok\":true}");
}

// GET: every game, {"games": [{"id", "profile", "name"}, ...]}, sent as it's read (the files held, so
// the length counted first holds).
typedef struct {
    int fd;
    bool send, ok;
    size_t len, count;
} games_out_t;

static bool games_out(const gameid_game_t *g, void *ctx) {
    games_out_t *o = ctx;
    char *const item = scratch.gameid.buf;
    item[0] = ',';
    const size_t n = gameid_game_json(g, item + 1, sizeof(scratch.gameid.buf) - 1);
    const size_t k = o->count++ ? n + 1 : n;
    if (o->send) o->ok = send_all(o->fd, o->count > 1 ? item : item + 1, k);
    o->len += k;
    return !o->send || o->ok;
}

static void handle_gameid_games_get(request_t *r) {
    static const char head[] = "{\"games\":[", tail[] = "]}";
    games_out_t o = {r->fd, false, true, 0, 0};
    cfgfs_hold();
    gameid_games_each(games_out, &o);
    if (send_head(r->fd, 200, "application/json", strlen(head) + o.len + strlen(tail), NULL) && send_all(r->fd, head, strlen(head))) {
        o = (games_out_t){r->fd, true, true, 0, 0};
        gameid_games_each(games_out, &o);
        if (o.ok) send_all(r->fd, tail, strlen(tail));
    }
    cfgfs_release();
}

// POST: a game added, or the one with its ID replaced.
static void handle_gameid_games_put(request_t *r) {
    const long n = gameid_body(r);
    if (n < 0) return;
    gameid_game_t g;
    const char *why;
    bool replaced = false;
    if (!gameid_game_parse(scratch.gameid.buf, (size_t)n, &g, &why)) { respond_error(r->fd, 400, why); return; }
    if (!gameid_game_put(&g, &replaced)) { respond_error(r->fd, 400, "could not save it (1000 games at most)"); return; }
    respond(r->fd, 200, "application/json", replaced ? "{\"ok\":true,\"replaced\":true}" : "{\"ok\":true,\"replaced\":false}");
}

// POST {"id"}: that game gone.
static void handle_gameid_games_delete(request_t *r) {
    const long n = gameid_body(r);
    if (n < 0) return;
    json_tok_t tok[4];
    char id[GAMEID_ID_MAX];
    bool found = false;
    if (json_parse(scratch.gameid.buf, (size_t)n, tok, 4) < 1 ||
        !json_str(scratch.gameid.buf, tok, json_get(scratch.gameid.buf, tok, 0, "id"), id, sizeof(id))) {
        respond_error(r->fd, 400, "send {\"id\": the game's ID}");
        return;
    }
    if (!gameid_game_delete(id, &found)) { respond_error(r->fd, 400, "could not save the games"); return; }
    respond(r->fd, 200, "application/json", found ? "{\"ok\":true,\"found\":true}" : "{\"ok\":true,\"found\":false}");
}

// POST /restart: reboots into the current image. POST /factory-reset: forgets the Wi-Fi network, the
// name, the paired SVS Bridge and gameID's consoles and games, and reboots into the setup portal.
static void handle_restart(request_t *r, bool forget) {
    if (forget && !gameid_wipe()) printf("http: gameID's files not erased\n"); // (no files: nothing kept anyway)
    if (forget && (!creds_forget() || !settings_clear())) {
        respond(r->fd, 500, "text/plain", "Could not erase the settings\n");
        return;
    }
    respond(r->fd, 200, "text/plain", forget ? "Settings erased; restarting into the setup portal\n" : "Restarting\n");
    printf("http: %s requested\n", forget ? "factory reset" : "restart");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response leave
    plat_reboot();
}

#if CRULLER_DEBUG
// POST /debug/baud?rate=N: the RT4K's line speed change, as the PIPe Profiler does it: "baud N" -> "baud
// switching to N", both ends switch, "baud ok" -> "baud confirmed N".
static void handle_debug_baud(int fd, const char *query) {
    char v[12], reply[96], msg[256];
    const uint32_t rate = query && form_field(query, "rate", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    rt4k_status_t st;
    rt4k_get_status(&st);
    const uint32_t old = st.baud;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "baud %lu", (unsigned long)rate);
    rtl1_pause(5000); // no mirror transfer in the middle
    if (!rate) {
        snprintf(msg, sizeof(msg), "need ?rate=N\n");
    } else if (!console_query(cmd, "baud", reply, sizeof(reply), 2500)) {
        snprintf(msg, sizeof(msg), "%s: no reply\n", cmd);
    } else if (strncmp(reply, "baud switching to", 17)) {
        snprintf(msg, sizeof(msg), "%s: refused: %s\n", cmd, reply); // bad baud / busy: nothing changed
    } else if (!rt4k_set_baud(rate)) {
        snprintf(msg, sizeof(msg), "RT4K said \"%s\", but the FT232R didn't switch (the RT4K should fall back)\n", reply);
    } else {
        vTaskDelay(pdMS_TO_TICKS(50));
        char confirm[96];
        if (console_query("baud ok", "baud", confirm, sizeof(confirm), 2500) && strstr(confirm, "confirmed")) {
            snprintf(msg, sizeof(msg), "now at %lu baud: %s / %s\n", (unsigned long)rate, reply, confirm);
        } else {
            rt4k_set_baud(old);
            snprintf(msg, sizeof(msg), "not confirmed at %lu baud; back at %lu\n", (unsigned long)rate, (unsigned long)old);
        }
    }
    rtl1_pause(0);
    respond(fd, 200, "text/plain", msg);
}

// POST /debug/gap?fixed=N or ?reply=N: the wait after a console command (rtl1_set_gap), and zeroed key ->
// screen / poll error counters for the next measurement.
static void handle_debug_gap(int fd, const char *query) {
    char v[12], msg[80];
    const uint32_t fixed = query && form_field(query, "fixed", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    const uint32_t reply = query && form_field(query, "reply", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    rtl1_set_gap(fixed, reply);
    ws_debug_reset();
    snprintf(msg, sizeof(msg), fixed ? "fixed %lu ms after a command\n" : "after the reply + %lu ms\n", (unsigned long)(fixed ? fixed : reply));
    respond(fd, 200, "text/plain", msg);
}

// POST /debug/flow?on=1|0: RTS/CTS flow control on the FT232R (see rt4k_set_flow_control).
static void handle_debug_flow(int fd, const char *query) {
    char v[4], msg[96];
    const bool on = query && form_field(query, "on", v, sizeof(v)) && !strcmp(v, "1");
    rt4k_set_flow_control(on);
    vTaskDelay(pdMS_TO_TICKS(200));
    snprintf(msg, sizeof(msg), "asked %s; flow control now %s, CTS %s\n", on ? "on" : "off", rt4k_flow_control() ? "on" : "off",
        rt4k_modem_status() & 0x10 ? "on" : "off");
    respond(fd, 200, "text/plain", msg);
}

// POST /debug/portal?minutes=N: the setup access point next to the station link (0 closes it).
static void handle_debug_portal(int fd, const char *query) {
    char v[8], msg[96];
    const uint32_t minutes = query && form_field(query, "minutes", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 5;
    net_portal_test(minutes > 60 ? 60 : minutes);
    snprintf(msg, sizeof(msg), minutes ? "opening \"Cruller_Setup\" for %lu min\n" : "closing the setup portal\n", (unsigned long)minutes);
    respond(fd, 200, "text/plain", msg);
}

// POST /debug/fault?kind=task|flash: self-test of the fault reports (health_fault_test). The board resets
// within ~10 s; the log then has the HardFault lines and the freeze record.
static void handle_debug_fault(int fd, const char *query) {
    char kind[8] = "";
    if (query) form_field(query, "kind", kind, sizeof(kind));
    if (health_fault_test(kind)) respond(fd, 200, "text/plain", "Faulting; the board resets within ~10 s\n"); // (the response leaves first)
    else respond(fd, 400, "text/plain", "kind=task, or kind=flash on the Pico 2 W\n");
}
#endif

static void handle(request_t *r) {
    const bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    char *query = strchr(r->path, '?');
    if (query) *query++ = 0;
    if (get && respond_asset(r->fd, r->path)) return;
    if (get && !strcmp(r->path, "/status")) handle_status(r->fd);
    else if (get && !strcmp(r->path, "/log")) handle_stream(r->fd, query, log_read);
    else if (get && !strcmp(r->path, "/rt4k/xfer")) handle_rt4k_xfer(r->fd, query);
    else if (get && !strcmp(r->path, "/ws")) handle_ws(r, false, NULL);
    else if (get && !strcmp(r->path, "/debug/tasks")) handle_debug_tasks(r->fd, query);
#if CRULLER_DEBUG
    else if (get && !strcmp(r->path, "/rt4k/rx")) handle_stream(r->fd, query, rt4k_rx_read); // (with /debug/raw)
    else if (post && !strcmp(r->path, "/debug/raw")) handle_debug_raw(r, query);
    else if (post && !strcmp(r->path, "/debug/baud")) handle_debug_baud(r->fd, query);
    else if (post && !strcmp(r->path, "/debug/gap")) handle_debug_gap(r->fd, query);
    else if (post && !strcmp(r->path, "/debug/flow")) handle_debug_flow(r->fd, query);
#endif
    else if (post && !strcmp(r->path, "/rt4k/put")) handle_rt4k_put(r, query);
    else if (post && !strcmp(r->path, "/rt4k/ask")) handle_rt4k_ask(r, query);
    else if (get && !strcmp(r->path, "/rt4k/ls")) handle_rt4k_ls(r, query);
    else if (get && !strcmp(r->path, "/rt4k/get")) handle_rt4k_get(r, query);
    else if (get && !strcmp(r->path, "/api/v1/info")) handle_api_info(r->fd);
    else if (get && !strcmp(r->path, "/api/v1/state")) handle_api_state(r->fd);
    else if (get && !strcmp(r->path, "/api/v1/events")) handle_ws(r, true, query);
    else if (post && (!strcmp(r->path, "/api/v1/command") || !strcmp(r->path, "/api/command"))) handle_api_command(r);
#if CRULLER_DEBUG
    else if (get && !strcmp(r->path, "/debug/lastfail")) {
        const uint8_t *d;
        const size_t n = rtl1_last_failure(&d);
        respond_bytes(r->fd, "", d, n);
    }
    else if (get && !strcmp(r->path, "/debug/usbtrace")) {
        rt4k_trace_dump(scratch.usbtrace, sizeof(scratch.usbtrace));
        respond(r->fd, 200, "text/plain", scratch.usbtrace);
    }
#endif
    else if (get && !strcmp(r->path, "/debug/memory")) handle_debug_memory(r->fd);
    else if (get && !strcmp(r->path, "/debug/tcp")) handle_debug_tcp(r->fd);
    else if (get && !strcmp(r->path, "/debug/console")) {
        console_debug(scratch.console, sizeof(scratch.console));
        respond(r->fd, 200, "text/plain", scratch.console);
    }
    else if (get && !strcmp(r->path, "/debug/freeze")) {
        freeze_dump(scratch.freeze, sizeof(scratch.freeze));
        respond(r->fd, 200, "text/plain", scratch.freeze);
    }
    else if (post && !strcmp(r->path, "/update")) handle_update(r);
    else if (post && !strcmp(r->path, "/update/fetch")) handle_update_fetch(r, query);
    else if (post && !strcmp(r->path, "/wifi")) handle_wifi(r);
    else if ((get || post) && (!strcmp(r->path, "/api/v1/svs") || !strcmp(r->path, "/api/svs"))) handle_svs(r, post);
    else if (post && (!strcmp(r->path, "/api/v1/svs/unpair") || !strcmp(r->path, "/api/svs/unpair"))) handle_svs_unpair(r);
    else if (post && !strcmp(r->path, "/api/v1/svs/profiles")) handle_svs_profiles(r);
    else if ((get || post) && !strcmp(r->path, "/api/v1/gameid/consoles")) handle_gameid_consoles(r, post);
    else if (get && !strcmp(r->path, "/api/v1/gameid/state")) {
        const size_t n = gameid_state_json(scratch.gameid.buf, sizeof(scratch.gameid.buf));
        respond(r->fd, n ? 200 : 503, "application/json", n ? scratch.gameid.buf : "{\"consoles\":[]}");
    }
    else if (get && !strcmp(r->path, "/api/v1/gameid/games")) handle_gameid_games_get(r);
    else if (post && !strcmp(r->path, "/api/v1/gameid/games")) handle_gameid_games_put(r);
    else if (post && !strcmp(r->path, "/api/v1/gameid/games/delete")) handle_gameid_games_delete(r);
    else if ((get || post) && !strcmp(r->path, "/setup")) handle_setup(r, post);
    else if (post && !strcmp(r->path, "/settings")) handle_settings(r);
    else if (post && !strcmp(r->path, "/restart")) handle_restart(r, false);
    else if (post && !strcmp(r->path, "/factory-reset")) handle_restart(r, true);
    else if (get && !strcmp(r->path, "/wifi/scan")) {
        net_scan_json(scratch.scan, sizeof(scratch.scan));
        respond(r->fd, 200, "application/json", scratch.scan);
    }
#if CRULLER_DEBUG
    else if (post && !strcmp(r->path, "/debug/portal")) handle_debug_portal(r->fd, query);
    else if (post && !strcmp(r->path, "/debug/wedge")) {
        // Self-test of the network watchdog: the board should reset ~18 s after this.
        respond(r->fd, 200, "text/plain", "Freezing the network for 60 s\n");
        vTaskDelay(pdMS_TO_TICKS(300)); // let the response leave
        health_wedge_network(60);
    }
    else if (post && !strcmp(r->path, "/debug/fault")) handle_debug_fault(r->fd, query);
#endif
    else if (net_state() == NET_PORTAL || via_portal(r->fd)) send_head(r->fd, 302, NULL, 0, "Location: http://192.168.4.1/\r\n"); // captive portal probes
    else respond(r->fd, 404, "text/plain", "Not found\n");
}

// --- SD card transfers -------------------------------------------------------------------------------
//
// Downloads and uploads (GET /rt4k/get, POST /rt4k/put) last as long as the RT4K link takes: ~45 s for
// a 4 MB file. They run on their own task so the page, folder listings and the API keep answering
// meanwhile. One at a time (the link carries one transfer anyway); the next ones wait in xfer_queue,
// each with its own copy of the request. Only this task runs their handlers, so their static buffers
// stay theirs.

static QueueHandle_t xfer_queue; // request_t *, from pvPortMalloc

static bool is_transfer(const request_t *r) {
    const size_t n = strcspn(r->path, "?");
    return n == 9 && ((!strcmp(r->method, "GET") && !strncmp(r->path, "/rt4k/get", 9)) ||
        (!strcmp(r->method, "POST") && !strncmp(r->path, "/rt4k/put", 9)));
}

static void hand_over(request_t *r) {
    request_t *copy = pvPortMalloc(sizeof(*copy));
    if (copy) {
        memcpy(copy, r, sizeof(*copy));
        if (xQueueSend(xfer_queue, &copy, 0) == pdTRUE) {
            r->adopted = true; // the transfer task answers and closes it
            return;
        }
        vPortFree(copy);
    }
    respond(r->fd, 503, "text/plain", "Too many SD card transfers waiting: try again once one is done\n");
}

static void xfer_task(void *param) {
    (void)param;
    for (;;) {
        request_t *r;
        xQueueReceive(xfer_queue, &r, portMAX_DELAY);
        handle(r);
        closesocket(r->fd);
        vPortFree(r);
    }
}

// --- server ------------------------------------------------------------------------------------

static void http_task(void *param) {
    (void)param;
    const int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(HTTP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, HTTP_BACKLOG) < 0) {
        printf("http: cannot listen on port %d\n", HTTP_PORT);
        vTaskDelete(NULL);
    }
    listening = true;
    printf("http: listening on port %d\n", HTTP_PORT);
    static request_t req;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int fd = accept(server, (struct sockaddr *)&peer, &plen);
        if (fd < 0) continue;
        const struct timeval tv = {.tv_sec = RECV_TIMEOUT_MS / 1000, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        memset(&req, 0, sizeof(req));
        req.fd = fd;
        if (read_request(&req)) {
            if (is_transfer(&req)) hand_over(&req);
            else handle(&req);
        }
        if (!req.adopted) closesocket(fd);
    }
}

void http_start(void) {
    svs_profiles_start(); // before the tasks that give them out
    xfer_queue = xQueueCreate(XFER_QUEUE, sizeof(request_t *));
    xTaskCreate(xfer_task, "xfer", PLAT_STACK(XFER_TASK_STACK), NULL, HTTP_TASK_PRIORITY, NULL);
    xTaskCreate(http_task, "http", PLAT_STACK(HTTP_TASK_STACK), NULL, HTTP_TASK_PRIORITY, NULL);
}
