// Records kept across restarts: the Wi-Fi credentials (creds.c), the settings (settings.c), the RT4K's
// firmware version and model (rt4k_info.c), each SVS input's profile (svs.c), and the RT4K's settings
// map (the page's, http.c). A save replaces a record whole; a power cut in the middle leaves the
// previous copy. A save that changes nothing writes nothing (each write wears the flash), so callers
// save whenever they like. Each target keeps them its own way (rp2: two alternating copies per record,
// a sector each, the map's 7, in the keys' order: a new record goes at the end).

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    STORE_CREDS,
    STORE_SETTINGS,
    STORE_RT4K,
    STORE_SVS_PROFILES,
    STORE_RT4K_MAP,
    STORE_KEYS,
} store_key_t;

#define STORE_RECORD_MAX 2048  // bytes, every record but the map
#define STORE_MAP_MAX    24576 // bytes, STORE_RT4K_MAP (the ESP32's NVS hasn't the room: it won't save)

// The most a key's record takes.
#define STORE_MAX(key) ((key) == STORE_RT4K_MAP ? STORE_MAP_MAX : STORE_RECORD_MAX)

// Copies the newest saved copy of a record of this size into out. False when there's none.
bool store_load(store_key_t key, void *out, size_t size);

// False if the write failed. True, writing nothing, when the record already holds this.
bool store_save(store_key_t key, const void *data, size_t size);
