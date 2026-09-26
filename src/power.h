// RT4K power state (on / standby / starting), tracked from what the RT4K says; see power_core.h.
// A small task sends the "ver" probes the core asks for.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "power_core.h"

void power_start(void);

power_state_t power_state(void);

// Events, from wherever they're seen.
void power_feed_text(const uint8_t *data, size_t len); // terminal text from the RT4K (rt4k task)
void power_break(void);                                // FTDI line break (rt4k task)
void power_alive(void);                                // a transfer got its ready line or a refusal
void power_silent(void);                               // a transfer got no ready line
void power_woken(void);                                // "pwr on" was sent
