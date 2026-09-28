// Raspberry Pi Pico 2 W (RP2350, Pico SDK): the types behind platform.h.

#pragma once

#include "pico/critical_section.h"
#include "pico/sha256.h"

typedef critical_section_t plat_lock_t;
typedef pico_sha256_state_t plat_sha256_t;

#define PLAT_STACK(words) (words)
