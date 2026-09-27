#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "pico/stdlib.h"

#include "creds.h"
#include "flash_ops.h"
#include "health.h"
#include "freeze.h"
#include "log.h"
#include "clients.h"
#include "console.h"
#include "power.h"
#include "rfc2217.h"
#include "rt4k.h"
#include "rtl1.h"
#include "ws.h"
#include "ws_proto.h"
#include "net.h"
#include "ota.h"
#include "platform_reboot.h"

#define HTTP_TASK_STACK     3072
#define HTTP_TASK_PRIORITY  (tskIDLE_PRIORITY + 2)
#define HTTP_PORT           80
#define HTTP_BACKLOG        4    // connections waiting while one request is served
#define RECV_TIMEOUT_MS     15000
#define HEADER_MAX          1536
#define BODY_CHUNK          1024

static volatile bool listening = false;
bool http_listening(void) { return listening; }

typedef struct {
    int fd;
    char method[8];
    char path[256];       // with the query string (/rt4k/put carries a 64-digit SHA-256)
    long content_length;
    char head[HEADER_MAX];
    size_t head_len;      // bytes in head[] (headers plus any body bytes read along with them)
    size_t body_start;    // offset of the body inside head[]
    bool adopted;         // the socket now belongs to someone else (WebSocket): don't close it
} request_t;

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

static void respond(int fd, int code, const char *reason, const char *type, const char *body) {
    char hdr[192];
    const size_t blen = body ? strlen(body) : 0;
    const int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, reason, type, (unsigned)blen);
    send_all(fd, hdr, (size_t)n);
    if (blen) send_all(fd, body, blen);
}

static void respond_bytes(int fd, const char *extra_headers, const uint8_t *body, size_t len) {
    char hdr[400];
    const int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n%s"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)len, extra_headers);
    send_all(fd, hdr, (size_t)n);
    if (len) send_all(fd, body, len);
}

// Web assets embedded from src/web at build time (cmake/embed.cmake).
extern const unsigned char web_fw_js[];
extern const size_t web_fw_js_len;
extern const unsigned char web_app_js[];
extern const size_t web_app_js_len;
extern const unsigned char web_index_html[];
extern const size_t web_index_html_len;

// no-store: with no-cache (and no validators) browsers still ran the previous app.js after an update.
static void respond_asset(int fd, const char *type, const unsigned char *body, size_t len) {
    char hdr[192];
    const int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        type, (unsigned)len);
    send_all(fd, hdr, (size_t)n);
    send_all(fd, body, len);
}

// Whether the request came in through the setup access point (192.168.4.1), real or test portal.
static bool via_portal(int fd) {
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    return getsockname(fd, (struct sockaddr *)&local, &len) == 0 && local.sin_addr.s_addr == PP_HTONL(0xC0A80401u);
}

static void redirect(int fd, const char *location) {
    char hdr[160];
    const int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", location);
    send_all(fd, hdr, (size_t)n);
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
    if (sscanf(r->head, "%7s %255s", r->method, r->path) != 2) return false;
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

// --- pages -------------------------------------------------------------------------------------

static const char *state_name(net_state_t s) {
    switch (s) {
        case NET_JOINING: return "joining";
        case NET_CONNECTED: return "connected";
        case NET_PORTAL: return "setup portal";
        default: return "starting";
    }
}

static void json_escape(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (; *in && n + 7 < size; in++) {
        if (*in == '"' || *in == '\\') { out[n++] = '\\'; out[n++] = *in; }
        else if ((unsigned char)*in < 0x20) n += (size_t)snprintf(out + n, size - n, "\\u%04x", *in);
        else out[n++] = *in;
    }
    out[n] = 0;
}

// Debug: GET /debug/tasks: where the ws/mirror tasks are and the link counters; with ?stacks, every
// task's state too. Only on request: uxTaskGetSystemState() suspends the scheduler on both cores while
// it scans every stack, for milliseconds, and the rt4k task missed USB packets (FT232R overruns).
static void handle_debug_tasks(int fd, const char *query) {
    static char out[2048];
    ws_debug(out, sizeof(out));
    size_t o = strlen(out);
    if (!query || !strstr(query, "stacks")) {
        respond(fd, 200, "OK", "text/plain", out);
        return;
    }
    static TaskStatus_t tasks[24];
    const UBaseType_t n = uxTaskGetSystemState(tasks, 24, NULL);
    static const char *states[] = {"running", "ready", "blocked", "suspended", "deleted", "invalid"};
    for (UBaseType_t i = 0; i < n && o < sizeof(out) - 80; i++) {
        const unsigned st = tasks[i].eCurrentState <= eInvalid ? (unsigned)tasks[i].eCurrentState : 5u;
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%-12s %-9s prio %lu stack free %lu\n", tasks[i].pcTaskName,
            states[st], (unsigned long)tasks[i].uxCurrentPriority, (unsigned long)tasks[i].usStackHighWaterMark);
    }
    respond(fd, 200, "OK", "text/plain", out);
}

