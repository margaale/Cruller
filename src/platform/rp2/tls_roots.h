// The root certificates tls.c trusts, as DER (tls_roots.c, written by scripts/tls_roots.sh).

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const uint8_t *der;
    size_t len;
} tls_root_t;

extern const tls_root_t tls_roots[];
extern const size_t tls_roots_count;
