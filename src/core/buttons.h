// The remote's buttons by name ({"button": ...} in POST /api/v1/command, docs/API.md), as hass-RT4K
// names them, to RT4K console commands. No I/O; tests/test_buttons.c checks it on the host.

#pragma once

#include <stddef.h>

// The console command for a button, case-insensitive: "power_on" -> "pwr on"; "power_off" -> "remote
// pwr"; hass-RT4K's other names -> "remote <the RT4K's key>" ("diagnostics" -> "remote diag"); any
// other name as the RT4K's key itself ("menu" -> "remote menu").
void buttons_command(const char *button, char *out, size_t size);
