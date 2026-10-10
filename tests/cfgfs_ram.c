// cfgfs_port.h for the host tests: a flash of RAM, 75 blocks of 4 KB, that can "lose power" after a
// number of writes (each later prog or erase fails, and what's flash stays as it was).

#include "cfgfs_ram.h"

#include <string.h>

#define BLOCK 4096
#define BLOCKS 75 // the Pico 2 W's 300 KB

static uint8_t flash[BLOCKS][BLOCK];
static int writes_left = -1; // -1: no cut

static int ram_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t size) {
    (void)c;
    memcpy(buf, &flash[b][off], size);
    return 0;
}

static int ram_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)c;
    if (writes_left == 0) return LFS_ERR_IO;
    if (writes_left > 0) writes_left--;
    const uint8_t *p = buf;
    for (lfs_size_t k = 0; k < size; k++) flash[b][off + k] &= p[k]; // NOR: bits only go to 0
    return 0;
}

static int ram_erase(const struct lfs_config *c, lfs_block_t b) {
    (void)c;
    if (writes_left == 0) return LFS_ERR_IO;
    if (writes_left > 0) writes_left--;
    memset(flash[b], 0xff, BLOCK);
    return 0;
}

static int ram_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

static uint8_t read_buf[CFGFS_CACHE_SIZE], prog_buf[CFGFS_CACHE_SIZE], lookahead[16];
static const struct lfs_config cfg = {
    .read = ram_read, .prog = ram_prog, .erase = ram_erase, .sync = ram_sync,
    .read_size = 16, .prog_size = 256, .block_size = BLOCK, .block_count = BLOCKS,
    .cache_size = CFGFS_CACHE_SIZE, .lookahead_size = sizeof(lookahead), .block_cycles = 500,
    .read_buffer = read_buf, .prog_buffer = prog_buf, .lookahead_buffer = lookahead,
};

static bool present = true;

const struct lfs_config *cfgfs_port_config(void) { return present ? &cfg : NULL; }
void cfgfs_port_lock(void) {}
void cfgfs_port_unlock(void) {}
bool cfgfs_port_write_begin(void) { return true; }
void cfgfs_port_write_end(void) {}

void cfgfs_ram_fill(uint8_t byte) { memset(flash, byte, sizeof(flash)); }
void cfgfs_ram_cut_after(int writes) { writes_left = writes; }
void cfgfs_ram_present(bool yes) { present = yes; }
