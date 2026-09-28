#include "power_core.h"

#include <string.h>

static struct {
    power_state_t state;
    uint32_t since;       // when the state was entered
    uint32_t last_heard;  // last sign of life (a [COM] line, a transfer's ready line)
    uint32_t last_probe;  // when the last probe went out
    bool probed;          // a probe has gone out since init
    bool probe_out;       // it hasn't been answered yet
    int silent;           // unanswered probes / polls in a row
} p;

static void set(power_state_t s, uint32_t now) {
    if (p.state == s) return;
    p.state = s;
    p.since = now;
    p.silent = 0;
}

static void heard(uint32_t now) {
    p.last_heard = now;
    p.silent = 0;
    p.probe_out = false;
}

// Silence counts only when the RT4K ought to answer: on, or not known yet.
static void count_silence(uint32_t now) {
    if (p.state != PWR_ON && p.state != PWR_UNKNOWN) return;
    if (++p.silent >= PWR_SILENT_TO_STANDBY) set(PWR_STANDBY, now);
}

static bool ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

void power_core_init(uint32_t now_ms) {
    memset(&p, 0, sizeof(p));
    p.state = PWR_UNKNOWN;
    p.since = p.last_heard = now_ms;
}

void power_core_line(const char *line, uint32_t now_ms) {
    if (strncmp(line, "[COM] ", 6)) return; // only command replies prove anything
    const char *l = line + 6;
    heard(now_ms);
    if (strstr(l, "Power On Requested")) {
        set(PWR_BOOTING, now_ms); // it was asleep and is starting up
    } else if (ends_with(l, "Serial Remote: pwr")) {
        set(PWR_STANDBY, now_ms); // the power toggle, answered: it was on, now it's going to sleep
    } else {
        set(PWR_ON, now_ms);      // anything else, "Bad Command: pwr on" included, means it's up
    }
}

void power_core_break(uint32_t now_ms) {
    set(PWR_STANDBY, now_ms);
    p.probe_out = false;
    p.last_probe = now_ms; // the next probe after a standby period
    p.probed = true;
}

void power_core_alive(uint32_t now_ms) {
    heard(now_ms);
    set(PWR_ON, now_ms);
}

void power_core_silent(uint32_t now_ms) {
    count_silence(now_ms);
}

void power_core_woken(uint32_t now_ms) {
    // The reply decides: "Power On Requested" (booting) or "Bad Command: pwr on" (it was on).
    if (p.state == PWR_STANDBY) set(PWR_BOOTING, now_ms);
}

void power_core_disconnected(uint32_t now_ms) {
    power_core_init(now_ms);
}

bool power_core_poll(uint32_t now_ms) {
    // Booting: silence means nothing yet, so a probe only waits until the next one is due.
    const uint32_t reply_ms = p.state == PWR_BOOTING ? PWR_PROBE_BOOTING_MS : PWR_REPLY_MS;
    if (p.probe_out && now_ms - p.last_probe >= reply_ms) {
        p.probe_out = false;
        count_silence(now_ms);
    }
    if (p.state == PWR_BOOTING && now_ms - p.since >= PWR_BOOT_GIVE_UP_MS) set(PWR_UNKNOWN, now_ms);
    if (p.probe_out) return false;

    uint32_t period;
    switch (p.state) {
        case PWR_UNKNOWN: period = PWR_PROBE_UNKNOWN_MS; break;
        case PWR_STANDBY: period = PWR_PROBE_STANDBY_MS; break;
        case PWR_BOOTING: period = PWR_PROBE_BOOTING_MS; break;
        default:
            if (now_ms - p.last_heard < PWR_IDLE_MS) return false; // traffic says it's on
            period = p.silent ? PWR_PROBE_UNKNOWN_MS : 0;           // quiet: check, and soon again
            break;
    }
    if (p.probed && now_ms - p.last_probe < period) return false;
    p.last_probe = now_ms;
    p.probed = true;
    p.probe_out = true;
    return true;
}

power_state_t power_core_state(void) {
    return p.state;
}

const char *power_state_name(power_state_t s) {
    switch (s) {
        case PWR_ON: return "on";
        case PWR_STANDBY: return "standby";
        case PWR_BOOTING: return "starting";
        default: return "unknown";
    }
}
