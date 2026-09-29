#include "creds.h"

#include <string.h>

#include "store.h"

static bool terminated(const wifi_creds_t *c) {
    return memchr(c->ssid, 0, sizeof(c->ssid)) && memchr(c->pass, 0, sizeof(c->pass));
}

bool creds_load(wifi_creds_t *out) {
    wifi_creds_t c;
    if (!store_load(STORE_CREDS, &c, sizeof(c)) || !terminated(&c) || !c.ssid[0]) return false;
    *out = c;
    return true;
}

bool creds_present(void) {
    wifi_creds_t c;
    return store_load(STORE_CREDS, &c, sizeof(c)) && terminated(&c);
}

bool creds_forget(void) {
    const wifi_creds_t none = {0}; // a valid record with no network: not "never set up"
    return creds_save(&none);
}

bool creds_save(const wifi_creds_t *creds) {
    return store_save(STORE_CREDS, creds, sizeof(*creds));
}
