// platform.h for the ESP32-S3.

#include "platform.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

uint32_t plat_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
uint32_t plat_us(void) { return (uint32_t)esp_timer_get_time(); }

void plat_lock_init(plat_lock_t *lock) { portMUX_INITIALIZE(lock); }
void plat_lock_enter(plat_lock_t *lock) { portENTER_CRITICAL(lock); }
void plat_lock_exit(plat_lock_t *lock) { portEXIT_CRITICAL(lock); }

// PSA Crypto (mbedTLS), on the SHA accelerator.
bool plat_sha256_start(plat_sha256_t *s) {
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    *s = psa_hash_operation_init();
    return psa_hash_setup(s, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

void plat_sha256_update(plat_sha256_t *s, const void *data, size_t len) { psa_hash_update(s, data, len); }

void plat_sha256_finish(plat_sha256_t *s, uint8_t digest[32]) {
    size_t n = 0;
    if (psa_hash_finish(s, digest, 32, &n) != PSA_SUCCESS) psa_hash_abort(s);
}

// The factory MAC address.
void plat_board_id(char *out, size_t size) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, size, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// esp_restart() stops Wi-Fi first.
void plat_reboot(void) { esp_restart(); }

// ESP-IDF's heap isn't the FreeRTOS one: internal RAM and PSRAM together. RAM split: not known here.
void plat_memory(plat_memory_t *out) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    out->heap_size = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_8BIT);
    out->heap_free = (uint32_t)info.total_free_bytes;
    out->heap_lowest = (uint32_t)info.minimum_free_bytes;
    out->heap_largest = (uint32_t)info.largest_free_block;
    out->ram_total = out->ram_data = out->ram_bss = 0;
}
