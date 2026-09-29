#include "ota_fetch_proto.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool ota_fetch_split_url(const char *url, char *host, size_t host_size, uint16_t *port, const char **path) {
    if (strncmp(url, "https://", 8)) return false;
    const char *h = url + 8;
    const size_t n = strcspn(h, ":/?#@");
    if (!n || n >= host_size || (h[n] != ':' && h[n] != '/')) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)h[i];
        if (!isalnum(c) && c != '.' && c != '-') return false;
    }
    memcpy(host, h, n);
    host[n] = 0;
    const char *p = h + n;
    *port = 443;
    if (*p == ':') {
        char *end;
        const unsigned long v = strtoul(p + 1, &end, 10);
        if (end == p + 1 || !v || v > 65535 || *end != '/') return false;
        *port = (uint16_t)v;
        p = end;
    }
    for (const char *q = p; *q; q++) {
        if ((unsigned char)*q <= 0x20 || *q == 0x7f) return false;
    }
    *path = p;
    return true;
}

bool ota_fetch_start_allowed(const char *url) {
    char host[OTA_FETCH_HOST_MAX];
    uint16_t port;
    const char *path;
    return !strncmp(url, OTA_FETCH_RELEASES, strlen(OTA_FETCH_RELEASES)) && url[strlen(OTA_FETCH_RELEASES)] &&
        ota_fetch_split_url(url, host, sizeof(host), &port, &path) && port == 443 && !strstr(path, "..");
}

bool ota_fetch_redirect_allowed(const char *url) {
    static const char suffix[] = ".githubusercontent.com";
    char host[OTA_FETCH_HOST_MAX];
    uint16_t port;
    const char *path;
    if (!ota_fetch_split_url(url, host, sizeof(host), &port, &path) || port != 443) return false;
    const size_t n = strlen(host), s = sizeof(suffix) - 1;
    return !strcasecmp(host, "github.com") || (n > s && !strcasecmp(host + n - s, suffix));
}

void ota_fetch_head_init(ota_fetch_head_t *h) {
    memset(h, 0, sizeof(*h));
    h->length = -1;
}

// One line of the head, without its CR LF.
static void head_line(ota_fetch_head_t *h) {
    char *l = h->line;
    if (!h->status) {
        // "HTTP/1.1 302 Found"
        if (strncmp(l, "HTTP/1.", 7) || !isdigit((unsigned char)l[7]) || l[8] != ' ' ||
            !isdigit((unsigned char)l[9]) || !isdigit((unsigned char)l[10]) || !isdigit((unsigned char)l[11]) ||
            (l[12] && l[12] != ' ') || l[9] == '0') {
            h->bad = true;
            return;
        }
        h->status = atoi(l + 9);
        return;
    }
    if (!*l) {
        h->done = true;
        return;
    }
    char *colon = strchr(l, ':');
    if (!colon) return;
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
    if (!strcasecmp(l, "Location")) {
        if (h->line_cut || strlen(v) >= sizeof(h->location)) h->bad = true;
        else strcpy(h->location, v);
    } else if (!strcasecmp(l, "Content-Length") && !h->line_cut) {
        char *end;
        const long n = strtol(v, &end, 10);
        h->length = end != v && !*end && n >= 0 ? n : -1;
    } else if (!strcasecmp(l, "Transfer-Encoding")) {
        for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
        h->chunked = strstr(v, "chunked") != NULL;
    }
}

size_t ota_fetch_head_feed(ota_fetch_head_t *h, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len && !h->done && !h->bad) {
        const char c = (char)data[i++];
        if (++h->total > OTA_FETCH_HEAD_MAX) {
            h->bad = true;
        } else if (c == '\n') {
            if (h->line_len && h->line[h->line_len - 1] == '\r') h->line_len--;
            h->line[h->line_len] = 0;
            head_line(h);
            h->line_len = 0;
            h->line_cut = false;
        } else if (h->line_len < sizeof(h->line) - 1) {
            h->line[h->line_len++] = c;
        } else {
            h->line_cut = true;
        }
    }
    return i;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ota_fetch_parse_sha256(const char *hex, uint8_t out[32]) {
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        const int hi = hex_value(hex[2 * i]), lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}
