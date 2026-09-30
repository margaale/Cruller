// Watchdog and fault reporting. A hang or fault resets the board after a few seconds instead of
// leaving it dead until someone power-cycles it; the log (kept across the reset) says why.
// A hang in an image still on trial (TBYB) also makes the boot ROM fall back to the previous one.

#pragma once

#include <stdbool.h>

// trial: this is a TBYB image not confirmed yet (see health_start()).
void health_start(bool trial);

#include <stdint.h>

// Confirming a TBYB image (rom_explicit_buy) turns the hardware watchdog off, since the boot ROM
// runs the trial on it: call this right after confirming.
void health_rearm_watchdog(void);

// Before scheduling a reboot on the watchdog: feeding it would keep postponing the reboot.
void health_stop_feeding(void);

// When the feeder task last ran (ms since boot), fed or not; for the freeze recorder.
uint32_t health_last_feed_ms(void);
// The same as the timer's raw microseconds (timer_hw->timerawl), read by the freeze recorder's ISR,
// which touches nothing in flash.
extern volatile uint32_t health_feed_us;
// Per core, the exception frame the HardFault handler found (NULL: none yet), for the freeze recorder.
extern const uint32_t *volatile health_fault_frame[2];

// After cyw43_arch_init(): also reset when the network stops passing traffic (gateway pings).
void health_start_net_probe(void);

// Before cyw43_arch_deinit(): stops the gateway pings for good.
void health_stop_net_probe(void);

// Self-test of that check: freezes the network for `seconds`, then lets go.
void health_wedge_network(uint32_t seconds);

// Self-test of the fault reports: the fault comes ~300 ms later (time to answer the request), then
// the board resets. kind "task": a fault in a task. "flash": flash stops answering, as when its chip
// browns out (both cores fault on their next read from it). False for a kind this target can't do.
bool health_fault_test(const char *kind);
