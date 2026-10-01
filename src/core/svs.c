#include "svs.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "platform.h"
#include "store.h"

static svs_state_t now_state;
static svs_switch_t now_switch;
static bool known;
static volatile uint32_t version;

bool svs_report(const svs_msg_t *m) {
    const int input = m->input;
    const uint32_t t = plat_ms();
    // Compared outside the critical section: only the HTTP task writes now_switch.
    const bool new_switch = m->has_switch && (!now_state.switch_seq || memcmp(&now_switch, &m->sw, sizeof(now_switch)));
    plat_critical_enter();
    const bool changed = !known || now_state.input != input;
    now_state.input = input;
    if (m->total > 0) now_state.total = m->total;
    memcpy(now_state.name, m->name, sizeof(now_state.name));
    memcpy(now_state.id, m->id, sizeof(now_state.id));
    now_state.at_ms = t;
    if (new_switch) {
        memcpy(&now_switch, &m->sw, sizeof(now_switch)); // (padding too: memcmp above compares it)
        now_state.switch_seq++;
        version++;
    }
    if (changed) {
        now_state.changed_ms = t;
        memmove(&now_state.history[1], &now_state.history[0], sizeof(now_state.history[0]) * (SVS_HISTORY - 1));
        now_state.history[0].input = input;
        now_state.history[0].at_ms = t;
        if (now_state.history_n < SVS_HISTORY) now_state.history_n++;
        version++;
    }
    known = true;
    plat_critical_exit();
    if (changed) printf("svs: input %d%s%s\n", input, m->name[0] ? " " : "", m->name);
    if (new_switch) printf("svs: %d inputs described, output to the RetroTINK: %s\n", m->sw.inputs_n,
        m->sw.has_output ? m->sw.output.kind : "not said");
    return changed;
}

bool svs_get(svs_state_t *out) {
    plat_critical_enter();
    const bool k = known;
    if (k) *out = now_state;
    plat_critical_exit();
    return k;
}

uint32_t svs_get_switch(svs_switch_t *out) {
    plat_critical_enter();
    const uint32_t seq = now_state.switch_seq;
    if (seq) memcpy(out, &now_switch, sizeof(*out));
    plat_critical_exit();
    return seq;
}

uint32_t svs_version(void) {
    return version;
}

// --- each input's profile ----------------------------------------------------------------------------
// The HTTP task sets and gets them; the WebSocket's asks for profiles_seq (with the status).

static char profiles[SVS_PROFILES_MAX]; // the record (store.h): the text, zeros after it
static bool unsaved;                    // it didn't reach the flash: written again with the next set
static volatile uint32_t profiles_seq;

void svs_profiles_start(void) {
    if (!store_load(STORE_SVS_PROFILES, profiles, sizeof(profiles)) || !memchr(profiles, 0, sizeof(profiles))) {
        memset(profiles, 0, sizeof(profiles));
    }
    profiles_seq = profiles[0] ? 1 : 0;
}

const char *svs_profiles_text(void) {
    return profiles;
}

uint32_t svs_profiles_seq(void) {
    return profiles_seq;
}

bool svs_profiles_set(const char *text, bool *changed) {
    *changed = strcmp(profiles, text) != 0;
    if (*changed) {
        memset(profiles, 0, sizeof(profiles));
        snprintf(profiles, sizeof(profiles), "%s", text);
        plat_critical_enter();
        profiles_seq++;
        version++;
        plat_critical_exit();
    } else if (!unsaved) {
        return true;
    }
    unsaved = !store_save(STORE_SVS_PROFILES, profiles, sizeof(profiles));
    printf("svs: profiles %s\n", unsaved ? "NOT kept (flash write failed)" : "kept");
    return !unsaved;
}
