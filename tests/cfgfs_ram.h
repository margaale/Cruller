// The host tests' flash for cfgfs (tests/cfgfs_ram.c).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cfgfs_port.h"

// Every byte of the flash set to byte (0xff: erased; anything else: whatever was there before).
void cfgfs_ram_fill(uint8_t byte);

// The power goes after this many more progs or erases (-1: never).
void cfgfs_ram_cut_after(int writes);

// Whether the target has flash for it (cfgfs_port_config returns NULL when not).
void cfgfs_ram_present(bool yes);
