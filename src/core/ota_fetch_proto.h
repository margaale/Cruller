// The firmware download's parts with no I/O (ota_fetch.c): which URLs it takes, and the replies'
// heads, read as they come. tests/test_ota_fetch.c checks them on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Where a download may start: this project's release assets.
#define OTA_FETCH_RELEASES "https://github.com/margaale/Cruller/releases/download/"

#define OTA_FETCH_HOST_MAX 64
#define OTA_FETCH_URL_MAX  1536 // GitHub's redirect to an asset carries a signed query (~970 bytes)
#define OTA_FETCH_HEAD_MAX 16384 // github.com's 302 has ~5 KB of headers

// "https://host[:port]/path": the host, the port (443 when none) and the path (with the query). False
// for anything else: not https, user info, no path, a host too long or with odd characters, spaces
// or control characters in the path (it goes into the request line as it is).
bool ota_fetch_split_url(const char *url, char *host, size_t host_size, uint16_t *port, const char **path);

// A URL a download may start from (OTA_FETCH_RELEASES...), and one GitHub may send it on to: https on
// port 443, to github.com or a *.githubusercontent.com host.
bool ota_fetch_start_allowed(const char *url);
bool ota_fetch_redirect_allowed(const char *url);

// A reply's head, read as it comes: the status, Content-Length, chunked or not, and Location. Other
// headers are skipped whatever their length.
typedef struct {
    int status;            // 0 until the status line is in
    long length;           // Content-Length, -1 when none
    bool chunked;          // Transfer-Encoding: chunked
    bool done;             // the blank line after the headers came
    bool bad;              // not an HTTP/1.x reply, a head over OTA_FETCH_HEAD_MAX, or a Location too long to keep
    char location[OTA_FETCH_URL_MAX];
    char line[OTA_FETCH_URL_MAX + 16]; // the line coming in: a longer one keeps only its start
    size_t line_len;
    bool line_cut;
    size_t total;          // bytes of the head so far
} ota_fetch_head_t;

void ota_fetch_head_init(ota_fetch_head_t *h);
// Feeds reply bytes; returns how many belong to the head (the rest is the body's start). Stops at
// the end of the head or at the first problem: check done and bad after each call.
size_t ota_fetch_head_feed(ota_fetch_head_t *h, const uint8_t *data, size_t len);

// 64 hex digits (either case) -> 32 bytes.
bool ota_fetch_parse_sha256(const char *hex, uint8_t out[32]);