// GET /debug/memory: clients against their limits, lwIP's pools and heap, the FreeRTOS heap, RAM.
extern char __data_start__, __data_end__, __bss_start__, __bss_end__, __StackTop;

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

static void handle_debug_memory(int fd) {
    static char out[2048];
    http_debug_memory(out, sizeof(out));
    respond(fd, 200, "OK", "text/plain", out);
}

void http_debug_memory(char *out, size_t size) {
    size_t o = 0;
    out[0] = 0;
#define ADD(...) o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__)
    const int ws_n = ws_clients(NULL), rfc_n = rfc2217_count(NULL);
    ADD("clients                 %d of %d, shared (see clients.h)\n", clients_used(), CLIENTS_MAX);
    ADD("  web pages (WebSocket)   %d (when full, a new one replaces the quietest page)\n", ws_n);
    ADD("  RFC 2217 (port 2217)    %d (when full, a new one replaces the oldest RFC 2217 client)\n", rfc_n);
    ADD("  HTTP                    1 request at a time, %d more waiting\n", HTTP_BACKLOG);

    ADD("\nlwIP pools              used  peak  size  failed\n");
    for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
        const struct stats_mem *m = lwip_stats.memp[pools[i].id];
        ADD("  %-21s %5u %5u %5u %7lu\n", pools[i].name, (unsigned)m->used, (unsigned)m->max, (unsigned)m->avail,
            (unsigned long)m->err);
    }
    ADD("  %-21s %5u %5u %5u %7lu  (bytes)\n", "lwIP heap", (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max,
        (unsigned)lwip_stats.mem.avail, (unsigned long)lwip_stats.mem.err);

    HeapStats_t hs;
    vPortGetHeapStats(&hs);
    ADD("\nFreeRTOS heap           %u KB: free %u, lowest ever %u, largest block %u (bytes)\n",
        (unsigned)(configTOTAL_HEAP_SIZE / 1024), (unsigned)hs.xAvailableHeapSpaceInBytes,
        (unsigned)hs.xMinimumEverFreeBytesRemaining, (unsigned)hs.xSizeOfLargestFreeBlockInBytes);
    const uint32_t data = (uint32_t)(&__data_end__ - &__data_start__), bss = (uint32_t)(&__bss_end__ - &__bss_start__);
    const uint32_t rest = (uint32_t)(&__StackTop - &__bss_end__);
    ADD("RAM                     %u KB: code and data %u KB, bss %u KB (FreeRTOS heap included), rest %u KB\n",
        (unsigned)((data + bss + rest) / 1024), (unsigned)(data / 1024), (unsigned)(bss / 1024), (unsigned)(rest / 1024));
#undef ADD
}

size_t http_debug_memory_json(char *out, size_t size) {
    size_t o = 0;
#define ADD(...) o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__)
    ADD("{\"clients\":{\"used\":%d,\"max\":%d,\"web\":%d,\"rfc2217\":%d,\"http_waiting\":%d},\"pools\":[",
        clients_used(), CLIENTS_MAX, ws_clients(NULL), rfc2217_count(NULL), HTTP_BACKLOG);
    for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
        const struct stats_mem *m = lwip_stats.memp[pools[i].id];
        ADD("%s{\"name\":\"%s\",\"used\":%u,\"peak\":%u,\"size\":%u,\"failed\":%lu}", i ? "," : "", pools[i].name,
            (unsigned)m->used, (unsigned)m->max, (unsigned)m->avail, (unsigned long)m->err);
    }
    HeapStats_t hs;
    vPortGetHeapStats(&hs);
    ADD("],\"lwip_heap\":{\"used\":%u,\"peak\":%u,\"size\":%u,\"failed\":%lu},"
        "\"heap\":{\"size\":%u,\"free\":%u,\"lowest\":%u,\"largest\":%u}}",
        (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max, (unsigned)lwip_stats.mem.avail,
        (unsigned long)lwip_stats.mem.err, (unsigned)configTOTAL_HEAP_SIZE, (unsigned)hs.xAvailableHeapSpaceInBytes,
        (unsigned)hs.xMinimumEverFreeBytesRemaining, (unsigned)hs.xSizeOfLargestFreeBlockInBytes);
#undef ADD
    return o < size ? o : 0;
}

