// tls.h for the Raspberry Pi Pico 2 W: mbedTLS (mbedtls_config.h) over an lwIP socket, trusting the
// roots in tls_roots.c. Everything it allocates comes from the FreeRTOS heap and goes back at
// tls_close(): ~45 KB while a connection is open.

#include "tls.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h" // its error codes only
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "tls_roots.h"

struct tls {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt roots;
};

static void *heap_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = pvPortMalloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

static void heap_free(void *p) {
    if (p) vPortFree(p);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const int n = send(((tls_t *)ctx)->fd, buf, len, 0);
    return n > 0 ? n : MBEDTLS_ERR_NET_SEND_FAILED;
}

// A timeout (SO_RCVTIMEO) ends the connection like any other failure.
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const int n = recv(((tls_t *)ctx)->fd, buf, len, 0);
    return n >= 0 ? n : MBEDTLS_ERR_NET_RECV_FAILED;
}

// Connects the socket to host:port (IPv4).
static bool open_socket(tls_t *t, const char *host, uint16_t port, uint32_t timeout_ms, char *err, size_t err_size) {
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        snprintf(err, err_size, "could not look up %s", host);
        return false;
    }
    t->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const struct timeval tv = {.tv_sec = (long)(timeout_ms / 1000), .tv_usec = (long)(timeout_ms % 1000) * 1000};
    const bool ok = t->fd >= 0 &&
        setsockopt(t->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
        setsockopt(t->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0 &&
        connect(t->fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) snprintf(err, err_size, "could not connect to %s", host);
    return ok;
}

tls_t *tls_connect(const char *host, uint16_t port, uint32_t timeout_ms, char *err, size_t err_size) {
    mbedtls_platform_set_calloc_free(heap_calloc, heap_free);
    tls_t *t = heap_calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, err_size, "not enough memory for TLS");
        return NULL;
    }
    t->fd = -1;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_x509_crt_init(&t->roots);

    // The roots stay in flash (no copy); one that doesn't parse is left out.
    for (size_t i = 0; i < tls_roots_count; i++) {
        const int r = mbedtls_x509_crt_parse_der_nocopy(&t->roots, tls_roots[i].der, tls_roots[i].len);
        if (r) printf("tls: root \"%s\" left out (-0x%04x)\n", tls_roots[i].name, (unsigned)-r);
    }

    static const unsigned char personal[] = "cruller";
    int r;
    if (!open_socket(t, host, port, timeout_ms, err, err_size)) goto fail;
    r = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, personal, sizeof(personal) - 1);
    if (!r) r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) {
        snprintf(err, err_size, "TLS setup failed (-0x%04x)", (unsigned)-r);
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->roots, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (!r) r = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (r) {
        snprintf(err, err_size, "TLS setup failed (-0x%04x)", (unsigned)-r);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

    while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            snprintf(err, err_size, "%s's certificate isn't trusted (0x%lx)", host,
                (unsigned long)mbedtls_ssl_get_verify_result(&t->ssl));
        } else {
            snprintf(err, err_size, "TLS handshake with %s failed (-0x%04x)", host, (unsigned)-r);
        }
        goto fail;
    }
    printf("tls: %s, %s, heap free %u (min %u)\n", host, mbedtls_ssl_get_ciphersuite(&t->ssl),
        (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());
    return t;

fail:
    tls_close(t);
    return NULL;
}

bool tls_write_all(tls_t *t, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        const int n = mbedtls_ssl_write(&t->ssl, p, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

int tls_read(tls_t *t, void *buf, size_t len) {
    for (;;) {
        const int n = mbedtls_ssl_read(&t->ssl, buf, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        return n;
    }
}

void tls_close(tls_t *t) {
    if (!t) return;
    if (t->fd >= 0) closesocket(t->fd);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->roots);
    heap_free(t);
}
