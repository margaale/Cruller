#include "power.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/sync.h"
#include "pico/time.h"

#include "console.h"
#include "rt4k.h"

#define POWER_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define POWER_TICK_MS       200

static critical_section_t lock; // every power_core call: events come from several tasks

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

power_state_t power_state(void) {
    critical_section_enter_blocking(&lock);
    const power_state_t s = power_core_state();
    critical_section_exit(&lock);
    return s;
}

void power_feed_line(const char *line) {
    critical_section_enter_blocking(&lock);
    power_core_line(line, now_ms());
    critical_section_exit(&lock);
}

#define EVENT(name, call)                        \
    void name(void) {                            \
        critical_section_enter_blocking(&lock);  \
        call(now_ms());                          \
        critical_section_exit(&lock);            \
    }
EVENT(power_break, power_core_break)
EVENT(power_alive, power_core_alive)
EVENT(power_silent, power_core_silent)
EVENT(power_woken, power_core_woken)

static void power_task(void *param) {
    (void)param;
    bool connected = false;
    power_state_t logged = PWR_UNKNOWN;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POWER_TICK_MS));
        const bool now_connected = rt4k_connected();
        critical_section_enter_blocking(&lock);
        if (connected && !now_connected) power_core_disconnected(now_ms());
        const bool probe = now_connected && power_core_poll(now_ms());
        const power_state_t s = power_core_state();
        critical_section_exit(&lock);
        connected = now_connected;
        if (s != logged) {
            printf("power: RT4K %s\n", power_state_name(s));
            logged = s;
        }
        if (probe) console_send(CON_POWER, "ver"); // answered when on, ignored in standby; nobody sees it
    }
}

void power_start(void) {
    critical_section_init(&lock);
    power_core_init(now_ms());
    xTaskCreate(power_task, "power", 512, NULL, POWER_TASK_PRIORITY, NULL);
}
