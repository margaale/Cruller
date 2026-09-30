#include "freeze.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/exception.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/m33_eppb.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/scb.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "health.h"

#define PERIOD_US    250000u
#define SAMPLES      16u      // per core: the last 4 s (the watchdog fires after 8 s without feeding)
#define NAME_LEN     12
#define FREEZE_MAGIC 0x46525a33u // "FRZ3" (FRZ2 had no fault capture: a record of it is dropped)
#define LOCKUP_PC    0xeffffffeu // a locked-up core's PC: it faulted again in its HardFault handler

typedef struct {
    uint32_t us, pc, lr;
    uint32_t wdt_left_ms; // watchdog time remaining
    uint32_t fed_ago_ms;  // since the feeder task last ran (health.c)
    uint16_t exc;         // what was interrupted: 0 a task, else its exception number (IRQ n: 16 + n)
    uint8_t primask;      // 1: with its interrupts off (a pico critical section or spin lock)
    uint8_t basepri;      // not 0: in a FreeRTOS critical section
    char task[NAME_LEN];
} sample_t;

enum { FLASH_NOT_READ, FLASH_OK, FLASH_WRONG, FLASH_DIRECT };

// A core found in its HardFault handler (or locked up in it): what faulted, taken once per record.
typedef struct {
    uint32_t pc, lr, xpsr;          // from the faulting code's exception frame (pc 0: not found)
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t flash_word;            // what the flash check read
    uint8_t taken;
    uint8_t flash;                  // FLASH_*
} fault_t;

typedef struct {
    uint32_t magic;
    struct {
        uint32_t count; // samples ever taken
        sample_t s[SAMPLES];
        fault_t fault;
    } core[2];
} record_t;

// Read through the uncached XIP window after a fault: the flash still answers with it, or it doesn't
// (a flash chip reset by a supply dip reads back garbage, and XIP gets bus errors while the QMI is in
// direct mode for a flash write).
#define FLASH_CHECK_VALUE 0xc0ffee42u
static const volatile uint32_t __attribute__((used)) flash_check = FLASH_CHECK_VALUE;

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

// An exception frame at sp, if sp could hold one (in RAM, aligned): a fault's own stack pointer can
// be anything. exc_return says whether the extra state context (Secure to Non-secure) comes first.
static const uint32_t *__not_in_flash_func(frame_at)(uint32_t sp, uint32_t exc_return) {
    if (!(exc_return & (1u << 5))) sp += 40;
    return sp % 4 == 0 && sp >= SRAM_BASE && sp <= SRAM_END - 32 ? (const uint32_t *)sp : NULL;
}

// The first sample in a HardFault: the fault status, the faulting code's PC and LR, and a flash read.
// The handler (health.c) says where the fault's frame is. When it couldn't even start (a lockup), the
// NMI's frame tells: it was pushed while locked up in the HardFault, so its LR is still the HardFault's
// EXC_RETURN, which says which stack that frame is on.
static void __not_in_flash_func(take_fault)(fault_t *f, uint core, const uint32_t *frame, uint32_t nmi_return) {
    f->taken = 1;
    f->cfsr = scb_hw->cfsr;
    f->hfsr = scb_hw->hfsr;
    f->mmfar = scb_hw->mmfar;
    f->bfar = scb_hw->bfar;
    const uint32_t *orig = health_fault_frame[core];
    const uint32_t hf_return = frame[5];
    if (!orig && hf_return >> 24 == 0xffu) {
        uint32_t sp;
        if (hf_return & (1u << 2)) {
            __asm volatile("mrs %0, psp" : "=r"(sp)); // from a task
        } else {
            // From a handler: its frame is right above the NMI's (8 words, 26 with FP state, one of padding).
            sp = (uint32_t)frame + (nmi_return & (1u << 4) ? 32u : 104u) + (frame[7] & (1u << 9) ? 4u : 0u);
        }
        orig = frame_at(sp, hf_return);
    }
    if (orig) {
        f->pc = orig[6];
        f->lr = orig[5];
        f->xpsr = orig[7];
    }
    // Last: if the flash read itself faults, what came before is kept.
    if (qmi_hw->direct_csr & QMI_DIRECT_CSR_EN_BITS) {
        f->flash = FLASH_DIRECT; // XIP answers with bus errors meanwhile: not read
    } else {
        const uintptr_t at = (uintptr_t)&flash_check - XIP_BASE + XIP_NOCACHE_NOALLOC_BASE;
        f->flash_word = *(const volatile uint32_t *)at;
        f->flash = f->flash_word == FLASH_CHECK_VALUE ? FLASH_OK : FLASH_WRONG;
    }
}

