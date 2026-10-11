// Host unit tests for the RT4K power state (src/core/power_core.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "power_core.h"

static uint32_t now;

// Runs the poll loop every 100 ms for `ms`; returns how many probes it asked for.
static int run(uint32_t ms) {
    int probes = 0;
    for (uint32_t t = 0; t < ms; t += 100) {
        now += 100;
        if (power_core_poll(now)) probes++;
    }
    return probes;
}

static void reset(void) {
    now = 1000;
    power_core_init(now);
}

static void test_starts_unknown_and_probes(void) {
    reset();
    CHECK(power_core_state() == PWR_UNKNOWN);
    CHECK(power_core_poll(now)); // right away
    CHECK(!power_core_poll(now)); // one at a time
}

static void test_answer_means_on(void) {
    reset();
    power_core_poll(now);
    power_core_line("[COM] RT4KPRO, FW Version: 1.87.3", now + 50);
    CHECK(power_core_state() == PWR_ON);
}

static void test_other_lines_prove_nothing(void) {
    reset();
    power_core_line("[MCU] Powering Up", now);
    power_core_line("some noise", now);
    CHECK(power_core_state() == PWR_UNKNOWN);
}

static void test_unknown_silence_means_standby(void) {
    reset();
    run(15000); // probes every 2 s, none answered
    CHECK(power_core_state() == PWR_STANDBY);
}

static void test_break_means_standby(void) {
    reset();
    power_core_alive(now);
    CHECK(power_core_state() == PWR_ON);
    power_core_break(now);
    CHECK(power_core_state() == PWR_STANDBY);
}

static void test_standby_probes_slowly(void) {
    reset();
    power_core_break(now);
    const int probes = run(20000);
    CHECK(probes >= 3 && probes <= 4); // every 5 s
    CHECK(power_core_state() == PWR_STANDBY); // unanswered probes in standby change nothing
}

static void test_ir_power_on_noticed(void) {
    reset();
    power_core_break(now);
    run(6000); // a probe went out
    power_core_line("[COM] RT4KPRO, FW Version: 1.87.3", now);
    CHECK(power_core_state() == PWR_ON);
}

static void test_wake_then_boot_then_on(void) {
    reset();
    power_core_break(now);
    power_core_woken(now);
    CHECK(power_core_state() == PWR_BOOTING);
    power_core_line("[COM] Power On Requested", now + 100);
    CHECK(power_core_state() == PWR_BOOTING);
    const int probes = run(5000); // probes every second while booting
    CHECK(probes >= 4);
    power_core_line("[COM] RT4KPRO, FW Version: 1.87.3", now);
    CHECK(power_core_state() == PWR_ON);
}

static void test_pwr_on_while_on(void) {
    reset();
    power_core_alive(now);
    power_core_woken(now);
    CHECK(power_core_state() == PWR_ON); // woken only matters from standby
    power_core_line("[COM] Bad Command: pwr on", now);
    CHECK(power_core_state() == PWR_ON);
}

static void test_power_toggle_means_standby(void) {
    reset();
    power_core_alive(now);
    power_core_line("[COM] Serial Remote: pwr", now);
    CHECK(power_core_state() == PWR_STANDBY);
}

static void test_boot_gives_up(void) {
    reset();
    power_core_break(now);
    power_core_woken(now);
    run(PWR_BOOT_GIVE_UP_MS + 1000);
    CHECK(power_core_state() != PWR_BOOTING);
}

static void test_on_with_traffic_does_not_probe(void) {
    reset();
    power_core_alive(now);
    int probes = 0;
    for (int i = 0; i < 600; i++) { // 60 s of mirror polls every 100 ms
        now += 100;
        power_core_alive(now);
        if (power_core_poll(now)) probes++;
    }
    CHECK(probes == 0);
}

static void test_on_quiet_probes_then_standby(void) {
    reset();
    power_core_alive(now);
    CHECK(run(PWR_IDLE_MS - 500) == 0);
    run(15000); // idle: probes, none answered
    CHECK(power_core_state() == PWR_STANDBY);
}

static void test_on_quiet_answered_stays_on(void) {
    reset();
    power_core_alive(now);
    run(PWR_IDLE_MS + 200); // the idle probe went out
    power_core_line("[COM] RT4KPRO, FW Version: 1.87.3", now);
    run(10000);
    CHECK(power_core_state() == PWR_ON);
}

static void test_mirror_silence_means_standby(void) {
    reset();
    power_core_alive(now);
    power_core_silent(now);
    power_core_silent(now);
    CHECK(power_core_state() == PWR_ON);
    power_core_silent(now);
    CHECK(power_core_state() == PWR_STANDBY);
}

static void test_one_answer_resets_silence(void) {
    reset();
    power_core_alive(now);
    power_core_silent(now);
    power_core_silent(now);
    power_core_alive(now);
    power_core_silent(now);
    power_core_silent(now);
    CHECK(power_core_state() == PWR_ON);
}

static void test_disconnect_forgets(void) {
    reset();
    power_core_alive(now);
    power_core_disconnected(now);
    CHECK(power_core_state() == PWR_UNKNOWN);
}

static void test_names(void) {
    CHECK(!strcmp(power_state_name(PWR_ON), "on"));
    CHECK(!strcmp(power_state_name(PWR_STANDBY), "standby"));
    CHECK(!strcmp(power_state_name(PWR_BOOTING), "starting"));
    CHECK(!strcmp(power_state_name(PWR_UNKNOWN), "unknown"));
}

int main(void) {
    RUN(test_starts_unknown_and_probes);
    RUN(test_answer_means_on);
    RUN(test_other_lines_prove_nothing);
    RUN(test_unknown_silence_means_standby);
    RUN(test_break_means_standby);
    RUN(test_standby_probes_slowly);
    RUN(test_ir_power_on_noticed);
    RUN(test_wake_then_boot_then_on);
    RUN(test_pwr_on_while_on);
    RUN(test_power_toggle_means_standby);
    RUN(test_boot_gives_up);
    RUN(test_on_with_traffic_does_not_probe);
    RUN(test_on_quiet_probes_then_standby);
    RUN(test_on_quiet_answered_stays_on);
    RUN(test_mirror_silence_means_standby);
    RUN(test_one_answer_resets_silence);
    RUN(test_disconnect_forgets);
    RUN(test_names);
    return tests_done();
}
