// Host unit tests for the remote's buttons by name (src/core/buttons.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "buttons.h"

static int failures, checks;

// The console command for a button is `want`.
static void expect(const char *button, const char *want) {
    char out[64];
    buttons_command(button, out, sizeof(out));
    checks++;
    if (strcmp(out, want)) {
        failures++;
        printf("  FAIL \"%s\" -> \"%s\", want \"%s\"\n", button, out, want);
    }
}

int main(void) {
    // Power, as hass-RT4K sends it: "pwr on" is the one command without "remote".
    expect("power_on", "pwr on");
    expect("pwr_on", "pwr on");
    expect("power_off", "remote pwr");
    expect("power", "remote pwr");
    expect("pwr", "remote pwr");

    // The RT4K's own keys go through.
    expect("menu", "remote menu");
    expect("ok", "remote ok");
    expect("diag", "remote diag");
    expect("prof12", "remote prof12");
    expect("aux8", "remote aux8");

    // hass-RT4K's other names become the RT4K's key.
    expect("enter", "remote ok");
    expect("diagnostics", "remote diag");
    expect("statistics", "remote stat");
    expect("cropping", "remote scaler");
    expect("processing", "remote sfx");
    expect("effects", "remote sfx");
    expect("color", "remote col");
    expect("audio", "remote aud");
    expect("profiles", "remote prof");
    expect("profile1", "remote prof1");
    expect("profile10", "remote prof10");
    expect("profile12", "remote prof12");
    expect("auto_gain", "remote gain");
    expect("auto_phase", "remote phase");
    expect("safe_mode", "remote safe");
    expect("gen_lock", "remote genlock");
    expect("triple_buffer", "remote buffer");
    expect("4k", "remote res4k");
    expect("1080p", "remote res1080p");
    expect("1440p", "remote res1440p");
    expect("480p", "remote res480p");
    expect("custom1", "remote res1");
    expect("custom4", "remote res4");
    expect("auto_crop_vertical", "remote aux1");
    expect("auto_crop_4_3", "remote aux2");
    expect("auto_crop_16_9", "remote aux3");

    // Any case, like hass-RT4K (it lowercases first).
    expect("MENU", "remote menu");
    expect("Power_On", "pwr on");
    expect("Diagnostics", "remote diag");

    // A name nobody maps goes through as a key; the RT4K refuses it if it doesn't have it.
    expect("profile13", "remote profile13");
    expect("", "remote ");

    // A small buffer is cut, not overrun.
    char small[8];
    buttons_command("diagnostics", small, sizeof(small));
    checks++;
    if (strcmp(small, "remote ")) {
        failures++;
        printf("  FAIL small buffer: \"%s\"\n", small);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
