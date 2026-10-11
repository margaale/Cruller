// ESP32-S3 (ESP-IDF): the types behind platform.h.

#pragma once

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "psa/crypto.h"

typedef portMUX_TYPE plat_lock_t;
typedef psa_hash_operation_t plat_sha256_t;

// ESP-IDF's xTaskCreate() takes the stack in bytes.
#define PLAT_STACK(words) ((words) * 4)

// RAM that startup doesn't clear (kept across resets), and a function that runs from RAM (none needs to:
// no fault handler logs here).
#define PLAT_NOINIT(name)  __NOINIT_ATTR name
#define PLAT_RAMFUNC(name) name

// The firmware index's name for this platform (which image it takes: an app .bin).
#define PLAT_NAME "esp32"