// GET /ws: WebSocket upgrade; the connection then belongs to ws.c.
static void handle_ws(request_t *r) {
    char upgrade[32], key[64], version[8];
    if (!get_header(r, "Upgrade", upgrade, sizeof(upgrade)) || strcasecmp(upgrade, "websocket") ||
        !get_header(r, "Sec-WebSocket-Key", key, sizeof(key)) ||
        !get_header(r, "Sec-WebSocket-Version", version, sizeof(version)) || strcmp(version, "13")) {
        respond(r->fd, 400, "Bad Request", "text/plain", "WebSocket upgrade expected\n");
        return;
    }
    if (!ws_has_room()) {
        respond(r->fd, 503, "Service Unavailable", "text/plain", "Too many WebSocket clients\n");
        return;
    }
    char accept[29], hdr[160];
    ws_accept_key(key, accept);
    const int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    send_all(r->fd, hdr, (size_t)n);
    r->adopted = ws_adopt(r->fd);
}

// GET /rt4k/xfer?cmd=osd|osd2|font: one RTL1 transfer, verified (CRC, sequence, SHA-256), as the
// raw payload; the RT4K's ready line comes back in X-Ready.
static void handle_rt4k_xfer(int fd, const char *query) {
    const char *c = query ? strstr(query, "cmd=") : NULL;
    char cmd[16] = "";
    if (c) {
        size_t n = strcspn(c + 4, "&");
        if (n >= sizeof(cmd)) n = sizeof(cmd) - 1;
        memcpy(cmd, c + 4, n);
        cmd[n] = 0;
    }
    if (strcmp(cmd, "osd") && strcmp(cmd, "osd2") && strcmp(cmd, "font")) {
        respond(fd, 400, "Bad Request", "text/plain", "cmd must be osd, osd2 or font\n");
        return;
    }
    static uint8_t buf[4096];
    static rtl1_info_t info;
    const rtl1_result_t r = rtl1_transfer(cmd, buf, sizeof(buf), &info, false, 0);
    if (r != RTL1_OK) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: %s\n", rtl1_result_name(r), info.detail);
        respond(fd, 502, "Bad Gateway", "text/plain", msg);
        return;
    }
    char headers[200];
    snprintf(headers, sizeof(headers), "X-Ready: %s\r\n", info.ready);
    respond_bytes(fd, headers, buf, info.len);
}

static void handle_status(int fd) {
    char body[1024];
    http_status_json(body, sizeof(body));
    respond(fd, 200, "OK", "application/json", body);
}

void http_status_json(char *body, size_t size) {
    char ssid[80];
    json_escape(ssid, sizeof(ssid), net_ssid());
    rt4k_status_t rt;
    rt4k_get_status(&rt);
    char rfc2217_ips[CLIENTS_MAX * 16 + 8];
    rfc2217_clients(rfc2217_ips, sizeof(rfc2217_ips));
    snprintf(body, size,
        "{\"version\":\"%s\",\"uptime_s\":%lu,\"net\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
        "\"boot_partition\":%d,\"boot_type\":\"%s\",\"heap_free\":%u,"
        "\"rt4k_usb\":\"%s\",\"rt4k_id\":\"%04x:%04x\",\"rt4k_baud\":%lu,\"rt4k_flow\":%s,"
        "\"rt4k_tx\":%lu,\"rt4k_rx\":%lu,\"rt4k_dropped\":%lu,\"rt4k_power\":\"%s\","
        "\"web_clients\":%d,\"rfc2217_count\":%d,\"clients_max\":%d,\"rfc2217_clients\":\"%s\"}",
        CRULLER_VERSION, (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000), state_name(net_state()),
        ssid, net_ip(), net_rssi(), ota_boot_partition(), ota_last_boot_type(), (unsigned)xPortGetFreeHeapSize(),
        rt.mounted ? "connected" : "not connected", rt.vid, rt.pid, (unsigned long)rt.baud,
        rt4k_flow_control() ? "true" : "false",
        (unsigned long)rt.tx_bytes, (unsigned long)rt.rx_bytes, (unsigned long)rt.tx_dropped,
        rt.mounted ? power_state_name(power_state()) : "unknown", ws_clients(NULL), rfc2217_count(NULL), CLIENTS_MAX,
        rfc2217_ips);
    // An upload to the RT4K's SD card: "put":{"path","sent","size"} (the page's progress bar).
    char path[96], path_esc[200];
    uint32_t sent, total;
    const size_t n = strlen(body);
    if (n && n < size && rtl1_put_progress(path, sizeof(path), &sent, &total)) {
        json_escape(path_esc, sizeof(path_esc), path);
        snprintf(body + n - 1, size - (n - 1), ",\"put\":{\"path\":\"%s\",\"sent\":%lu,\"size\":%lu}}", path_esc,
            (unsigned long)sent, (unsigned long)total);
    }
}

