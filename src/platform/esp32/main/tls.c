// tls.h for the ESP32-S3: ESP-TLS, with ESP-IDF's certificate bundle.

#include "tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_tls.h"

struct tls {
    esp_tls_t *h;
};

tls_t *tls_connect(const char *host, uint16_t port, uint32_t timeout_ms, char *err, size_t err_size) {
    tls_t *t = calloc(1, sizeof(*t));
    if (t) t->h = esp_tls_init();
    if (!t || !t->h) {
        free(t);
        snprintf(err, err_size, "not enough memory for TLS");
        return NULL;
    }
    const esp_tls_cfg_t cfg = {.crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = (int)timeout_ms};
    if (esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, t->h) != 1) {
        snprintf(err, err_size, "TLS connection to %s failed", host);
        tls_close(t);
        return NULL;
    }
    return t;
}

bool tls_write_all(tls_t *t, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        const ssize_t n = esp_tls_conn_write(t->h, p, len);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

int tls_read(tls_t *t, void *buf, size_t len) {
    for (;;) {
        const ssize_t n = esp_tls_conn_read(t->h, buf, len);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        return (int)n;
    }
}

void tls_close(tls_t *t) {
    if (!t) return;
    if (t->h) esp_tls_conn_destroy(t->h);
    free(t);
}
