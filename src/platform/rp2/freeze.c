#include "freeze.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/exception.h"
#include "hardware/structs/m33_eppb.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "health.h"

#define PERIOD_US    250000u
#define SAMPLES      16u      // per core: the last 4 s (the watchdog fires after 8 s without feeding)
#define NAME_LEN     12
#define FREEZE_MAGIC 0x46525a32u // "FRZ2" (FRZ1's samples had no context and masks: a record of it is dropped)

typedef struct {
    uint32_t us, pc, lr;
    uint32_t wdt_left_ms; // watchdog time remaining
    uint32_t fed_ago_ms;  // since the feeder task last ran (health.c)
    uint16_t exc;         // what was interrupted: 0 a task, else its exception number (IRQ n: 16 + n)
    uint8_t primask;      // 1: with its interrupts off (a pico critical section or spin lock)
    uint8_t basepri;      // not 0: in a FreeRTOS critical section
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

// The sample is taken in an NMI: the timer alarm's IRQ is routed to the core's NMI and never enabled
// in the NVIC. Unlike an IRQ it also samples a core with its interrupts off, and both cores of a frozen
// board were exactly that (FreeRTOS critical sections only raise BASEPRI, an IRQ at priority 0 still
// saw into those). Samples then stop only on a core stuck on a bus access, halted by the RCP (a boot
// ROM check that failed: see ota.c's rom_pin()), or locked up.
// The handler touches nothing in flash (only registers and RAM; the vector table is in RAM), so it
// keeps sampling even if flash access (XIP) stalls: then the record shows where each core is stuck.
// The running task's name is read straight from its TCB: FreeRTOS keeps the current TCB per core in
// pxCurrentTCBs, and the name's offset in a TCB is measured once.
extern void *volatile pxCurrentTCBs[];
static uint32_t name_offset; // 0: not measured yet
static char no_task[] = "-"; // in RAM, like everything the handler reads (a string literal is in flash)

// Called by freeze_isr with the exception frame (r0-r3, r12, lr, pc, xpsr) of what was interrupted.
void __attribute__((used)) __not_in_flash_func(freeze_sample)(const uint32_t *frame) {
    // The interrupted code's masks: taking an NMI changes neither.
    uint32_t primask, basepri;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("mrs %0, basepri" : "=r"(basepri));
    const uint core = get_core_num();
    const uint alarm = (uint)alarm_of[core];
    timer_hw->intr = 1u << alarm; // acknowledge
    timer_hw->alarm[alarm] = timer_hw->timerawl + PERIOD_US;
    if (rec.magic != FREEZE_MAGIC) return;
    sample_t *s = &rec.core[core].s[rec.core[core].count % SAMPLES];
    s->us = timer_hw->timerawl;
    s->pc = frame[6];
    s->lr = frame[5];
    s->exc = (uint16_t)(frame[7] & 0x1ffu); // the stacked xPSR's IPSR
    s->primask = (uint8_t)(primask & 1u);
    s->basepri = (uint8_t)basepri;
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

// The record as text. Times are relative to the newest sample of either core. After the task: in a
// task or an exception ("irq n", "exc n": 3 HardFault, 14 PendSV, 15 SysTick), and its interrupts
// (on, "OFF": all masked, "rtos": in a FreeRTOS critical section).
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
            char where[12];
            if (!s->exc) snprintf(where, sizeof(where), "task");
            else if (s->exc >= 16) snprintf(where, sizeof(where), "irq %u", (unsigned)(s->exc - 16));
            else snprintf(where, sizeof(where), "exc %u", (unsigned)s->exc);
            // Signed: a sample the reset cut short (its count not yet stepped) can be newer than "newest".
            const long ms = (long)((int32_t)(s->us - newest) / 1000);
            o += (size_t)snprintf(out + o, size - o,
                "core %d %6ld ms %-11s pc=%08lx lr=%08lx %-6s ints %-4s wdt left %lu fed %lu ms ago\n", c, ms,
                task, (unsigned long)s->pc, (unsigned long)s->lr, where, s->primask ? "OFF" : s->basepri ? "rtos" : "on",
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
        static char text[2 * SAMPLES * 128];
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
        exception_set_exclusive_handler(NMI_EXCEPTION, freeze_isr); // the same for both cores (one vector table)
        hw_set_bits(&eppb_hw->nmi_mask[irq / 32u], 1u << (irq % 32u)); // this core's NMI (the register is core-local)
        hw_set_bits(&timer_hw->inte, 1u << alarm);
        timer_hw->alarm[alarm] = timer_hw->timerawl + PERIOD_US;
    } else {
        printf("freeze: no free timer alarm for core %u\n", core);
    }
    vTaskDelete(NULL);
}

void freeze_start(void) {
    xTaskCreateAffinitySet(start_task, "frz0", 256, NULL, configMAX_PRIORITIES - 1, 1u << 0, NULL);
    xTaskCreateAffinitySet(start_task, "frz1", 256, NULL, configMAX_PRIORITIES - 1, 1u << 1, NULL);
}
