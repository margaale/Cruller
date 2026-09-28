// ESP32-S3 (ESP-IDF): the types behind platform.h.

#pragma once

#include "freertos/FreeRTOS.h"
#include "psa/crypto.h"

typedef portMUX_TYPE plat_lock_t;
typedef psa_hash_operation_t plat_sha256_t;

// ESP-IDF's xTaskCreate() takes the stack in bytes.
#define PLAT_STACK(words) ((words) * 4)
