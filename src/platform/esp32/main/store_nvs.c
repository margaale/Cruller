// store.h in NVS (the "nvs" partition): one blob per record. NVS writes are atomic, so a power cut
// keeps the previous copy. nvs_flash_init() runs in main.c.

#include "store.h"

#include "nvs.h"

static const char *const keys[STORE_KEYS] = {
    [STORE_CREDS] = "creds",
    [STORE_SETTINGS] = "settings",
    [STORE_RT4K] = "rt4k",
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
    if (size > STORE_RECORD_MAX) return false;
    nvs_handle_t h;
    if (nvs_open("cruller", NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, keys[key], data, size) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
