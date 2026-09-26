// RT4K power state, without an RTOS: fed events, says when to probe. power.c drives it; the host
// unit tests (tests/test_power.c) drive it directly.
//
// What the RT4K shows (firmware 1.75+): while on, every console command gets a "[COM] " reply. In
// standby it ignores everything but "pwr on" (reply "Power On Requested", then it boots), and its
// serial output drops low when it powers down, which the FTDI chip reports as a line break.
// Probes use "ver": answered when on, ignored (and harmless) in standby.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PWR_UNKNOWN,  // just connected, or lost track
    PWR_ON,
    PWR_STANDBY,
    PWR_BOOTING,  // woken ("Power On Requested"), not answering yet
} power_state_t;

#define PWR_PROBE_UNKNOWN_MS  2000   // probe period while unknown
#define PWR_PROBE_STANDBY_MS  5000   // while in standby (notices a power-on from the IR remote)
#define PWR_PROBE_BOOTING_MS  1000   // while booting
#define PWR_IDLE_MS           30000  // on, but nothing heard for this long: probe
#define PWR_REPLY_MS          3000   // a probe unanswered for this long counts as silence
#define PWR_BOOT_GIVE_UP_MS   90000  // booting this long without an answer: unknown
#define PWR_SILENT_TO_STANDBY 3      // unanswered probes / polls in a row that mean standby

void power_core_init(uint32_t now_ms);

void power_core_line(const char *line, uint32_t now_ms); // a text line from the RT4K
void power_core_break(uint32_t now_ms);                  // line break: its output went low
void power_core_alive(uint32_t now_ms);                  // a transfer (mirror poll) got its ready line
void power_core_silent(uint32_t now_ms);                 // a transfer got no ready line
void power_core_woken(uint32_t now_ms);                  // "pwr on" was sent
void power_core_disconnected(uint32_t now_ms);           // the USB serial link went away

// Call often (every few hundred ms). True: send a "ver" probe now (then counted as outstanding).
bool power_core_poll(uint32_t now_ms);

power_state_t power_core_state(void);
const char *power_state_name(power_state_t s);
