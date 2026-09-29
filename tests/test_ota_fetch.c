// Host unit tests for the firmware download's parts with no I/O (src/core/ota_fetch_proto.c). Run
// with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "ota_fetch_proto.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)

#define ASSET "https://github.com/margaale/Cruller/releases/download/v0.3.3-alpha.61/cruller-0.3.3-alpha.61-pico2_w-cruller.uf2"

static ota_fetch_head_t h;

// Feeds text in pieces of `step` bytes (0: all at once); returns the head's length.
static size_t feed(const char *text, size_t step) {
    ota_fetch_head_init(&h);
    const size_t len = strlen(text);
    size_t used = 0;
    while (used < len && !h.done && !h.bad) {
        size_t n = step && step < len - used ? step : len - used;
        used += ota_fetch_head_feed(&h, (const uint8_t *)text + used, n);
    }
    return used;
}

// --- URLs -------------------------------------------------------------------------------------------

static void test_split_url(void) {
    char host[OTA_FETCH_HOST_MAX];
    uint16_t port = 0;
    const char *path = NULL;
    CHECK(ota_fetch_split_url(ASSET, host, sizeof(host), &port, &path));
    CHECK(!strcmp(host, "github.com") && port == 443 && !strncmp(path, "/margaale/Cruller/releases/download/", 36));
    CHECK(ota_fetch_split_url("https://example.com:8443/a?b=c", host, sizeof(host), &port, &path));
    CHECK(!strcmp(host, "example.com") && port == 8443 && !strcmp(path, "/a?b=c"));
    CHECK(!ota_fetch_split_url("http://github.com/x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://github.com", host, sizeof(host), &port, &path)); // no path
    CHECK(!ota_fetch_split_url("https://github.com?x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://user@github.com/x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://github.com:0/x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://github.com:99999/x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://github.com:/x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://git_hub.com/x", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://github.com/a b", host, sizeof(host), &port, &path));
    CHECK(!ota_fetch_split_url("https://github.com/a\r\nX: y", host, sizeof(host), &port, &path));
    char tiny[8];
    CHECK(!ota_fetch_split_url("https://github.com/x", tiny, sizeof(tiny), &port, &path));
}

static void test_allowed(void) {
    CHECK(ota_fetch_start_allowed(ASSET));
    CHECK(!ota_fetch_start_allowed(OTA_FETCH_RELEASES));
    CHECK(!ota_fetch_start_allowed("https://github.com/someone/Cruller/releases/download/v1/x.uf2"));
    CHECK(!ota_fetch_start_allowed("https://github.com:444/margaale/Cruller/releases/download/v1/x.uf2"));
    CHECK(!ota_fetch_start_allowed("https://github.com/margaale/Cruller/releases/download/../../x"));
    CHECK(!ota_fetch_start_allowed("http://github.com/margaale/Cruller/releases/download/v1/x.uf2"));

    CHECK(ota_fetch_redirect_allowed("https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sig=x"));
    CHECK(ota_fetch_redirect_allowed("https://objects.githubusercontent.com/x"));
    CHECK(ota_fetch_redirect_allowed("https://GitHub.com/margaale/Cruller/releases/download/v1/x.uf2"));
    CHECK(!ota_fetch_redirect_allowed("https://githubusercontent.com/x"));
    CHECK(!ota_fetch_redirect_allowed("https://evilgithubusercontent.com/x"));
    CHECK(!ota_fetch_redirect_allowed("https://githubusercontent.com.evil.net/x"));
    CHECK(!ota_fetch_redirect_allowed("http://objects.githubusercontent.com/x"));
    CHECK(!ota_fetch_redirect_allowed("https://objects.githubusercontent.com:8443/x"));
    CHECK(!ota_fetch_redirect_allowed("/relative/path"));
}

// --- replies ----------------------------------------------------------------------------------------

static void test_redirect(void) {
    // github.com's 302: the Location, and kilobytes of headers that don't matter.
    static char reply[8192];
    char csp[3000];
    memset(csp, 'x', sizeof(csp) - 1);
    csp[sizeof(csp) - 1] = 0;
    char loc[1100];
    snprintf(loc, sizeof(loc), "https://release-assets.githubusercontent.com/github-production-release-asset/1392596635/ccf0f5f4?sp=r&sv=2018-11-09&jwt=%0*d", 900, 7);
    snprintf(reply, sizeof(reply), "HTTP/1.1 302 Found\r\nServer: github.com\r\nContent-Security-Policy: %s\r\n"
        "location: %s\r\ncontent-length: 0\r\n\r\n", csp, loc);
    for (size_t step = 0; step <= 7; step += 7) {
        const size_t used = feed(reply, step);
        CHECK(h.done && !h.bad && used == strlen(reply));
        CHECK(h.status == 302 && h.length == 0 && !h.chunked);
        CHECK(!strcmp(h.location, loc));
    }
}

static void test_ok_with_body(void) {
    const char *reply = "HTTP/1.1 200 OK\r\nContent-Length: 1421312\r\nContent-Type: application/octet-stream\r\n\r\nUF2\nbody";
    const size_t used = feed(reply, 0);
    CHECK(h.done && h.status == 200 && h.length == 1421312 && !h.location[0]);
    CHECK(!strcmp(reply + used, "UF2\nbody")); // the body starts right after the blank line
    // One byte at a time, bare LFs.
    const char *lf = "HTTP/1.0 200 OK\nContent-Length:  12 \n\nrest";
    CHECK(feed(lf, 1) == strlen(lf) - 4 && h.done && h.status == 200 && h.length == 12);
}

static void test_head_fields(void) {
    feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n", 0);
    CHECK(h.done && h.chunked && h.length == -1);
    feed("HTTP/1.1 404 Not Found\r\n\r\n", 0);
    CHECK(h.done && h.status == 404);
    feed("HTTP/1.1 200\r\nContent-Length: 12x\r\n\r\n", 0);
    CHECK(h.done && h.length == -1);
    feed("HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n", 0);
    CHECK(h.done && h.length == -1);
    feed("HTTP/1.1 200 OK\r\nno colon here\r\n\r\n", 0); // skipped
    CHECK(h.done && !h.bad);
}

static void test_bad(void) {
    feed("SSH-2.0-OpenSSH\r\n\r\n", 0);
    CHECK(h.bad && !h.done);
    feed("HTTP/2 200\r\n\r\n", 0);
    CHECK(h.bad);
    feed("HTTP/1.1 20 OK\r\n\r\n", 0);
    CHECK(h.bad);
    // A Location too long to keep.
    static char reply[4096];
    char loc[2000];
    memset(loc, 'a', sizeof(loc) - 1);
    loc[sizeof(loc) - 1] = 0;
    snprintf(reply, sizeof(reply), "HTTP/1.1 302 Found\r\nLocation: https://x.githubusercontent.com/%s\r\n\r\n", loc);
    feed(reply, 0);
    CHECK(h.bad);
    // A head that never ends.
    static char endless[OTA_FETCH_HEAD_MAX + 100];
    strcpy(endless, "HTTP/1.1 200 OK\r\n");
    size_t n = strlen(endless);
    for (; n + 6 < sizeof(endless); n += 6) memcpy(endless + n, "A: b\r\n", 6);
    endless[n] = 0;
    feed(endless, 0);
    CHECK(h.bad && !h.done);
}

static void test_sha256(void) {
    uint8_t d[32];
    CHECK(ota_fetch_parse_sha256("f22a60f46ae8888781ded912fbad030f35bc5bccbfddc93099103180ed6fee67", d));
    CHECK(d[0] == 0xf2 && d[1] == 0x2a && d[31] == 0x67);
    CHECK(ota_fetch_parse_sha256("F22A60F46AE8888781DED912FBAD030F35BC5BCCBFDDC93099103180ED6FEE67", d) && d[0] == 0xf2);
    CHECK(!ota_fetch_parse_sha256("f22a60f46ae8888781ded912fbad030f35bc5bccbfddc93099103180ed6fee6", d));
    CHECK(!ota_fetch_parse_sha256("f22a60f46ae8888781ded912fbad030f35bc5bccbfddc93099103180ed6fee677", d));
    CHECK(!ota_fetch_parse_sha256("g22a60f46ae8888781ded912fbad030f35bc5bccbfddc93099103180ed6fee67", d));
    CHECK(!ota_fetch_parse_sha256("", d));
}

#define RUN(t) do { current = #t; t(); } while (0)

int main(void) {
    RUN(test_split_url);
    RUN(test_allowed);
    RUN(test_redirect);
    RUN(test_ok_with_body);
    RUN(test_head_fields);
    RUN(test_bad);
    RUN(test_sha256);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
