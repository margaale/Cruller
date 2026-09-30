#include "buttons.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// hass-RT4K's names that aren't the RT4K's own key (its COMMAND_MAP, custom_components/retrotink/
// const.py). The keys themselves ("menu", "diag", "prof1"...) go through as they are.
static const struct {
    const char *name, *key;
} aliases[] = {
    {"enter", "ok"},
    {"diagnostics", "diag"},
    {"statistics", "stat"},
    {"cropping", "scaler"},
    {"processing", "sfx"},
    {"effects", "sfx"},
    {"color", "col"},
    {"audio", "aud"},
    {"profiles", "prof"},
    {"profile1", "prof1"},
    {"profile2", "prof2"},
    {"profile3", "prof3"},
    {"profile4", "prof4"},
    {"profile5", "prof5"},
    {"profile6", "prof6"},
    {"profile7", "prof7"},
    {"profile8", "prof8"},
    {"profile9", "prof9"},
    {"profile10", "prof10"},
    {"profile11", "prof11"},
    {"profile12", "prof12"},
    {"auto_gain", "gain"},
    {"auto_phase", "phase"},
    {"safe_mode", "safe"},
    {"gen_lock", "genlock"},
    {"triple_buffer", "buffer"},
    {"4k", "res4k"},
    {"1080p", "res1080p"},
    {"1440p", "res1440p"},
    {"480p", "res480p"},
    {"custom1", "res1"},
    {"custom2", "res2"},
    {"custom3", "res3"},
    {"custom4", "res4"},
    {"auto_crop_vertical", "aux1"},
    {"auto_crop_4_3", "aux2"},
    {"auto_crop_16_9", "aux3"},
};

void buttons_command(const char *button, char *out, size_t size) {
    char b[48];
    size_t n = 0;
    for (; button[n] && n + 1 < sizeof(b); n++) b[n] = (char)tolower((unsigned char)button[n]);
    b[n] = 0;
    if (!strcmp(b, "power_on") || !strcmp(b, "pwr_on")) {
        snprintf(out, size, "pwr on");
        return;
    }
    if (!strcmp(b, "power_off") || !strcmp(b, "power") || !strcmp(b, "pwr")) {
        snprintf(out, size, "remote pwr");
        return;
    }
    const char *key = b;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (!strcmp(b, aliases[i].name)) {
            key = aliases[i].key;
            break;
        }
    }
    snprintf(out, size, "remote %s", key);
}
