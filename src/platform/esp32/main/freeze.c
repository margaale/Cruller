// freeze.h on the ESP32-S3, for now: no sampling. A watchdog or panic reset already prints a
// backtrace on the console, and log.c keeps the log across it; this reports why the last run ended.

#include "freeze.h"

#include <stdio.h>

#include "esp_system.h"

static const char *reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "other";
    }
}

void freeze_report(void) {
    const esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
        printf("freeze: last run ended in a %s reset\n", reason_name(r));
    }
}

void freeze_start(void) {
}

size_t freeze_dump(char *out, size_t size) {
    const int n = snprintf(out, size, "No freeze recorder on this board. Last reset: %s.\n", reason_name(esp_reset_reason()));
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}
