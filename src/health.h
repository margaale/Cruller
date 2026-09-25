// Watchdog and fault reporting. A hang or fault resets the board after a few seconds instead of
// leaving it dead until someone power-cycles it; the log (kept across the reset) says why.
// A hang in an image still on trial (TBYB) also makes the boot ROM fall back to the previous one.

#pragma once

void health_start(void);

// After cyw43_arch_init(): also reset if the CYW43 stops answering.
void health_start_net_probe(void);
