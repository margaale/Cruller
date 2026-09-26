#include "rfc2217.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "pico/time.h"

#include "power.h"
#include "rfc2217_proto.h"
#include "rt4k.h"

#define RFC2217_PORT          2217
#define RFC2217_TASK_STACK    1024
#define RFC2217_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define TICK_MS               20
#define CLIENT_LINE_MAX       256
#define LINE_IDLE_MS          50  // a line without "\n" (just "\r") goes out after this pause

static int client_fd = -1;
static char client_ip[16];
static rfc2217_t proto;
static int modem_sent = -1; // modem state last announced to the client (-1: not yet)

// Client data is sent to the RT4K a whole line at a time: bytes trickling in separately could be
// cut apart by one of Cruller's own transfers (its "\r<cmd>\r\n" would end the client's line).
static uint8_t line[CLIENT_LINE_MAX];
static size_t line_len;
static uint32_t line_last_ms;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static bool send_all(int fd, const uint8_t *p, size_t len) {
    while (len) {
        const int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void flush_line(void) {
    if (!line_len) return;
    // "pwr on" wakes a sleeping RT4K: tell the power tracker, as rt4k_command() does.
    size_t n = line_len;
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
    const size_t start = line[0] == '\r' ? 1 : 0;
    if (n - start == 6 && !memcmp(line + start, "pwr on", 6)) power_woken();
    rt4k_send_raw(line, line_len);
    line_len = 0;
}

static void from_client(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        line[line_len++] = data[i];
        if (data[i] == '\n' || line_len == sizeof(line)) flush_line();
    }
    line_last_ms = now_ms();
}

static void drop_client(const char *why) {
    if (client_fd < 0) return;
    closesocket(client_fd);
    client_fd = -1;
    line_len = 0;
    printf("rfc2217: client %s %s\n", client_ip, why);
    client_ip[0] = 0;
}

static void rfc2217_task(void *param) {
    (void)param;
    const int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(RFC2217_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 1) < 0) {
        printf("rfc2217: cannot listen on port %d\n", RFC2217_PORT);
        vTaskDelete(NULL);
    }
    printf("rfc2217: listening on port %d\n", RFC2217_PORT);
    static uint8_t in[512], data[512], reply[256], text[256], out[520];
    uint32_t rx_pos = 0;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(server, &rd);
        int maxfd = server;
        if (client_fd >= 0) {
            FD_SET(client_fd, &rd);
            if (client_fd > maxfd) maxfd = client_fd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = TICK_MS * 1000};
        const int ready = select(maxfd + 1, &rd, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(server, &rd)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            const int fd = accept(server, (struct sockaddr *)&peer, &plen);
            if (fd >= 0) {
                drop_client("replaced by a new connection");
                client_fd = fd;
                const struct timeval snd = {.tv_sec = 1, .tv_usec = 0};
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                inet_ntoa_r(peer.sin_addr, client_ip, sizeof(client_ip));
                rfc2217_init(&proto);
                const size_t n = rfc2217_greeting(&proto, out, sizeof(out));
                if (!send_all(fd, out, n)) drop_client("failed");
                else printf("rfc2217: client %s connected\n", client_ip);
                rx_pos = rt4k_rx_head(); // from now on, not the terminal's history
                modem_sent = -1;
            }
        }

        if (client_fd >= 0 && ready > 0 && FD_ISSET(client_fd, &rd)) {
            const int n = recv(client_fd, in, sizeof(in), 0);
            if (n <= 0) {
                drop_client("left");
            } else {
                rfc2217_io_t io = {.data = data, .data_max = sizeof(data), .reply = reply, .reply_max = sizeof(reply)};
                rfc2217_input(&proto, in, (size_t)n, &io, (uint8_t)(rt4k_modem_status() & 0xf0));
                if (io.reply_len && !send_all(client_fd, reply, io.reply_len)) drop_client("failed");
                if (io.data_len) from_client(data, io.data_len);
            }
        }
        if (line_len && now_ms() - line_last_ms >= LINE_IDLE_MS) flush_line();

        // The modem state (CTS, DSR...), announced at connect and on every change.
        const int modem = rt4k_modem_status() & 0xf0;
        if (client_fd >= 0 && modem != modem_sent) {
            const size_t n = rfc2217_modemstate((uint8_t)modem, out, sizeof(out));
            if (send_all(client_fd, out, n)) modem_sent = modem;
            else drop_client("failed");
        }

        // What the RT4K says, to the client.
        if (client_fd >= 0) {
            size_t n;
            while (client_fd >= 0 && (n = rt4k_rx_read(&rx_pos, (char *)text, sizeof(text))) > 0) {
                size_t off = 0;
                while (off < n) {
                    size_t used;
                    const size_t m = rfc2217_escape(text + off, n - off, out, sizeof(out), &used);
                    if (!send_all(client_fd, out, m)) {
                        drop_client("failed");
                        break;
                    }
                    off += used;
                }
            }
        }
    }
}

void rfc2217_client(char *out, size_t size) {
    snprintf(out, size, "%s", client_ip);
}

void rfc2217_start(void) {
    xTaskCreate(rfc2217_task, "rfc2217", RFC2217_TASK_STACK, NULL, RFC2217_TASK_PRIORITY, NULL);
}
