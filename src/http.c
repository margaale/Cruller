#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "pico/stdlib.h"

#include "creds.h"
#include "flash_ops.h"
#include "health.h"
#include "log.h"
#include "rt4k.h"
#include "net.h"
#include "ota.h"
#include "platform_reboot.h"

#define HTTP_TASK_STACK     3072
#define HTTP_TASK_PRIORITY  (tskIDLE_PRIORITY + 2)
#define HTTP_PORT           80
#define RECV_TIMEOUT_MS     15000
#define HEADER_MAX          1536
#define BODY_CHUNK          1024

static volatile bool listening = false;
bool http_listening(void) { return listening; }

typedef struct {
    int fd;
    char method[8];
    char path[96];
    long content_length;
    char head[HEADER_MAX];
    size_t head_len;      // bytes in head[] (headers plus any body bytes read along with them)
    size_t body_start;    // offset of the body inside head[]
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
    if (sscanf(r->head, "%7s %95s", r->method, r->path) != 2) return false;
    for (char *line = strstr(r->head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (!strncasecmp(line + 2, "Content-Length:", 15)) r->content_length = strtol(line + 17, NULL, 10);
    }
    return true;
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

static void handle_status(int fd) {
    char ssid[80], body[512];
    json_escape(ssid, sizeof(ssid), net_ssid());
    rt4k_status_t rt;
    rt4k_get_status(&rt);
    snprintf(body, sizeof(body),
        "{\"version\":\"%s\",\"uptime_s\":%lu,\"net\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
        "\"boot_partition\":%d,\"boot_type\":\"%s\",\"heap_free\":%u,"
        "\"rt4k_usb\":\"%s\",\"rt4k_id\":\"%04x:%04x\",\"rt4k_baud\":%lu,"
        "\"rt4k_tx\":%lu,\"rt4k_rx\":%lu,\"rt4k_dropped\":%lu}",
        CRULLER_VERSION, (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000), state_name(net_state()),
        ssid, net_ip(), ota_boot_partition(), ota_last_boot_type(), (unsigned)xPortGetFreeHeapSize(),
        rt.mounted ? "connected" : "not connected", rt.vid, rt.pid, (unsigned long)rt.baud,
        (unsigned long)rt.tx_bytes, (unsigned long)rt.rx_bytes, (unsigned long)rt.tx_dropped);
    respond(fd, 200, "OK", "application/json", body);
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

static const char PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Cruller</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:560px;margin:auto;padding:16px;background:#111;color:#eee}"
    "section{background:#1c1c1c;border-radius:8px;padding:12px 16px;margin:12px 0}"
    "h1{margin:4px 0}h2{font-size:1.05em;margin:4px 0 8px}td{padding:2px 12px 2px 0}"
    "input,button{font-size:1em;margin:4px 0;padding:8px;box-sizing:border-box;width:100%}"
    "button{background:#d4537e;color:#fff;border:0;border-radius:4px}progress{width:100%}"
    "pre{background:#000;padding:8px;height:180px;overflow:auto;white-space:pre-wrap;font-size:.85em;margin:4px 0}"
    "</style></head><body><h1>Cruller</h1>"
    "<section><h2>Status</h2><table id=st></table></section>"
    "<section><h2>RT4K terminal</h2><pre id=rx></pre>"
    "<form onsubmit='return cmd()'><input id=cm placeholder='Command, e.g. remote menu' autocomplete=off autocapitalize=none></form></section>"
    "<section><h2>Firmware update</h2><input type=file id=fw accept=.uf2>"
    "<button onclick=upd()>Upload</button><progress id=pg value=0 max=1 hidden></progress><div id=um></div></section>"
    "<section><h2>Wi-Fi</h2><form method=post action=/wifi>"
    "<input name=ssid placeholder='Network name (SSID)' required maxlength=32 autocomplete=off autocapitalize=none>"
    "<input name=pass type=password placeholder=Password maxlength=64>"
    "<button>Save and reboot</button></form></section>"
    "<section><h2>Log</h2><pre id=lg></pre></section>"
    "<script>"
    "function st(){fetch('/status').then(r=>r.json()).then(s=>{document.getElementById('st').innerHTML="
    "Object.entries(s).map(([k,v])=>'<tr><td>'+k+'</td><td>'+String(v).replace(/</g,'&lt;')+'</td></tr>').join('')}).catch(()=>{})}"
    "st();setInterval(st,5000);"
    "const pos={rx:0,lg:0};"
    "function pull(u,id){fetch(u+'?since='+pos[id]).then(r=>{pos[id]=+r.headers.get('X-Next')||pos[id];return r.text()})"
    ".then(t=>{if(t){const e=document.getElementById(id);e.textContent+=t;e.scrollTop=e.scrollHeight}}).catch(()=>{})}"
    "setInterval(()=>{pull('/rt4k/rx','rx');pull('/log','lg')},700);"
    "function cmd(){const i=document.getElementById('cm');if(!i.value)return false;"
    "fetch('/rt4k/cmd',{method:'POST',body:i.value}).then(r=>r.text()).then(t=>{document.getElementById('rx').textContent+='> '+i.value+'  ['+t.trim()+']\\n';i.value=''});return false}"
    "function upd(){const f=document.getElementById('fw').files[0],m=document.getElementById('um'),p=document.getElementById('pg');"
    "if(!f){m.textContent='Choose a .uf2 file';return}"
    "const x=new XMLHttpRequest();x.open('POST','/update');p.hidden=false;"
    "x.upload.onprogress=e=>{if(e.lengthComputable){p.max=e.total;p.value=e.loaded}};"
    "x.onload=()=>{m.textContent=x.responseText};x.onerror=()=>{m.textContent='Upload failed'};x.send(f)}"
    "</script></body></html>";

static bool ota_sink(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    return ota_feed(data, len);
}

static void handle_update(request_t *r) {
    if (r->content_length <= 0) {
        respond(r->fd, 411, "Length Required", "text/plain", "Send the .uf2 file as the request body\n");
        return;
    }
    printf("http: firmware upload, %ld bytes\n", r->content_length);
    if (!flash_quiet_begin()) {
        respond(r->fd, 503, "Service Unavailable", "text/plain", "Update failed: could not stop the USB host\n");
        return;
    }
    ota_begin();
    const bool received = read_body(r, ota_sink, NULL);
    if (!received || !ota_finish()) {
        flash_quiet_end();
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

static void handle(request_t *r) {
    const bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    char *query = strchr(r->path, '?');
    if (query) *query++ = 0;
    if (get && !strcmp(r->path, "/")) respond(r->fd, 200, "OK", "text/html", PAGE);
    else if (get && !strcmp(r->path, "/status")) handle_status(r->fd);
    else if (get && !strcmp(r->path, "/log")) handle_stream(r->fd, query, log_read);
    else if (get && !strcmp(r->path, "/rt4k/rx")) handle_stream(r->fd, query, rt4k_rx_read);
    else if (post && !strcmp(r->path, "/rt4k/cmd")) handle_rt4k_cmd(r);
    else if (post && !strcmp(r->path, "/update")) handle_update(r);
    else if (post && !strcmp(r->path, "/wifi")) handle_wifi(r);
    else if (post && !strcmp(r->path, "/debug/wedge")) {
        // Self-test of the network watchdog: the board should reset ~18 s after this.
        respond(r->fd, 200, "OK", "text/plain", "Freezing the network for 60 s\n");
        vTaskDelay(pdMS_TO_TICKS(300)); // let the response leave
        health_wedge_network(60);
    }
    else if (net_state() == NET_PORTAL) redirect(r->fd, "http://192.168.4.1/"); // captive portal probes
    else respond(r->fd, 404, "Not Found", "text/plain", "Not found\n");
}

// --- server ------------------------------------------------------------------------------------

static void http_task(void *param) {
    (void)param;
    const int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(HTTP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 4) < 0) {
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
        closesocket(fd);
    }
}

void http_start(void) {
    xTaskCreate(http_task, "http", HTTP_TASK_STACK, NULL, HTTP_TASK_PRIORITY, NULL);
}
