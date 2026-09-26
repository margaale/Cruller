#pragma once

#include <stddef.h>
#include <stdint.h>

void sha256_ref(const uint8_t *data, size_t len, uint8_t out[32]);
