#include "flash_ops.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "hardware/flash.h"
#include "pico/flash.h"

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
    const int rc = flash_safe_execute(do_erase, &op, FLASH_SAFE_TIMEOUT_MS);
    flash_quiet_end();
    if (rc != PICO_OK) printf("flash: erase at 0x%06lx failed (%d)\n", (unsigned long)offset, rc);
    return rc == PICO_OK;
}

bool flash_program_safe(uint32_t offset, const void *data, size_t count) {
    if (!flash_quiet_begin()) return false;
    flash_op_t op = {offset, data, count};
    const int rc = flash_safe_execute(do_program, &op, FLASH_SAFE_TIMEOUT_MS);
    flash_quiet_end();
    if (rc != PICO_OK) printf("flash: program at 0x%06lx failed (%d)\n", (unsigned long)offset, rc);
    return rc == PICO_OK;
}
