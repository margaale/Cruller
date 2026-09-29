// A TLS client connection (HTTPS), for Cruller's own firmware downloads (ota_fetch.c). Per target:
// mbedTLS on the Pico 2 W (the roots it trusts are in tls_roots.c), ESP-TLS with ESP-IDF's certificate
// bundle on the ESP32-S3. The server's chain and name are checked; not the certificates' dates (the
// boards have no clock).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tls tls_t;

// Connects to host:port and completes the handshake; reads and writes give up after timeout_ms.
// NULL on failure, with the reason in err.
tls_t *tls_connect(const char *host, uint16_t port, uint32_t timeout_ms, char *err, size_t err_size);
bool tls_write_all(tls_t *t, const void *data, size_t len);
// Bytes read (> 0), 0 once the server closed the connection, < 0 on an error or a timeout.
int tls_read(tls_t *t, void *buf, size_t len);
void tls_close(tls_t *t); // NULL is fine
