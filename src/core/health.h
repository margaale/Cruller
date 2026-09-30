// Watchdog and fault reporting. A hang or fault resets the board after a few seconds instead of
// leaving it dead until someone power-cycles it; the log (kept across the reset) says why.
// A hang in an image still on trial (TBYB) also makes the boot ROM fall back to the previous one.

#pragma once

#include <stdbool.h>
#include <stddef.h>

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

// The board's own sensors, for Home Assistant and the Debug tab. On the Pico 2 W: its supply (VSYS:
// USB's 5 V less the input diode's ~0.3 V), sampled 10 times a second in bursts of ADC reads, each
// sample with the burst's average and lowest read (a dip within it shows); whether USB brings 5 V;
// the chip's temperature. A supply that sagged under load reset the board (0.4.3, see freeze.c).
typedef struct {
    bool supply;              // the fields below it are known
    uint32_t supply_mv;       // the last sample's average
    uint32_t supply_min_mv;   // the lowest read since boot
    int8_t usb_power;         // 1: 5 V on USB (VBUS), 0: none, -1: unknown
    bool temperature;         // temperature_dc is known
    int32_t temperature_dc;   // tenths of °C
} health_sensors_t;

// One sample (every 100 ms), for the Debug tab's chart.
typedef struct {
    uint16_t supply_mv, supply_min_mv; // the burst's average and lowest read
    int16_t temperature_dc;            // the latest temperature (read once a second)
} health_sample_t;

// After cyw43_arch_init() (VSYS shares a pin with the CYW43): starts sampling.
void health_start_sensors(void);
// False on a board with none.
bool health_sensors(health_sensors_t *out);
// The samples taken after sample number *seq (the newest `max` of them), oldest first; *seq becomes
// the newest's number. 0 on a board with none.
size_t health_sensor_samples(uint32_t *seq, health_sample_t *out, size_t max);

// Self-test of that check: freezes the network for `seconds`, then lets go.
void health_wedge_network(uint32_t seconds);

// Self-test of the fault reports: the fault comes ~300 ms later (time to answer the request), then
// the board resets. kind "task": a fault in a task. "flash": flash stops answering, as when its chip
// browns out (both cores fault on their next read from it). False for a kind this target can't do.
bool health_fault_test(const char *kind);
