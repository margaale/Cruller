#include "flash_ops.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/time.h"

#include "rt4k.h"

#define FLASH_SAFE_TIMEOUT_MS 1000

typedef struct {
    uint32_t offset;
    const void *data;
    size_t count;
} flash_op_t;

static SemaphoreHandle_t quiet_lock; // recursive: OTA holds it for the whole image, each write nests
static int quiet_depth;

void flash_ops_init(void) {
    quiet_lock = xSemaphoreCreateRecursiveMutex();
}

bool flash_quiet_begin(void) {
    xSemaphoreTakeRecursive(quiet_lock, portMAX_DELAY);
    if (quiet_depth == 0 && !rt4k_suspend()) {
        printf("flash: could not stop the USB host\n");
        rt4k_resume();
        xSemaphoreGiveRecursive(quiet_lock);
        return false;
    }
    quiet_depth++;
    return true;
}

void flash_quiet_end(void) {
    if (--quiet_depth == 0) rt4k_resume();
    xSemaphoreGiveRecursive(quiet_lock);
}

// flash_safe_execute() runs the write with interrupts off on both cores, the tick's (core 0) too: a
// sector erase is tens of ms of ticks that never happen (one pending tick is all SysTick keeps). An
// OTA image is ~230 of them, back to back with the upload, and tick time fell to a fraction of real
// time: the watchdog feeder's 1 s delay took up to 7 s, the gateway ping's 2 s over 10 s (the health
// check gave up on the network), lwIP's timers crawled. So after each write the tick count is moved
// up to real time again.
static bool tick_base_known;
static uint32_t tick_base_ms;     // the time since boot at tick 0, as the ticks count it
static uint32_t ticks_restored;   // in all, for the log

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static int run_safe(void (*fn)(void *), flash_op_t *op) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return flash_safe_execute(fn, op, FLASH_SAFE_TIMEOUT_MS);
    if (!tick_base_known) {
        tick_base_ms = now_ms() - (uint32_t)xTaskGetTickCount();
        tick_base_known = true;
    }
    const int rc = flash_safe_execute(fn, op, FLASH_SAFE_TIMEOUT_MS);
    // Against the base, not this write's own length, so the rounding doesn't add up over an image.
    // The pending tick is in already: both cores turn interrupts back on before this returns.
    const int32_t behind = (int32_t)(now_ms() - tick_base_ms - (uint32_t)xTaskGetTickCount());
    if (behind > 0) {
        xTaskCatchUpTicks((TickType_t)behind);
        ticks_restored += (uint32_t)behind;
    }
    return rc;
}

uint32_t flash_ticks_restored_ms(void) {
    return ticks_restored;
}

static void do_erase(void *param) {
    const flash_op_t *op = param;
    flash_range_erase(op->offset, op->count);
}

static void do_program(void *param) {
    const flash_op_t *op = param;
    flash_range_program(op->offset, op->data, op->count);
}

bool flash_erase_safe(uint32_t offset, size_t count) {
    if (!flash_quiet_begin()) return false;
    flash_op_t op = {offset, NULL, count};
    const int rc = run_safe(do_erase, &op);
    flash_quiet_end();
    if (rc != PICO_OK) printf("flash: erase at 0x%06lx failed (%d)\n", (unsigned long)offset, rc);
    return rc == PICO_OK;
}

bool flash_program_safe(uint32_t offset, const void *data, size_t count) {
    if (!flash_quiet_begin()) return false;
    flash_op_t op = {offset, data, count};
    const int rc = run_safe(do_program, &op);
    flash_quiet_end();
    if (rc != PICO_OK) printf("flash: program at 0x%06lx failed (%d)\n", (unsigned long)offset, rc);
    return rc == PICO_OK;
}
