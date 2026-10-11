// What health.c shares with the rest of the Pico 2 W's files, beyond health.h.

#pragma once

#include <stdint.h>

// Confirming an updated image (rom_explicit_buy) turns the hardware watchdog off, since the boot ROM
// runs the trial on it: ota_confirm_if_trial() calls this right after.
void health_rearm_watchdog(void);

// After cyw43_arch_init() (VSYS shares a pin with the CYW43): starts sampling health.h's sensors, and
// lighting the status LED (status_led.h; it's on the CYW43 too).
void health_start_sensors(void);

// When the feeder task last ran, as the timer's raw microseconds (timer_hw->timerawl), fed or not:
// read by the freeze recorder's ISR, which touches nothing in flash.
extern volatile uint32_t health_feed_us;
// Per core, the exception frame the HardFault handler found (NULL: none yet), for the freeze recorder.
extern const uint32_t *volatile health_fault_frame[2];
