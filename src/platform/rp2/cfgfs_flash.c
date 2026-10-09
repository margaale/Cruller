// cfgfs_port.h on the Pico 2 W: littlefs in the data partition after store.h's records (flash_layout.h),
// read through the XIP window, written with flash_ops.h (the USB host held off once per cfgfs write).

#include "cfgfs_port.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "flash_layout.h"
#include "flash_ops.h"

_Static_assert(CFGFS_SIZE % FLASH_SECTOR_SIZE_B == 0 && CFGFS_SIZE >= 64u * 1024u, "cfgfs needs whole sectors");

static int fl_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
    memcpy(buffer, FLASH_RAW_PTR(CFGFS_OFFSET + block * c->block_size + off), size);
    return LFS_ERR_OK;
}

static int fl_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size) {
    return flash_program_safe(CFGFS_OFFSET + block * c->block_size + off, buffer, size) ? LFS_ERR_OK : LFS_ERR_IO;
}

static int fl_erase(const struct lfs_config *c, lfs_block_t block) {
    return flash_erase_safe(CFGFS_OFFSET + block * c->block_size, c->block_size) ? LFS_ERR_OK : LFS_ERR_IO;
}

static int fl_sync(const struct lfs_config *c) {
    (void)c;
    return LFS_ERR_OK;
}

static uint8_t read_buf[CFGFS_CACHE_SIZE], prog_buf[CFGFS_CACHE_SIZE], lookahead[16];
static const struct lfs_config cfg = {
    .read = fl_read, .prog = fl_prog, .erase = fl_erase, .sync = fl_sync,
    .read_size = 16, .prog_size = FLASH_PAGE_SIZE_B, .block_size = FLASH_SECTOR_SIZE_B, .block_count = CFGFS_SIZE / FLASH_SECTOR_SIZE_B,
    .cache_size = CFGFS_CACHE_SIZE, .lookahead_size = sizeof(lookahead), .block_cycles = 500,
    .read_buffer = read_buf, .prog_buffer = prog_buf, .lookahead_buffer = lookahead,
};

static SemaphoreHandle_t lock;

const struct lfs_config *cfgfs_port_config(void) {
    if (!lock) lock = xSemaphoreCreateRecursiveMutex(); // (the first call: cfgfs_mount, at start)
    return lock ? &cfg : NULL;
}

void cfgfs_port_lock(void) {
    if (lock) xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void cfgfs_port_unlock(void) {
    if (lock) xSemaphoreGiveRecursive(lock);
}

bool cfgfs_port_write_begin(void) {
    return flash_quiet_begin();
}

void cfgfs_port_write_end(void) {
    flash_quiet_end();
}
