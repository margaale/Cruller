#include "flash_ops.h"

#include "hardware/flash.h"
#include "pico/flash.h"

#define FLASH_SAFE_TIMEOUT_MS 1000

typedef struct {
    uint32_t offset;
    const void *data;
    size_t count;
} flash_op_t;

static void do_erase(void *param) {
    const flash_op_t *op = param;
    flash_range_erase(op->offset, op->count);
}

static void do_program(void *param) {
    const flash_op_t *op = param;
    flash_range_program(op->offset, op->data, op->count);
}

bool flash_erase_safe(uint32_t offset, size_t count) {
    flash_op_t op = {offset, NULL, count};
    return flash_safe_execute(do_erase, &op, FLASH_SAFE_TIMEOUT_MS) == PICO_OK;
}

bool flash_program_safe(uint32_t offset, const void *data, size_t count) {
    flash_op_t op = {offset, data, count};
    return flash_safe_execute(do_program, &op, FLASH_SAFE_TIMEOUT_MS) == PICO_OK;
}
