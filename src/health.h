// Watchdog and fault reporting. A hang or fault resets the board after a few seconds instead of
// leaving it dead until someone power-cycles it; the log (kept across the reset) says why.
// A hang in an image still on trial (TBYB) also makes the boot ROM fall back to the previous one.

#pragma once

void health_start(void);

#include <stdint.h>

// Confirming a TBYB image (rom_explicit_buy) turns the hardware watchdog off, since the boot ROM
// runs the trial on it: call this right after confirming.
void health_rearm_watchdog(void);

// After cyw43_arch_init(): also reset when the network stops passing traffic (gateway pings).
void health_start_net_probe(void);

// Self-test of that check: freezes the network for `seconds`, then lets go.
void health_wedge_network(uint32_t seconds);
