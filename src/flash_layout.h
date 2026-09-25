// Flash layout constants. Offsets are physical flash offsets; they must match pt.json.

#pragma once

#include <stdint.h>
#include "hardware/regs/addressmap.h"

#define FLASH_SECTOR_SIZE_B   4096u
#define FLASH_PAGE_SIZE_B     256u

// Cruller data partition (pt.json "Cruller data").
#define DATA_PART_OFFSET      0x3A2000u
#define DATA_PART_SIZE        0x5B000u

// Wi-Fi credentials: two alternating sectors at the start of the data partition.
#define CREDS_SECTOR0_OFFSET  (DATA_PART_OFFSET)
#define CREDS_SECTOR1_OFFSET  (DATA_PART_OFFSET + FLASH_SECTOR_SIZE_B)

// DonutShop (arduino-pico, flash=4194304_2097152) LittleFS, read once after migrating.
// The first Cruller OTA overwrites it (partition B and the data partition overlap it).
#define DS_FS_OFFSET          0x1FF000u
#define DS_FS_SIZE            0x200000u

// When running from a partition, 0x10000000 is translated to the partition start. Physical
// flash offsets are read through the untranslated XIP alias instead.
#define FLASH_RAW_PTR(off)    ((const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + (off)))
