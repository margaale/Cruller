#include "ota_fetch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ota.h"
#include "ota_fetch_proto.h"
#include "platform.h"
#include "tls.h"

#define FETCH_TASK_STACK    3072  // words: mbedTLS's handshake (ECDHE, the chain's RSA and ECDSA checks)
#define FETCH_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define FETCH_TIMEOUT_MS    15000 // connecting, and each read
#define FETCH_REDIRECTS     3     // github.com sends it on once, to its asset host
#define FETCH_SIZE_MAX      (8u << 20)
#define FETCH_HEAP_NEEDED   (72 * 1024) // TLS (a 16 KB record buffer, the handshake, the roots), the stack, fetch_t

typedef enum { FETCH_NONE, FETCH_RUNNING, FETCH_DONE, FETCH_FAILED } fetch_state_t;

static volatile fetch_state_t state;
static volatile uint32_t got, want, version;
static char failure[120];
static uint8_t want_sha[32];

// One download's working set, on the FreeRTOS heap while it runs.
typedef struct {
    char url[OTA_FETCH_URL_MAX];   // where it is now (after redirects)
    char host[OTA_FETCH_HOST_MAX];
    ota_fetch_head_t head;
    uint8_t buf[2048];             // the request going out, then what comes back
} fetch_t;

static void set_state(fetch_state_t s) {
    state = s;
    version++;
}

// Records why the download failed, for the page (one line, no quotes or backslashes). Returns false.
static bool fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(failure, sizeof(failure), fmt, ap);
    va_end(ap);
    for (char *c = failure; *c; c++) {
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = '\'';
    }
    printf("fetch: %s\n", failure);
    return false;
}

// Feeds the body into the inactive partition, hashing it on the way: off/avail are its first bytes,
// read along with the head. True once all of it came and matched the SHA-256.
static bool take_body(fetch_t *f, tls_t *t, size_t off, size_t avail) {
    plat_sha256_t sha;
    bool hashing = false, ok = false;
    for (;;) {
        if (avail > want - got) {
            fail("%s sent more than %lu bytes", f->host, (unsigned long)want);
            break;
        }
        if (avail) {
            if (!ota_feed(f->buf + off, avail)) {
                fail("%s", ota_error());
                break;
            }
            // Started once the first bytes went in: they stopped the RT4K's link, the SHA engine's other user.
            if (!hashing && !(hashing = plat_sha256_start(&sha))) {
                fail("the SHA-256 engine is busy");
                break;
            }
            plat_sha256_update(&sha, f->buf + off, avail);
            got += (uint32_t)avail;
        }
        if (got == want) {
            ok = true;
            break;
        }
        const int n = tls_read(t, f->buf, sizeof(f->buf));
        if (n <= 0) {
            fail("the download stopped at %lu of %lu bytes (%s)", (unsigned long)got, (unsigned long)want,
                n ? "connection lost or timed out" : "closed by the server");
            break;
        }
        off = 0;
        avail = (size_t)n;
    }
    uint8_t digest[32] = {0};
    if (hashing) plat_sha256_finish(&sha, digest); // frees the engine, also after a failure
    if (!ok) return false;
    if (memcmp(digest, want_sha, sizeof(digest))) return fail("the download doesn't match its SHA-256");
    return true;
}

