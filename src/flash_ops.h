// Flash erase/program that is safe with FreeRTOS SMP (the other core is parked while flash is busy).

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Offsets are physical flash offsets; data must be in RAM.
bool flash_erase_safe(uint32_t offset, size_t count);
bool flash_program_safe(uint32_t offset, const void *data, size_t count);
