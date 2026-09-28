// The Wi-Fi network to join (kept in store.h).

#pragma once

#include <stdbool.h>

#define CREDS_SSID_MAX 32
#define CREDS_PASS_MAX 64

typedef struct {
    char ssid[CREDS_SSID_MAX + 1];
    char pass[CREDS_PASS_MAX + 1];
} wifi_creds_t;

bool creds_load(wifi_creds_t *out);   // false when there's no network stored
bool creds_save(const wifi_creds_t *creds);

// Factory reset: stores "no network" (a valid record with an empty name), so the next boot opens the
// setup portal instead of importing DonutShop's old Wi-Fi again.
bool creds_forget(void);
// Whether a record was ever written (including a forgotten network).
bool creds_present(void);
