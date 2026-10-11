#include "health.h"
#include "health_rp2.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "hardware/adc.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/scb.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"

#include "log.h"
#include "platform.h"

#define WDT_TIMEOUT_MS    8000
#define WDT_FEED_MS       1000
#define WDT_TASK_PRIORITY (tskIDLE_PRIORITY + 2) // with the busy tasks (http, ws): time slicing gives it
                                                 // a turn; at 1 an OTA upload starved it for ~4 s

static volatile uint32_t last_feed_ms;
volatile uint32_t health_feed_us; // the same, in timer microseconds, for the freeze recorder's ISR
static volatile bool rebooting; // stop feeding: a reboot is scheduled on the watchdog
static volatile bool armed;     // our watchdog is in charge (not during a TBYB trial)

// Runs in the timer task (highest priority): if the feeder is being starved, say who holds the cores
// before the watchdog resets the board (the log survives the reset).
static void starve_check(TimerHandle_t t) {
    (void)t;
    const uint32_t late = plat_ms() - last_feed_ms;
    if (!armed || !last_feed_ms || late < 3000) return;
    char line[96];
    snprintf(line, sizeof(line), "\n*** watchdog not fed for %lu ms; core0=%s core1=%s\n", (unsigned long)late,
        pcTaskGetName(xTaskGetCurrentTaskHandleForCore(0)), pcTaskGetName(xTaskGetCurrentTaskHandleForCore(1)));
    log_write_raw(line);
}

static void wdt_task(void *param) {
    (void)param;
    for (;;) {
        if (health_net_dead()) vTaskDelete(NULL);
        // watchdog_update() reloads the counter with our 8 s: it would postpone a reboot that
        // rom_reboot()/watchdog_reboot() scheduled on the same watchdog, forever.
        if (armed && !rebooting) watchdog_update();
        last_feed_ms = plat_ms();
        health_feed_us = timer_hw->timerawl;
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_MS));
    }
}

void health_start(bool trial) {
    xTimerStart(xTimerCreate("starve", pdMS_TO_TICKS(1000), pdTRUE, NULL, starve_check), 0);
    xTaskCreate(wdt_task, "wdt", 256, NULL, WDT_TASK_PRIORITY, NULL);
    if (trial) {
        // An image on trial is watched by the boot ROM (reset and roll back unless confirmed within
        // ~16.7 s). Arming and feeding our own watchdog now would defeat that: a new image hanging
        // early would never roll back. Ours takes over after the confirmation (health_rearm_watchdog).
        printf("health: trial boot, the boot ROM's watchdog is in charge until confirmed\n");
        return;
    }
    health_rearm_watchdog();
    printf("health: watchdog %u ms\n", WDT_TIMEOUT_MS);
}

void health_stop_feeding(void) {
    rebooting = true;
}

void health_rearm_watchdog(void) {
    watchdog_enable(WDT_TIMEOUT_MS, true); // pause_on_debug: a debugger halting the cores doesn't reset the board
    armed = true;
}

// --- sensors (health.h) --------------------------------------------------------------------------
// VSYS/3 is on GPIO29 (ADC3), which is also the CYW43's SPI clock: it's read with the CYW43's lock
// held, and the driver takes the pin back at its next transfer (as pico-examples' power_status does).

#define SENSOR_PERIOD_MS 100
#define SUPPLY_BURST     256 // reads per sample, ~2 µs each: ~0.5 ms holding the CYW43
#define SAMPLE_RING      32u // power of two; a Debug report (every 2 s) takes 20
#define TEMP_EVERY       10  // samples: the temperature once a second

static health_sample_t samples[SAMPLE_RING];
static volatile uint32_t sample_seq;       // samples ever taken (samples[(seq - 1) % SAMPLE_RING] is the newest)
static volatile uint32_t supply_lowest_mv = UINT32_MAX;
static volatile int8_t usb_power = -1;
static volatile bool temperature_known;
static volatile int32_t temperature_dc;

static uint32_t adc_to_mv(uint32_t raw) {
    return raw * 3300u / 4096u; // 12 bits over the 3.3 V reference
}