// GETs f->url, following GitHub's redirect to the asset's host, and takes the body.
static bool download(fetch_t *f) {
    tls_t *t = NULL;
    size_t off = 0, avail = 0;
    for (int hops = 0;; hops++) {
        uint16_t port;
        const char *path;
        if (!ota_fetch_split_url(f->url, f->host, sizeof(f->host), &port, &path)) return fail("bad address");
        printf("fetch: connecting to %s\n", f->host);
        char err[96];
        if (!(t = tls_connect(f->host, port, FETCH_TIMEOUT_MS, err, sizeof(err)))) return fail("%s", err);
        const int n = snprintf((char *)f->buf, sizeof(f->buf), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Cruller/%s\r\n"
            "Accept: application/octet-stream\r\nConnection: close\r\n\r\n", path, f->host, CRULLER_VERSION);
        if (n <= 0 || (size_t)n >= sizeof(f->buf)) {
            tls_close(t);
            return fail("the address from %s is too long", f->host);
        }
        if (!tls_write_all(t, f->buf, (size_t)n)) {
            tls_close(t);
            return fail("could not send the request to %s", f->host);
        }
        ota_fetch_head_init(&f->head);
        int r = 0;
        while (!f->head.done && !f->head.bad && (r = tls_read(t, f->buf, sizeof(f->buf))) > 0) {
            off = ota_fetch_head_feed(&f->head, f->buf, (size_t)r);
            avail = (size_t)r - off;
        }
        if (!f->head.done) {
            tls_close(t);
            return fail(f->head.bad ? "%s sent a reply Cruller can't read" : "%s didn't answer", f->host);
        }
        if (f->head.status < 300 || f->head.status > 399) break;
        tls_close(t);
        if (hops == FETCH_REDIRECTS) return fail("too many redirects");
        if (!ota_fetch_redirect_allowed(f->head.location)) return fail("%s sent it outside GitHub", f->host);
        strcpy(f->url, f->head.location);
    }
    bool ok = false;
    if (f->head.status != 200) fail("%s answered %d", f->host, f->head.status);
    else if (f->head.chunked) fail("%s sent it in chunks", f->host);
    else if (f->head.length >= 0 && (uint32_t)f->head.length != want) {
        fail("%s has %ld bytes, not %lu", f->host, f->head.length, (unsigned long)want);
    } else ok = take_body(f, t, off, avail);
    tls_close(t);
    return ok;
}

static void fetch_task(void *param) {
    fetch_t *f = param;
    printf("fetch: %lu bytes from %s\n", (unsigned long)want, f->url);
    ota_begin();
    const bool ok = download(f) && (ota_finish() || fail("%s", ota_error()));
    vPortFree(f);
    if (ok) {
        printf("fetch: written, restarting into it\n");
        set_state(FETCH_DONE);
        vTaskDelay(pdMS_TO_TICKS(1500)); // the status saying so goes out first
        ota_reboot_into_update();
    }
    ota_abort();
    set_state(FETCH_FAILED);
    vTaskDelete(NULL);
}

bool ota_fetch_start(const char *url, const char *sha256_hex, uint32_t size, const char **why) {
    if (ota_fetch_running()) {
        *why = "Cruller is downloading an update already";
        return false;
    }
    if (strlen(url) >= OTA_FETCH_URL_MAX || !ota_fetch_start_allowed(url)) {
        *why = "url: one of this project's release assets, " OTA_FETCH_RELEASES "...";
        return false;
    }
    if (!ota_fetch_parse_sha256(sha256_hex, want_sha)) {
        *why = "sha: the image's SHA-256, 64 hex digits";
        return false;
    }
    if (!size || size > FETCH_SIZE_MAX) {
        *why = "size: the image's size in bytes";
        return false;
    }
    fetch_t *f = xPortGetFreeHeapSize() >= FETCH_HEAP_NEEDED ? pvPortMalloc(sizeof(*f)) : NULL;
    if (!f) {
        *why = "not enough free memory for a download";
        return false;
    }
    strcpy(f->url, url);
    got = 0;
    want = size;
    failure[0] = 0;
    set_state(FETCH_RUNNING);
    if (xTaskCreate(fetch_task, "fetch", PLAT_STACK(FETCH_TASK_STACK), f, FETCH_TASK_PRIORITY, NULL) != pdPASS) {
        vPortFree(f);
        fail("could not start the download");
        set_state(FETCH_FAILED);
        *why = "not enough free memory for a download";
        return false;
    }
    return true;
}

bool ota_fetch_running(void) {
    return state == FETCH_RUNNING || state == FETCH_DONE;
}

bool ota_fetch_json(char *out, size_t size) {
    const fetch_state_t s = state;
    if (s == FETCH_RUNNING || s == FETCH_DONE) {
        snprintf(out, size, "{\"from\":\"github\",\"got\":%lu,\"size\":%lu%s}", (unsigned long)got, (unsigned long)want,
            s == FETCH_DONE ? ",\"done\":true" : "");
        return true;
    }
    if (s == FETCH_FAILED) {
        snprintf(out, size, "{\"from\":\"github\",\"failed\":\"%s\"}", failure);
        return true;
    }
    return false;
}

uint32_t ota_fetch_version(void) {
    return version;
}
