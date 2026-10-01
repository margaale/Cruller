#include "rt4k_info.h"

#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "console.h"
#include "store.h"

static plat_lock_t lock; // lines come from the rt4k task, ticks from the power task, reads from anywhere

void rt4k_info_start(void) {
    plat_lock_init(&lock);
    rt4k_info_t saved;
    const bool have = store_load(STORE_RT4K, &saved, sizeof(saved));
    rt4k_info_core_init(have ? &saved : NULL);
}

void rt4k_info_line(const char *line) {
    plat_lock_enter(&lock);
    rt4k_info_core_line(line);
    plat_lock_exit(&lock);
}

void rt4k_info_tick(bool on) {
    rt4k_info_t keep;
    plat_lock_enter(&lock);
    const char *ask = rt4k_info_core_poll(on, plat_ms());
    const bool changed = rt4k_info_core_take_changed(&keep);
    plat_lock_exit(&lock);
    if (ask) console_send(CON_POWER, ask); // nobody sees the reply; rt4k_info_line reads it (and power.c: on)
    // Not from the rt4k task: a flash write stops the USB host (flash_quiet_begin) and waits for it.
    if (changed) {
        printf("rt4k: firmware %s, model %s: %s\n", keep.version, keep.model,
            store_save(STORE_RT4K, &keep, sizeof(keep)) ? "kept" : "NOT kept (flash write failed)");
    }
}

bool rt4k_info_get(rt4k_info_t *out) {
    plat_lock_enter(&lock);
    const bool fresh = rt4k_info_core_get(out);
    plat_lock_exit(&lock);
    return fresh;
}

bool rt4k_info_profile(char *out, size_t size) {
    plat_lock_enter(&lock);
    const bool known = rt4k_info_core_profile(out, size);
    plat_lock_exit(&lock);
    return known;
}

void rt4k_info_profile_soon(void) {
    plat_lock_enter(&lock);
    rt4k_info_core_profile_soon();
    plat_lock_exit(&lock);
}

uint32_t rt4k_info_seq(void) {
    plat_lock_enter(&lock);
    const uint32_t seq = rt4k_info_core_seq();
    plat_lock_exit(&lock);
    return seq;
}