// Called by freeze_isr with the exception frame (r0-r3, r12, lr, pc, xpsr) of what was interrupted,
// and the NMI's own EXC_RETURN.
void __attribute__((used)) __not_in_flash_func(freeze_sample)(const uint32_t *frame, uint32_t nmi_return) {
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
    if ((s->exc == 3 || s->pc == LOCKUP_PC) && !rec.core[core].fault.taken) take_fault(&rec.core[core].fault, core, frame, nmi_return);
}

// Picks the stack the exception frame was pushed to (MSP or PSP) and hands it to the C code, with the
// EXC_RETURN.
void __attribute__((naked, section(".time_critical.freeze_isr"))) freeze_isr(void) {
    __asm volatile(
        "tst lr, #4       \n"
        "ite eq           \n"
        "mrseq r0, msp    \n"
        "mrsne r0, psp    \n"
        "mov r1, lr       \n"
        "b freeze_sample  \n");
}

// The newest sample time of either core: the record's times are relative to it.
static uint32_t newest_us(void) {
    uint32_t newest = 0;
    bool any = false;
    for (int c = 0; c < 2; c++) {
        if (!rec.core[c].count) continue;
        const uint32_t us = rec.core[c].s[(rec.core[c].count - 1) % SAMPLES].us;
        if (!any || (int32_t)(us - newest) > 0) newest = us;
        any = true;
    }
    return newest;
}

// The CFSR's set bits by name (MemManage, BusFault, UsageFault).
static void cfsr_names(uint32_t cfsr, char *out, size_t size) {
    static const struct {
        uint8_t bit;
        const char *name;
    } bits[] = {
        {0, "IACCVIOL"}, {1, "DACCVIOL"}, {3, "MUNSTKERR"}, {4, "MSTKERR"}, {5, "MLSPERR"}, {7, "MMARVALID"},
        {8, "IBUSERR"}, {9, "PRECISERR"}, {10, "IMPRECISERR"}, {11, "UNSTKERR"}, {12, "STKERR"}, {13, "LSPERR"},
        {15, "BFARVALID"}, {16, "UNDEFINSTR"}, {17, "INVSTATE"}, {18, "INVPC"}, {19, "NOCP"}, {20, "STKOF"},
        {24, "UNALIGNED"}, {25, "DIVBYZERO"},
    };
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]) && o < size; i++) {
        if (cfsr & (1u << bits[i].bit)) o += (size_t)snprintf(out + o, size - o, "%s%s", o ? " " : "", bits[i].name);
    }
    if (!o) snprintf(out, size, "none");
}

