// The RT4K's firmware version and model as it last said them (rt4k_info_core.h), kept across restarts
// (store.h) so the page and Home Assistant know them while it sleeps; and the profile it has loaded,
// while it's on.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rt4k_info_core.h"

void rt4k_info_start(void);                 // loads what was kept; before the RT4K sends anything
void rt4k_info_line(const char *line);      // every text line from the RT4K (console.c)
void rt4k_info_tick(bool on);               // the power task's tick: asks what's missing, keeps what changed

// What's known ("" where nothing is). True when the RT4K said it since it last came on.
bool rt4k_info_get(rt4k_info_t *out);

// The profile it has loaded, its path under /profile ("": none). False when not known (not on).
bool rt4k_info_profile(char *out, size_t size);
// Something that may have changed it (the SVS switched inputs: Auto Load SVS): asked soon.
void rt4k_info_profile_soon(void);

// Changes whenever rt4k_info_get() would give something else (for pushing status).
uint32_t rt4k_info_seq(void);
