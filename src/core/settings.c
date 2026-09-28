#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "store.h"

static settings_t cached;
static bool loaded;

void settings_get(settings_t *out) {
    if (!loaded) {
        if (!store_load(STORE_SETTINGS, &cached, sizeof(cached)) || !memchr(cached.name, 0, sizeof(cached.name)) ||
            !memchr(cached.svs_bridge, 0, sizeof(cached.svs_bridge))) {
            memset(&cached, 0, sizeof(cached));
        }
        loaded = true;
    }
    *out = cached;
}

bool settings_save(const settings_t *s) {
    settings_t copy = *s;
    copy.name[SETTINGS_NAME_MAX] = 0;
    copy.svs_bridge[SETTINGS_NAME_MAX] = 0;
    if (!store_save(STORE_SETTINGS, &copy, sizeof(copy))) return false;
    cached = copy;
    loaded = true;
    return true;
}

bool settings_clear(void) {
    const settings_t none = {0};
    return settings_save(&none);
}

void settings_hostname(const char *name, char *out, size_t size) {
    size_t n = (size_t)snprintf(out, size, "cruller");
    bool hyphen = true; // one "-" before the name, none doubled, none trailing
    for (const char *p = name; *p && n + 2 < size; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            if (hyphen) out[n++] = '-';
            out[n++] = (char)tolower(c);
            hyphen = false;
        } else {
            hyphen = true;
        }
    }
    out[n < size ? n : size - 1] = 0;
}

bool settings_name_ok(const char *name) {
    const size_t len = strlen(name);
    if (!len || len > SETTINGS_NAME_MAX) return false;
    bool any = false;
    for (const char *p = name; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c)) any = true;
        else if (c != ' ' && c != '-' && c != '_') return false;
    }
    return any;
}
