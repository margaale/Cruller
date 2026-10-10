// What cfgfs.c needs from a target: its littlefs geometry and block device (the read, prog, erase and sync
// callbacks over its flash, with the read, prog and lookahead buffers), and a lock. src/platform/rp2/
// cfgfs_flash.c, src/platform/esp32/main/cfgfs_part.c; tests/cfgfs_ram.c for the host tests.

#pragma once

#include <stdbool.h>

#include "lfs.h"

// Every target's read and prog caches (lfs_config.cache_size): a file's buffer is that big too.
#define CFGFS_CACHE_SIZE 256

// The filesystem's config, or NULL when the target has no flash for it.
const struct lfs_config *cfgfs_port_config(void);

// A lock the same task may take again (held across a whole write; cfgfs_lines inside it).
void cfgfs_port_lock(void);
void cfgfs_port_unlock(void);

// Around anything that writes (rp2: the USB host held off while flash is written, once for the lot).
bool cfgfs_port_write_begin(void);
void cfgfs_port_write_end(void);
