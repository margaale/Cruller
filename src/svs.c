#include "svs.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/time.h"

static svs_state_t now_state;
static bool known;
static volatile uint32_t version;

bool svs_report(int input, int total, const char *name, const char *id) {
    const uint32_t t = to_ms_since_boot(get_absolute_time());
    taskENTER_CRITICAL();
    const bool changed = !known || now_state.input != input;
    now_state.input = input;
    if (total > 0) now_state.total = total;
    snprintf(now_state.name, sizeof(now_state.name), "%s", name ? name : "");
    snprintf(now_state.id, sizeof(now_state.id), "%s", id ? id : "");
    now_state.at_ms = t;
    if (changed) {
        now_state.changed_ms = t;
        memmove(&now_state.history[1], &now_state.history[0], sizeof(now_state.history[0]) * (SVS_HISTORY - 1));
        now_state.history[0].input = input;
        now_state.history[0].at_ms = t;
        if (now_state.history_n < SVS_HISTORY) now_state.history_n++;
        version++;
    }
    known = true;
    taskEXIT_CRITICAL();
    if (changed) printf("svs: input %d%s%s\n", input, name && *name ? " " : "", name ? name : "");
    return changed;
}

bool svs_get(svs_state_t *out) {
    taskENTER_CRITICAL();
    const bool k = known;
    if (k) *out = now_state;
    taskEXIT_CRITICAL();
    return k;
}

uint32_t svs_version(void) {
    return version;
}