// One core's part of the record as text. After the task: in a task or an exception ("irq n", "exc n":
// 3 HardFault, 14 PendSV, 15 SysTick, "lockup": faulted again in the HardFault handler), and its
// interrupts (on, "OFF": all masked, "rtos": in a FreeRTOS critical section). Then what faulted, if
// the core was found in a HardFault.
static size_t format_core(int c, uint32_t newest, char *out, size_t size) {
    size_t o = 0;
    out[0] = 0;
    const uint32_t count = rec.core[c].count;
    const uint32_t n = count < SAMPLES ? count : SAMPLES;
    if (!n) o += (size_t)snprintf(out + o, size - o, "core %d: no samples\n", c);
    for (uint32_t k = count - n; k < count && o < size; k++) {
        const sample_t *s = &rec.core[c].s[k % SAMPLES];
        char task[NAME_LEN];
        memcpy(task, s->task, NAME_LEN);
        task[NAME_LEN - 1] = 0;
        char where[12];
        if (s->pc == LOCKUP_PC) snprintf(where, sizeof(where), "lockup");
        else if (!s->exc) snprintf(where, sizeof(where), "task");
        else if (s->exc >= 16) snprintf(where, sizeof(where), "irq %u", (unsigned)(s->exc - 16));
        else snprintf(where, sizeof(where), "exc %u", (unsigned)s->exc);
        // Signed: a sample the reset cut short (its count not yet stepped) can be newer than "newest".
        const long ms = (long)((int32_t)(s->us - newest) / 1000);
        o += (size_t)snprintf(out + o, size - o,
            "core %d %6ld ms %-11s pc=%08lx lr=%08lx %-6s ints %-4s wdt left %lu fed %lu ms ago\n", c, ms,
            task, (unsigned long)s->pc, (unsigned long)s->lr, where, s->primask ? "OFF" : s->basepri ? "rtos" : "on",
            (unsigned long)s->wdt_left_ms, (unsigned long)s->fed_ago_ms);
    }
    const fault_t *f = &rec.core[c].fault;
    if (f->taken && o < size) {
        char names[96], flash[48], mmfar[12] = "-", bfar[12] = "-"; // the addresses only when valid
        cfsr_names(f->cfsr, names, sizeof(names));
        if (f->cfsr & (1u << 7)) snprintf(mmfar, sizeof(mmfar), "%08lx", (unsigned long)f->mmfar);
        if (f->cfsr & (1u << 15)) snprintf(bfar, sizeof(bfar), "%08lx", (unsigned long)f->bfar);
        if (f->flash == FLASH_OK) snprintf(flash, sizeof(flash), "reads fine");
        else if (f->flash == FLASH_WRONG) snprintf(flash, sizeof(flash), "reads WRONG (%08lx)", (unsigned long)f->flash_word);
        else if (f->flash == FLASH_DIRECT) snprintf(flash, sizeof(flash), "in direct mode (a flash write)");
        else snprintf(flash, sizeof(flash), "not read");
        o += (size_t)snprintf(out + o, size - o,
            "core %d fault: pc=%08lx lr=%08lx xpsr=%08lx cfsr=%08lx (%s) hfsr=%08lx mmfar=%s bfar=%s; flash %s\n",
            c, (unsigned long)f->pc, (unsigned long)f->lr, (unsigned long)f->xpsr, (unsigned long)f->cfsr, names,
            (unsigned long)f->hfsr, mmfar, bfar, flash);
    }
    return o < size ? o : size - 1;
}

size_t freeze_dump(char *out, size_t size) {
    const uint32_t newest = newest_us();
    const size_t o = format_core(0, newest, out, size);
    return o + format_core(1, newest, out + o, size - o);
}

void freeze_report(void) {
    // Watchdog reset reason: bit 0 the timer ran out, bit 1 forced (watchdog_reboot(), rom_reboot()).
    const uint32_t reason = watchdog_hw->reason;
    if (rec.magic == FREEZE_MAGIC && watchdog_caused_reboot()) {
        static char text[SAMPLES * 128 + 256]; // one core at a time
        printf("\n*** freeze record, watchdog reason 0x%lx (%s) (ms relative to the last sample):\n",
            (unsigned long)reason, reason & 1 ? "timeout" : reason & 2 ? "forced" : "?");
        const uint32_t newest = newest_us();
        for (int c = 0; c < 2; c++) {
            format_core(c, newest, text, sizeof(text));
            printf("%s", text);
        }
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