static void sensors_task(void *param) {
    (void)param;
    adc_init();
    adc_set_temp_sensor_enabled(true);
    for (uint32_t n = 0;; n++) {
        cyw43_thread_enter();
        const bool vbus = cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN); // also wakes it: its clock line goes quiet
        adc_gpio_init(PICO_VSYS_PIN);
        adc_select_input(PICO_VSYS_PIN - ADC_BASE_PIN);
        for (int i = 0; i < 3; i++) (void)adc_read(); // the first reads after switching come out low
        uint32_t sum = 0, lowest = 0xfff;
        for (int i = 0; i < SUPPLY_BURST; i++) {
            const uint32_t v = adc_read();
            sum += v;
            if (v < lowest) lowest = v;
        }
        cyw43_thread_exit();
        const uint32_t avg_mv = 3 * adc_to_mv(sum / SUPPLY_BURST), min_mv = 3 * adc_to_mv(lowest); // VSYS/3
        if (n % TEMP_EVERY == 0) {
            // RP2350 datasheet: 0.706 V at 27 °C, -1.721 mV per °C.
            adc_select_input(NUM_ADC_CHANNELS - 1);
            (void)adc_read();
            uint32_t t = 0;
            for (int i = 0; i < 16; i++) t += adc_read();
            const int32_t uv = (int32_t)(t * 3300000ull / 4096u / 16u);
            temperature_dc = 270 - (uv - 706000) * 10 / 1721;
            temperature_known = true;
        }
        usb_power = vbus;
        if (min_mv < supply_lowest_mv) supply_lowest_mv = min_mv;
        health_sample_t *s = &samples[sample_seq % SAMPLE_RING];
        s->supply_mv = (uint16_t)avg_mv;
        s->supply_min_mv = (uint16_t)min_mv;
        s->temperature_dc = (int16_t)temperature_dc;
        __dmb(); // the sample before its number
        sample_seq++;
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

void health_start_sensors(void) {
    // The CYW43's own task's stack: while cyw43_arch_gpio_get() waits for the chip's answer, the driver
    // hands the packets that arrive meanwhile to lwIP on this task's stack (cyw43_do_ioctl). With 512
    // words, 14 were left.
    xTaskCreate(sensors_task, "sensors", CYW43_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
}

bool health_sensors(health_sensors_t *out) {
    const uint32_t seq = sample_seq;
    *out = (health_sensors_t){.usb_power = -1};
    if (!seq) return true; // not sampled yet: nothing known
    out->supply = true;
    out->supply_mv = samples[(seq - 1) % SAMPLE_RING].supply_mv;
    out->supply_min_mv = supply_lowest_mv;
    out->usb_power = usb_power;
    out->temperature = temperature_known;
    out->temperature_dc = temperature_dc;
    return true;
}

size_t health_sensor_samples(uint32_t *seq, health_sample_t *out, size_t max) {
    if (max > SAMPLE_RING - 1) max = SAMPLE_RING - 1; // the oldest slot may be being rewritten
    const uint32_t newest = sample_seq;
    uint32_t from = *seq;
    if (newest - from > max || from > newest) from = newest > max ? newest - max : 0; // behind, or from before a reset
    size_t n = 0;
    for (uint32_t k = from; k != newest; k++) out[n++] = samples[k % SAMPLE_RING];
    *seq = newest;
    return n;
}

// --- self-test of the fault reports ------------------------------------------------------------
// In RAM, since the "flash" kind stops flash answering under it.

static void __not_in_flash_func(fault_task)(void *param) {
    vTaskDelay(pdMS_TO_TICKS(300)); // the request's answer leaves first
    if (param) {
        // In the QMI's direct mode (a flash write's), every XIP access is a bus error: flash stops
        // answering the way it does when its chip browns out. This read faults first; the other core
        // faults on its next read from flash.
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
        (void)*(const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    } else {
        __asm volatile("udf #0"); // an undefined instruction: a UsageFault, escalated to a HardFault
    }
    for (;;) __asm volatile("nop");
}

bool health_fault_test(const char *kind) {
    const bool flash = !strcmp(kind, "flash");
    if (!flash && strcmp(kind, "task")) return false;
    xTaskCreate(fault_task, "fault", 256, flash ? (void *)1 : NULL, configMAX_PRIORITIES - 2, NULL);
    return true;
}

// --- faults ------------------------------------------------------------------------------------
// Log where it happened, stop feeding the watchdog, and spin: the watchdog resets the board and the
// log survives the reset. Without the stop, the other core kept feeding it: the board stayed up with
// one core dead (and the RT4K's USB with it, on core 1), found by /debug/fault?kind=task.
// All of it runs from RAM and reads nothing in flash (not even a string literal): a handler in flash
// can't run when the fault came from flash failing, and the core locks up instead (both cores did,
// the freeze record showed; see freeze.c).

static void __not_in_flash_func(hex32)(char *out, uint32_t v) {
    for (int i = 7; i >= 0; i--, v >>= 4) out[i] = (char)((v & 15) < 10 ? '0' + (v & 15) : 'a' + (v & 15) - 10);
}

// One per core (they can fault at once), filled in place: each '=' is followed by a value, in order.
static char fault_line[2][160] = {
    [0 ... 1] = "\n*** HardFault core ? pc=........ lr=........ r0=........ r1=........ r2=........ r3=........ "
                "cfsr=........ hfsr=........ mmfar=........ bfar=........\n",
};

const uint32_t *volatile health_fault_frame[2];

void __attribute__((used)) __not_in_flash_func(hardfault_report)(const uint32_t *frame) {
    rebooting = true; // the feeder (wdt_task) stops, on whichever core it runs
    const uint core = get_core_num();
    health_fault_frame[core] = frame;
    // Fault status: CFSR (MMFSR|BFSR|UFSR), HFSR, and the faulting addresses if valid.
    const uint32_t values[] = {frame[6], frame[5], frame[0], frame[1], frame[2], frame[3], scb_hw->cfsr, scb_hw->hfsr,
        scb_hw->mmfar, scb_hw->bfar};
    char *line = fault_line[core];
    char *p = line;
    while (*p != '?') p++;
    *p = (char)('0' + core);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        while (*p != '=') p++;
        hex32(++p, values[i]);
    }
    log_write_raw(line);
    for (;;) __asm volatile("nop");
}

// Picks the stack the exception frame was pushed to (MSP or PSP) and hands it to the C code.
void __attribute__((naked, section(".time_critical.isr_hardfault"))) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4       \n"
        "ite eq           \n"
        "mrseq r0, msp    \n"
        "mrsne r0, psp    \n"
        "b hardfault_report\n");
}
