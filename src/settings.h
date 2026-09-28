// Cruller's own settings (kept in store.h):
// its name, and the SVS Bridge it's paired with (docs/SVS.md). A factory reset clears them.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_NAME_MAX 32

typedef struct {
    char name[SETTINGS_NAME_MAX + 1];       // "Living" ("" : unnamed)
    char svs_bridge[SETTINGS_NAME_MAX + 1]; // the paired SVS Bridge's id ("" : none)
} settings_t;

// The stored settings (all empty if none were saved). Cheap: a copy from RAM after the first call.
void settings_get(settings_t *out);

// Saves them. False if the write failed.
bool settings_save(const settings_t *s);

// Factory reset: back to empty.
bool settings_clear(void);

// The mDNS / DHCP host name for a name: "cruller" when unnamed, else "cruller-" and the name in
// lowercase with anything but letters and digits as single hyphens ("Game room" -> "cruller-game-room").
void settings_hostname(const char *name, char *out, size_t size);

// Whether a name is acceptable: 1-32 characters, letters, digits, spaces, hyphens and underscores.
bool settings_name_ok(const char *name);
