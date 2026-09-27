#include "svs.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/time.h"

static svs_state_t now_state;
static bool known;

bool svs_report(int input, const char *name, const char *id) {
    const uint32_t t = to_ms_since_boot(get_absolute_time());
    taskENTER_CRITICAL();
    const bool changed = !known || now_state.input != input;
    now_state.input = input;
    snprintf(now_state.name, sizeof(now_state.name), "%s", name ? name : "");
    snprintf(now_state.id, sizeof(now_state.id), "%s", id ? id : "");
    now_state.at_ms = t;
    if (changed) now_state.changed_ms = t;
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
