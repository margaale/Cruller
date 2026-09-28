#include "power.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "platform.h"
#include "console.h"
#include "rt4k.h"

#define POWER_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define POWER_TICK_MS       200

static plat_lock_t lock; // every power_core call: events come from several tasks

static uint32_t now_ms(void) {
    return plat_ms();
}

power_state_t power_state(void) {
    plat_lock_enter(&lock);
    const power_state_t s = power_core_state();
    plat_lock_exit(&lock);
    return s;
}

void power_feed_line(const char *line) {
    plat_lock_enter(&lock);
    power_core_line(line, now_ms());
    plat_lock_exit(&lock);
}

#define EVENT(name, call)                        \
    void name(void) {                            \
        plat_lock_enter(&lock);  \
        call(now_ms());                          \
        plat_lock_exit(&lock);            \
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
        plat_lock_enter(&lock);
        if (connected && !now_connected) power_core_disconnected(now_ms());
        const bool probe = now_connected && power_core_poll(now_ms());
        const power_state_t s = power_core_state();
        plat_lock_exit(&lock);
        connected = now_connected;
        if (s != logged) {
            printf("power: RT4K %s\n", power_state_name(s));
            logged = s;
        }
        if (probe) console_send(CON_POWER, "ver"); // answered when on, ignored in standby; nobody sees it
    }
}

void power_start(void) {
    plat_lock_init(&lock);
    power_core_init(now_ms());
    xTaskCreate(power_task, "power", PLAT_STACK(512), NULL, POWER_TASK_PRIORITY, NULL);
}
