// cfgfs_port.h on the ESP32: littlefs in the "cruller" data partition (partitions.csv). A board updated
// over the air keeps the partition table it was flashed with, which may not have it: the same place in
// the flash (unused till then) is registered by hand.

#include "cfgfs_port.h"

#include <stdio.h>

#include "esp_flash.h"
#include "esp_partition.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define PART_LABEL   "cruller"
#define PART_SUBTYPE 0x83 // littlefs
#define PART_OFFSET  0x820000 // (partitions.csv)
#define PART_SIZE    0x100000
#define BLOCK        4096

static const esp_partition_t *part;

static int pt_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
    return esp_partition_read(part, block * c->block_size + off, buffer, size) == ESP_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

static int pt_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size) {
    return esp_partition_write(part, block * c->block_size + off, buffer, size) == ESP_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

static int pt_erase(const struct lfs_config *c, lfs_block_t block) {
    return esp_partition_erase_range(part, block * c->block_size, c->block_size) == ESP_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

static int pt_sync(const struct lfs_config *c) {
    (void)c;
    return LFS_ERR_OK;
}

static uint8_t read_buf[CFGFS_CACHE_SIZE], prog_buf[CFGFS_CACHE_SIZE], lookahead[32];
static struct lfs_config cfg = {
    .read = pt_read, .prog = pt_prog, .erase = pt_erase, .sync = pt_sync,
    .read_size = 16, .prog_size = 256, .block_size = BLOCK,
    .cache_size = CFGFS_CACHE_SIZE, .lookahead_size = sizeof(lookahead), .block_cycles = 500,
    .read_buffer = read_buf, .prog_buffer = prog_buf, .lookahead_buffer = lookahead,
};

static SemaphoreHandle_t lock;

const struct lfs_config *cfgfs_port_config(void) {
    if (!part) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
        if (!part && esp_partition_register_external(esp_flash_default_chip, PART_OFFSET, PART_SIZE, PART_LABEL,
                         ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, &part) != ESP_OK) {
            printf("cfgfs: no \"%s\" partition, and none could be registered\n", PART_LABEL);
            part = NULL;
        }
        if (!part) return NULL;
        cfg.block_count = part->size / BLOCK;
    }
    if (!lock) lock = xSemaphoreCreateRecursiveMutex();
    return lock ? &cfg : NULL;
}

void cfgfs_port_lock(void) {
    if (lock) xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void cfgfs_port_unlock(void) {
    if (lock) xSemaphoreGiveRecursive(lock);
}

bool cfgfs_port_write_begin(void) {
    return true;
}

void cfgfs_port_write_end(void) {
}
