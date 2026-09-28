#include "freeze.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "health.h"

#define PERIOD_US    250000u
#define SAMPLES      16u      // per core: the last 4 s (the watchdog fires after 8 s without feeding)
#define NAME_LEN     12
#define FREEZE_MAGIC 0x46525a31u // "FRZ1"

typedef struct {
    uint32_t us, pc, lr;
    uint32_t wdt_left_ms; // watchdog time remaining
    uint32_t fed_ago_ms;  // since the feeder task last ran (health.c)
    char task[NAME_LEN];
} sample_t;

typedef struct {
    uint32_t magic;
    struct {
        uint32_t count; // samples ever taken
        sample_t s[SAMPLES];
    } core[2];
} record_t;

static record_t __uninitialized_ram(rec);
static int alarm_of[2] = {-1, -1};

// The ISR touches nothing in flash (only registers and RAM), so it keeps sampling even if flash
// access (XIP) stalls: then the record shows where each core is stuck. If the samples stop anyway,
// interrupts are off on that core. The running task's name is read straight from its TCB: FreeRTOS
// keeps the current TCB per core in pxCurrentTCBs, and the name's offset in a TCB is measured once.
extern void *volatile pxCurrentTCBs[];
static uint32_t name_offset; // 0: not measured yet
static char no_task[] = "-"; // in RAM, like everything the ISR reads (a string literal is in flash)

// Called by freeze_isr with the exception frame (r0-r3, r12, lr, pc, xpsr) of what was interrupted.
void __attribute__((used)) __not_in_flash_func(freeze_sample)(const uint32_t *frame) {
    const uint core = get_core_num();
    const uint alarm = (uint)alarm_of[core];
    timer_hw->intr = 1u << alarm; // acknowledge
    timer_hw->alarm[alarm] = timer_hw->timerawl + PERIOD_US;
    if (rec.magic != FREEZE_MAGIC) return;
    sample_t *s = &rec.core[core].s[rec.core[core].count % SAMPLES];
    s->us = timer_hw->timerawl;
    s->pc = frame[6];
    s->lr = frame[5];
    s->wdt_left_ms = (watchdog_hw->ctrl & WATCHDOG_CTRL_TIME_BITS) / 1000; // the counter ticks in µs
    s->fed_ago_ms = (s->us - health_feed_us) / 1000;
    const char *tcb = (const char *)pxCurrentTCBs[core];
    const char *name = tcb && name_offset ? tcb + name_offset : no_task;
    for (int i = 0; i < NAME_LEN; i++) {
        s->task[i] = name[i];
        if (!name[i]) break;
    }
    s->task[NAME_LEN - 1] = 0;
    rec.core[core].count++;
}

// Picks the stack the exception frame was pushed to (MSP or PSP) and hands it to the C code.
void __attribute__((naked, section(".time_critical.freeze_isr"))) freeze_isr(void) {
    __asm volatile(
        "tst lr, #4       \n"
        "ite eq           \n"
        "mrseq r0, msp    \n"
        "mrsne r0, psp    \n"
        "b freeze_sample  \n");
}

// The record as text. Times are relative to the newest sample of either core.
static size_t format(char *out, size_t size) {
    uint32_t newest = 0;
    bool any = false;
    for (int c = 0; c < 2; c++) {
        if (!rec.core[c].count) continue;
        const uint32_t us = rec.core[c].s[(rec.core[c].count - 1) % SAMPLES].us;
        if (!any || (int32_t)(us - newest) > 0) newest = us;
        any = true;
    }
    size_t o = 0;
    out[0] = 0;
    for (int c = 0; c < 2 && o < size; c++) {
        const uint32_t count = rec.core[c].count;
        const uint32_t n = count < SAMPLES ? count : SAMPLES;
        if (!n) o += (size_t)snprintf(out + o, size - o, "core %d: no samples\n", c);
        for (uint32_t k = count - n; k < count && o < size; k++) {
            const sample_t *s = &rec.core[c].s[k % SAMPLES];
            char task[NAME_LEN];
            memcpy(task, s->task, NAME_LEN);
            task[NAME_LEN - 1] = 0;
            o += (size_t)snprintf(out + o, size - o, "core %d %6ld ms %-11s pc=%08lx lr=%08lx wdt left %lu fed %lu ms ago\n",
                c, -(long)((newest - s->us) / 1000), task, (unsigned long)s->pc, (unsigned long)s->lr,
                (unsigned long)s->wdt_left_ms, (unsigned long)s->fed_ago_ms);
        }
    }
    return o < size ? o : size - 1;
}

size_t freeze_dump(char *out, size_t size) {
    return format(out, size);
}

void freeze_report(void) {
    // Watchdog reset reason: bit 0 the timer ran out, bit 1 forced (watchdog_reboot(), rom_reboot()).
    const uint32_t reason = watchdog_hw->reason;
    if (rec.magic == FREEZE_MAGIC && watchdog_caused_reboot()) {
        static char text[2 * SAMPLES * 110];
        format(text, sizeof(text));
        printf("\n*** freeze record, watchdog reason 0x%lx (%s) (ms relative to the last sample):\n%s",
            (unsigned long)reason, reason & 1 ? "timeout" : reason & 2 ? "forced" : "?", text);
    }
    memset(&rec, 0, sizeof(rec));
    rec.magic = FREEZE_MAGIC;
}

static void start_task(void *param) {
    const uint core = get_core_num();
    (void)param;
    const TaskHandle_t self = xTaskGetCurrentTaskHandle(); // the name's offset in a TCB (see freeze_sample)
    name_offset = (uint32_t)(pcTaskGetName(self) - (const char *)self);
    const int alarm = hardware_alarm_claim_unused(false);
    if (alarm >= 0) {
        alarm_of[core] = alarm;
        const uint irq = hardware_alarm_get_irq_num((uint)alarm);
        irq_set_exclusive_handler(irq, freeze_isr);
        irq_set_priority(irq, PICO_HIGHEST_IRQ_PRIORITY); // samples inside other handlers too
        hw_set_bits(&timer_hw->inte, 1u << alarm);
        timer_hw->alarm[alarm] = timer_hw->timerawl + PERIOD_US;
        irq_set_enabled(irq, true); // on this core
    } else {
        printf("freeze: no free timer alarm for core %u\n", core);
    }
    vTaskDelete(NULL);
}

void freeze_start(void) {
    xTaskCreateAffinitySet(start_task, "frz0", 256, NULL, configMAX_PRIORITIES - 1, 1u << 0, NULL);
    xTaskCreateAffinitySet(start_task, "frz1", 256, NULL, configMAX_PRIORITIES - 1, 1u << 1, NULL);
}