// GET <path>?since=N: text written after position N, with the new position in X-Next.
static void handle_stream(int fd, const char *query, size_t (*reader)(uint32_t *, char *, size_t)) {
    uint32_t pos = 0;
    const char *p = query ? strstr(query, "since=") : NULL;
    if (p) pos = (uint32_t)strtoul(p + 6, NULL, 10);
    static char text[2048];
    const size_t n = reader(&pos, text, sizeof(text));
    char hdr[200];
    const int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\n"
        "X-Next: %lu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)n, (unsigned long)pos);
    send_all(fd, hdr, (size_t)h);
    if (n) send_all(fd, text, n);
}

static bool ota_sink(const uint8_t *data, size_t len, void *ctx) {
    bool *quiet = ctx;
    if (!*quiet) {
        if (!flash_quiet_begin()) return false; // "connection lost" is close enough: it can't proceed
        *quiet = true;
    }
    return ota_feed(data, len);
}

static void handle_update(request_t *r) {
    if (r->content_length <= 0) {
        respond(r->fd, 411, "Length Required", "text/plain", "Send the .uf2 file as the request body\n");
        return;
    }
    printf("http: firmware upload, %ld bytes\n", r->content_length);
    // Clients sending "Expect: 100-continue" (curl does above 1 MiB) wait for this before the body.
    // Without it they paused ~1 s, and the board hung during that pause (root cause still unknown).
    char expect[24];
    if (get_header(r, "Expect", expect, sizeof(expect)) && !strcasecmp(expect, "100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(r->fd, cont, sizeof(cont) - 1);
    }
    bool quiet = false; // the USB host is suspended by ota_sink, once the body is actually flowing
    ota_begin();
    const bool received = read_body(r, ota_sink, &quiet);
    if (!received || !ota_finish()) {
        if (quiet) flash_quiet_end();
        char msg[96];
        snprintf(msg, sizeof(msg), "Update failed: %s\n", received ? ota_error() : "connection lost");
        respond(r->fd, 400, "Bad Request", "text/plain", msg);
        return;
    }
    respond(r->fd, 200, "OK", "text/plain", "Update written, rebooting into it\n");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response leave
    ota_reboot_into_update();
}

typedef struct {
    char buf[256];
    size_t len;
} form_t;

static bool form_sink(const uint8_t *data, size_t len, void *ctx);

static void handle_rt4k_cmd(request_t *r) {
    static form_t form;
    form.len = 0;
    form.buf[0] = 0;
    if (r->content_length <= 0 || r->content_length >= (long)sizeof(form.buf) || !read_body(r, form_sink, &form)) {
        respond(r->fd, 400, "Bad Request", "text/plain", "Send the command as the request body\n");
        return;
    }
    // Strip line endings; rt4k_command() adds the framing the RT4K expects.
    for (char *c = form.buf; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    if (!rt4k_command(form.buf)) {
        respond(r->fd, 503, "Service Unavailable", "text/plain", "RT4K queue full\n");
        return;
    }
    rt4k_status_t rt;
    rt4k_get_status(&rt);
    respond(r->fd, 200, "OK", "text/plain", rt.mounted ? "sent\n" : "queued, but no RT4K is connected (dropped)\n");
}

// Debug: POST /debug/raw[?pause=S]: the body goes to the RT4K as is (no framing), and transfers
// (the mirror's polls) are refused for S seconds (default 10) so they don't interleave. For working
// out protocols such as RTL1 put from a PC; replies show up in /rt4k/rx.
typedef struct {
    uint8_t buf[4608];
    size_t len;
} raw_t;

static bool raw_sink(const uint8_t *data, size_t len, void *ctx) {
    raw_t *b = ctx;
    if (b->len + len > sizeof(b->buf)) return false;
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    return true;
}

static void handle_debug_raw(request_t *r, const char *query) {
    static raw_t body;
    body.len = 0;
    const char *p = query ? strstr(query, "pause=") : NULL;
    const uint32_t pause_s = p ? (uint32_t)strtoul(p + 6, NULL, 10) : 10;
    rtl1_pause(pause_s * 1000);
    if (r->content_length <= 0 || r->content_length > (long)sizeof(body.buf) || !read_body(r, raw_sink, &body)) {
        respond(r->fd, 400, "Bad Request", "text/plain", "Send up to 4608 bytes as the body\n");
        return;
    }
    if (!rt4k_link_lock(2000)) {
        respond(r->fd, 503, "Service Unavailable", "text/plain", "link busy\n");
        return;
    }
    // In chunks: the TX queue (2048 bytes) is smaller than a full frame.
    size_t sent = 0;
    for (int tries = 0; sent < body.len && tries < 500; tries++) {
        const size_t n = body.len - sent < 512 ? body.len - sent : 512;
        if (rt4k_write(body.buf + sent, n)) sent += n;
        else vTaskDelay(pdMS_TO_TICKS(2));
    }
    rt4k_link_unlock();
    char msg[48];
    snprintf(msg, sizeof(msg), "sent %u of %u bytes\n", (unsigned)sent, (unsigned)body.len);
    respond(r->fd, sent == body.len ? 200 : 503, sent == body.len ? "OK" : "Service Unavailable", "text/plain", msg);
}

static bool form_sink(const uint8_t *data, size_t len, void *ctx) {
    form_t *f = ctx;
    if (f->len + len >= sizeof(f->buf)) return false;
    memcpy(f->buf + f->len, data, len);
    f->len += len;
    f->buf[f->len] = 0;
    return true;
}

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

// A path on the RT4K's SD card, relative to its root: printable ASCII (spaces and brackets are fine:
// firmware zips have "lumacode/NES/PVM Style D93 (FBX).lmc"), no "..", no backslash, no leading '/'.
static bool sd_path_ok(const char *p) {
    if (!*p || *p == '/' || strstr(p, "..")) return false;
    for (; *p; p++) {
        if (*p < 0x20 || *p > 0x7e || *p == '\\') return false;
    }
    return true;
}

// POST /rt4k/put?path=<sd path>&sha=<sha256 hex>: writes the body to the RT4K's SD card.
static void handle_rt4k_put(request_t *r, const char *query) {
    char path[96], sha[72];
    if (!query || !form_field(query, "path", path, sizeof(path)) || !sd_path_ok(path) ||
        !form_field(query, "sha", sha, sizeof(sha)) || strlen(sha) != 64 || strspn(sha, "0123456789abcdefABCDEF") != 64 ||
        r->content_length <= 0) {
        respond(r->fd, 400, "Bad Request", "text/plain", "Need ?path=<file>&sha=<sha256 hex> and the file as the body\n");
        return;
    }
    char expect[24];
    if (get_header(r, "Expect", expect, sizeof(expect)) && !strcasecmp(expect, "100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(r->fd, cont, sizeof(cont) - 1);
    }
    printf("http: RT4K upload %s, %ld bytes\n", path, r->content_length);
    body_reader_t reader = {r, r->content_length, 0};
    static rtl1_info_t info;
    const uint32_t t0 = to_ms_since_boot(get_absolute_time());
    const rtl1_result_t res = rtl1_put(path, (uint32_t)r->content_length, sha, body_read, &reader, &info);
    const uint32_t ms = to_ms_since_boot(get_absolute_time()) - t0;
    char msg[320];
    if (res == RTL1_OK) {
        snprintf(msg, sizeof(msg), "ok %s %ld bytes in %lu ms (%s)\n", path, r->content_length, (unsigned long)ms,
            info.detail);
        respond(r->fd, 200, "OK", "text/plain", msg);
    } else {
        snprintf(msg, sizeof(msg), "%s: %s\n", rtl1_result_name(res), info.detail);
        respond(r->fd, res == RTL1_ERR_DEVICE ? 409 : 502, res == RTL1_ERR_DEVICE ? "Conflict" : "Bad Gateway",
            "text/plain", msg);
    }
    printf("http: RT4K upload %s: %s", path, msg);
}

// POST /rt4k/ask?expect=<text>[&timeout=<ms>] with a console command as the body: the first reply
// line containing <text> (e.g. "ver" / "FW Version:", "fwup check" / "fwup").
static void handle_rt4k_ask(request_t *r, const char *query) {
    static form_t form;
    form.len = 0;
    form.buf[0] = 0;
    char expect[48], tmo[12];
    if (!query || !form_field(query, "expect", expect, sizeof(expect)) || !expect[0] || r->content_length <= 0 ||
        r->content_length >= (long)sizeof(form.buf) || !read_body(r, form_sink, &form)) {
        respond(r->fd, 400, "Bad Request", "text/plain", "Need ?expect=<text> and a command as the body\n");
        return;
    }
    for (char *c = form.buf; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    uint32_t timeout_ms = form_field(query, "timeout", tmo, sizeof(tmo)) ? (uint32_t)strtoul(tmo, NULL, 10) : 3000;
    if (timeout_ms > 20000) timeout_ms = 20000;
    char line[200];
    if (rt4k_query(form.buf, expect, line, sizeof(line), timeout_ms)) {
        strncat(line, "\n", sizeof(line) - strlen(line) - 1);
        respond(r->fd, 200, "OK", "text/plain", line);
    } else {
        respond(r->fd, 504, "Gateway Timeout", "text/plain", "no reply from the RT4K\n");
    }
}

// --- POST /api/command -------------------------------------------------------------------------------
//
// For automations (Home Assistant...): console commands in, their own replies out, through the same
// queue as everything else (console.c), so they never get another sender's replies. Body:
//   {"command": "remote menu"}   {"commands": ["remote menu", "remote down"]}   {"button": "menu"}
// or plain text, one command per line. "button" takes the remote's names as hass-RT4K sends them:
// "menu" -> "remote menu"; "power_on" -> "pwr on"; "power_off" / "power" -> "remote pwr".
// Answer: {"ok":true,"power":"on","results":[{"command":"ver","sent":true,"reply":["[COM] ..."]}]}

#define API_MAX_COMMANDS 8
#define API_CMD_MAX      200

// The JSON string value after "key": ... (no nesting needed). Returns the position after it, or NULL.
static const char *json_string(const char *p, char *out, size_t size) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',' || *p == '[' || *p == ':') p++;
    if (*p != '"') return NULL;
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            c = *++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
        }
        if (n + 1 < size) out[n++] = c;
    }
    out[n] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static const char *json_key(const char *body, const char *key) {
    char k[24];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(body, k);
    return p ? p + strlen(k) : NULL;
}

static void button_command(const char *button, char *out, size_t size) {
    char b[48];
    size_t n = 0;
    for (; button[n] && n + 1 < sizeof(b); n++) b[n] = (char)tolower((unsigned char)button[n]);
    b[n] = 0;
    if (!strcmp(b, "power_on") || !strcmp(b, "pwr_on")) snprintf(out, size, "pwr on");
    else if (!strcmp(b, "power_off") || !strcmp(b, "power") || !strcmp(b, "pwr")) snprintf(out, size, "remote pwr");
    else snprintf(out, size, "remote %s", b);
}

typedef struct {
    char *out;
    size_t size, len;
    int lines;
} api_reply_t;

static void api_line(const char *line, void *ctx) {
    api_reply_t *a = ctx;
    char esc[400];
    json_escape(esc, sizeof(esc), line);
    a->len += (size_t)snprintf(a->out + a->len, a->len < a->size ? a->size - a->len : 0, "%s\"%s\"",
        a->lines++ ? "," : "", esc);
}

static void handle_api_command(request_t *r) {
    static raw_t body;
    body.len = 0;
    if (r->content_length <= 0 || r->content_length >= (long)sizeof(body.buf) || !read_body(r, raw_sink, &body)) {
        respond(r->fd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"send commands as the body\"}");
        return;
    }
    body.buf[body.len] = 0;
    const char *text = (const char *)body.buf;
    static char cmds[API_MAX_COMMANDS][API_CMD_MAX];
    int count = 0;
    if (*text == '{') {
        const char *p;
        char v[API_CMD_MAX];
        if ((p = json_key(text, "commands"))) {
            while (count < API_MAX_COMMANDS && (p = json_string(p, cmds[count], API_CMD_MAX))) count++;
        } else if ((p = json_key(text, "command")) && json_string(p, cmds[0], API_CMD_MAX)) {
            count = 1;
        } else if ((p = json_key(text, "button")) && json_string(p, v, sizeof(v))) {
            button_command(v, cmds[0], API_CMD_MAX);
            count = 1;
        }
    } else {
        for (const char *p = text; *p && count < API_MAX_COMMANDS;) {
            const size_t n = strcspn(p, "\r\n");
            if (n && n < API_CMD_MAX) {
                memcpy(cmds[count], p, n);
                cmds[count++][n] = 0;
            }
            p += n;
            while (*p == '\r' || *p == '\n') p++;
        }
    }
    for (int i = 0; i < count; i++) {
        for (char *c = cmds[i]; *c; c++) if ((unsigned char)*c < 0x20) *c = ' '; // one line each
    }
    if (!count) {
        respond(r->fd, 400, "Bad Request", "application/json",
            "{\"ok\":false,\"error\":\"no command (\\\"command\\\", \\\"commands\\\" or \\\"button\\\")\"}");
        return;
    }
    static char results[3800], out[4096];
    size_t len = 0;
    bool all_sent = true;
    for (int i = 0; i < count && len < sizeof(results) - 64; i++) {
        char esc[400];
        json_escape(esc, sizeof(esc), cmds[i]);
        len += (size_t)snprintf(results + len, sizeof(results) - len, "%s{\"command\":\"%s\",\"reply\":[",
            i ? "," : "", esc);
        api_reply_t reply = {results, sizeof(results) - 32, len, 0}; // room kept for the closing parts
        const bool sent = console_run(CON_HTTP, cmds[i], api_line, &reply);
        len = reply.len < sizeof(results) - 32 ? reply.len : sizeof(results) - 32;
        len += (size_t)snprintf(results + len, sizeof(results) - len, "],\"sent\":%s}", sent ? "true" : "false");
        all_sent &= sent;
    }
    snprintf(out, sizeof(out), "{\"ok\":%s,\"power\":\"%s\",\"results\":[%s]}", all_sent ? "true" : "false",
        power_state_name(power_state()), results);
    respond(r->fd, all_sent ? 200 : 503, all_sent ? "OK" : "Service Unavailable", "application/json", out);
}

static void handle_wifi(request_t *r) {
    static form_t form;
    form.len = 0;
    form.buf[0] = 0;
    wifi_creds_t creds = {0};
    if (r->content_length <= 0 || r->content_length >= (long)sizeof(form.buf) || !read_body(r, form_sink, &form) ||
        !form_field(form.buf, "ssid", creds.ssid, sizeof(creds.ssid)) || !creds.ssid[0]) {
        respond(r->fd, 400, "Bad Request", "text/plain", "Invalid network name\n");
        return;
    }
    if (!form_field(form.buf, "pass", creds.pass, sizeof(creds.pass))) creds.pass[0] = 0;
    if (!creds_save(&creds)) {
        respond(r->fd, 500, "Internal Server Error", "text/plain", "Could not save the credentials\n");
        return;
    }
    respond(r->fd, 200, "OK", "text/html",
        "<html><body style='font-family:sans-serif;background:#111;color:#eee'>"
        "<h3>Saved. Cruller is rebooting and joining the network.</h3></body></html>");
    vTaskDelay(pdMS_TO_TICKS(500));
    platform_reboot();
}

// POST /restart: reboots into the current image. POST /factory-reset: forgets the Wi-Fi network (the
// only setting Cruller keeps) and reboots into the setup portal.
static void handle_restart(request_t *r, bool forget) {
    if (forget && !creds_forget()) {
        respond(r->fd, 500, "Internal Server Error", "text/plain", "Could not erase the settings\n");
        return;
    }
    respond(r->fd, 200, "OK", "text/plain", forget ? "Settings erased; restarting into the setup portal\n" : "Restarting\n");
    printf("http: %s requested\n", forget ? "factory reset" : "restart");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response leave
    platform_reboot();
}

static void handle(request_t *r) {
    const bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    char *query = strchr(r->path, '?');
    if (query) *query++ = 0;
    if (get && !strcmp(r->path, "/")) respond_asset(r->fd, "text/html; charset=utf-8", web_index_html, web_index_html_len);
    else if (get && !strcmp(r->path, "/fw.js")) respond_asset(r->fd, "application/javascript", web_fw_js, web_fw_js_len);
    else if (get && !strcmp(r->path, "/app.js")) respond_asset(r->fd, "application/javascript", web_app_js, web_app_js_len);
    else if (get && !strcmp(r->path, "/status")) handle_status(r->fd);
    else if (get && !strcmp(r->path, "/log")) handle_stream(r->fd, query, log_read);
    else if (get && !strcmp(r->path, "/rt4k/rx")) handle_stream(r->fd, query, rt4k_rx_read);
    else if (post && !strcmp(r->path, "/rt4k/cmd")) handle_rt4k_cmd(r);
    else if (get && !strcmp(r->path, "/rt4k/xfer")) handle_rt4k_xfer(r->fd, query);
    else if (get && !strcmp(r->path, "/ws")) handle_ws(r);
    else if (get && !strcmp(r->path, "/debug/tasks")) handle_debug_tasks(r->fd, query);
    else if (post && !strcmp(r->path, "/debug/raw")) handle_debug_raw(r, query);
    else if (post && !strcmp(r->path, "/debug/baud")) {
        // POST /debug/baud?rate=N: the RT4K's line speed change, as the PIPe Profiler does it:
        // "baud N" -> "baud switching to N", both ends switch, "baud ok" -> "baud confirmed N".
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
        } else if (!rt4k_query(cmd, "baud", reply, sizeof(reply), 2500)) {
            snprintf(msg, sizeof(msg), "%s: no reply\n", cmd);
        } else if (strncmp(reply, "baud switching to", 17)) {
            snprintf(msg, sizeof(msg), "%s: refused: %s\n", cmd, reply); // bad baud / busy: nothing changed
        } else if (!rt4k_set_baud(rate)) {
            snprintf(msg, sizeof(msg), "RT4K said \"%s\", but the FT232R didn't switch (the RT4K should fall back)\n", reply);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
            char confirm[96];
            if (rt4k_query("baud ok", "baud", confirm, sizeof(confirm), 2500) && strstr(confirm, "confirmed")) {
                snprintf(msg, sizeof(msg), "now at %lu baud: %s / %s\n", (unsigned long)rate, reply, confirm);
            } else {
                rt4k_set_baud(old);
                snprintf(msg, sizeof(msg), "not confirmed at %lu baud; back at %lu\n", (unsigned long)rate, (unsigned long)old);
            }
        }
        rtl1_pause(0);
        respond(r->fd, 200, "OK", "text/plain", msg);
    }
    else if (post && !strcmp(r->path, "/debug/gap")) {
        // POST /debug/gap?fixed=N or ?reply=N: the wait after a console command (rtl1_set_gap), and
        // zeroed key -> screen / poll error counters for the next measurement.
        char v[12];
        const uint32_t fixed = query && form_field(query, "fixed", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
        const uint32_t reply = query && form_field(query, "reply", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
        rtl1_set_gap(fixed, reply);
        ws_debug_reset();
        char msg[80];
        snprintf(msg, sizeof(msg), fixed ? "fixed %lu ms after a command\n" : "after the reply + %lu ms\n",
            (unsigned long)(fixed ? fixed : reply));
        respond(r->fd, 200, "OK", "text/plain", msg);
    }
    else if (post && !strcmp(r->path, "/debug/flow")) {
        // POST /debug/flow?on=1|0: RTS/CTS flow control on the FT232R (see rt4k_set_flow_control).
        const bool on = query && strstr(query, "on=1");
        rt4k_set_flow_control(on);
        vTaskDelay(pdMS_TO_TICKS(200));
        char msg[96];
        snprintf(msg, sizeof(msg), "asked %s; flow control now %s, CTS %s\n", on ? "on" : "off",
            rt4k_flow_control() ? "on" : "off", rt4k_modem_status() & 0x10 ? "on" : "off");
        respond(r->fd, 200, "OK", "text/plain", msg);
    }
    else if (post && !strcmp(r->path, "/rt4k/put")) handle_rt4k_put(r, query);
    else if (post && !strcmp(r->path, "/rt4k/ask")) handle_rt4k_ask(r, query);
    else if (post && !strcmp(r->path, "/api/command")) handle_api_command(r);
    else if (get && !strcmp(r->path, "/debug/lastfail")) {
        const uint8_t *d;
        const size_t n = rtl1_last_failure(&d);
        respond_bytes(r->fd, "", d, n);
    }
    else if (get && !strcmp(r->path, "/debug/usbtrace")) {
        static char trace_text[12288];
        rt4k_trace_dump(trace_text, sizeof(trace_text));
        respond(r->fd, 200, "OK", "text/plain", trace_text);
    }
    else if (get && !strcmp(r->path, "/debug/memory")) handle_debug_memory(r->fd);
    else if (get && !strcmp(r->path, "/debug/console")) {
        static char console_text[1600];
        console_debug(console_text, sizeof(console_text));
        respond(r->fd, 200, "OK", "text/plain", console_text);
    }
    else if (get && !strcmp(r->path, "/debug/freeze")) {
        static char freeze_text[4096];
        freeze_dump(freeze_text, sizeof(freeze_text));
        respond(r->fd, 200, "OK", "text/plain", freeze_text);
    }
    else if (post && !strcmp(r->path, "/update")) handle_update(r);
    else if (post && !strcmp(r->path, "/wifi")) handle_wifi(r);
    else if (post && !strcmp(r->path, "/restart")) handle_restart(r, false);
    else if (post && !strcmp(r->path, "/factory-reset")) handle_restart(r, true);
    else if (get && !strcmp(r->path, "/wifi/scan")) {
        static char scan[1600];
        net_scan_json(scan, sizeof(scan));
        respond(r->fd, 200, "OK", "application/json", scan);
    }
    else if (post && !strcmp(r->path, "/debug/portal")) {
        // POST /debug/portal?minutes=N: the setup access point next to the station link (0 closes it).
        char v[8];
        const uint32_t minutes = query && form_field(query, "minutes", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 5;
        net_portal_test(minutes > 60 ? 60 : minutes);
        char msg[96];
        snprintf(msg, sizeof(msg), minutes ? "opening \"Cruller_Setup\" for %lu min\n" : "closing the setup portal\n",
            (unsigned long)minutes);
        respond(r->fd, 200, "OK", "text/plain", msg);
    }
    else if (post && !strcmp(r->path, "/debug/wedge")) {
        // Self-test of the network watchdog: the board should reset ~18 s after this.
        respond(r->fd, 200, "OK", "text/plain", "Freezing the network for 60 s\n");
        vTaskDelay(pdMS_TO_TICKS(300)); // let the response leave
        health_wedge_network(60);
    }
    else if (net_state() == NET_PORTAL || via_portal(r->fd)) redirect(r->fd, "http://192.168.4.1/"); // captive portal probes
    else respond(r->fd, 404, "Not Found", "text/plain", "Not found\n");
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
        if (read_request(&req)) handle(&req);
        if (!req.adopted) closesocket(fd);
    }
}

void http_start(void) {
    xTaskCreate(http_task, "http", HTTP_TASK_STACK, NULL, HTTP_TASK_PRIORITY, NULL);
}
