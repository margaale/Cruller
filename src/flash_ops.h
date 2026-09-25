// Flash erase/program that is safe with FreeRTOS SMP (the other core is parked while flash is busy).
//
// Writing flash while TinyUSB's host (the RT4K link) is running breaks the link to the CYW43: the
// board keeps running but drops off the network, mid-OTA. Bench builds without the USB host update
// fine, so every write happens with the host suspended. A series of writes (an OTA image) holds
// flash_quiet_begin()/flash_quiet_end() around all of them; single writes do it themselves.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Before the scheduler starts.
void flash_ops_init(void);

// Nestable. False (and nothing held) if the USB host could not be stopped.
bool flash_quiet_begin(void);
void flash_quiet_end(void);

// Offsets are physical flash offsets; data must be in RAM.
bool flash_erase_safe(uint32_t offset, size_t count);
bool flash_program_safe(uint32_t offset, const void *data, size_t count);
