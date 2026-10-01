// store.h in NVS (the "nvs" partition): one blob per record. NVS writes are atomic, so a power cut
// keeps the previous copy. nvs_flash_init() runs in main.c.

#include "store.h"

#include <stdlib.h>
#include <string.h>

#include "nvs.h"

static const char *const keys[STORE_KEYS] = {
    [STORE_CREDS] = "creds",
    [STORE_SETTINGS] = "settings",
    [STORE_RT4K] = "rt4k",
    [STORE_SVS_PROFILES] = "svsprof",
    [STORE_RT4K_MAP] = "rt4kmap", // (the 24 KB "nvs" partition hasn't the room for one of 24 KB: it fails)
};

bool store_load(store_key_t key, void *out, size_t size) {
    nvs_handle_t h;
    if (nvs_open("cruller", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = size;
    const bool ok = nvs_get_blob(h, keys[key], out, &len) == ESP_OK && len == size;
    nvs_close(h);
    return ok;
}

bool store_save(store_key_t key, const void *data, size_t size) {
    if (size > STORE_MAX(key)) return false;
    nvs_handle_t h;
    if (nvs_open("cruller", NVS_READWRITE, &h) != ESP_OK) return false;
    // The same as kept: nothing to write.
    void *kept = malloc(size);
    size_t len = size;
    const bool same = kept && nvs_get_blob(h, keys[key], kept, &len) == ESP_OK && len == size && !memcmp(kept, data, size);
    free(kept);
    const bool ok = same || (nvs_set_blob(h, keys[key], data, size) == ESP_OK && nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}
