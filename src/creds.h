// Wi-Fi credentials store (two alternating flash sectors in the data partition).

#pragma once

#include <stdbool.h>

#define CREDS_SSID_MAX 32
#define CREDS_PASS_MAX 64

typedef struct {
    char ssid[CREDS_SSID_MAX + 1];
    char pass[CREDS_PASS_MAX + 1];
} wifi_creds_t;

bool creds_load(wifi_creds_t *out);
bool creds_save(const wifi_creds_t *creds);

// First boot after migrating from DonutShop: read /wifi.json from its LittleFS, if present.
bool creds_import_donutshop(wifi_creds_t *out);
