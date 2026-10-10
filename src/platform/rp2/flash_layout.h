// Flash layout constants. Offsets are physical flash offsets; they must match src/platform/rp2/pt.json.

#pragma once

#include <stdint.h>
#include "hardware/regs/addressmap.h"

#define FLASH_SECTOR_SIZE_B   4096u
#define FLASH_PAGE_SIZE_B     256u

// Cruller data partition (pt.json "Cruller data").
#define DATA_PART_OFFSET      0x3A2000u
#define DATA_PART_SIZE        0x5B000u

// store.h's records (store_flash.c): two alternating sectors each from the start of the data
// partition, in the keys' order: the Wi-Fi credentials, the settings (name, paired SVS Bridge), the
// RT4K's firmware and model, each SVS input's profile, and whatever comes after.
#define STORE_SECTOR_OFFSET(key, copy) (DATA_PART_OFFSET + (2u * (uint32_t)(key) + (uint32_t)(copy)) * FLASH_SECTOR_SIZE_B)
#define STORE_SECTORS         16u // room for 8 records

// cfgfs.h's littlefs (cfgfs_flash.c): the rest of the data partition, after store.h's records.
#define CFGFS_OFFSET          (DATA_PART_OFFSET + STORE_SECTORS * FLASH_SECTOR_SIZE_B)
#define CFGFS_SIZE            (DATA_PART_SIZE - STORE_SECTORS * FLASH_SECTOR_SIZE_B)

// DonutShop (arduino-pico, flash=4194304_2097152) LittleFS, read once after migrating.
// The first Cruller OTA overwrites it (partition B and the data partition overlap it).
#define DS_FS_OFFSET          0x1FF000u
#define DS_FS_SIZE            0x200000u

// When running from a partition, 0x10000000 is translated to the partition start. Physical
// flash offsets are read through the untranslated XIP alias instead.
#define FLASH_RAW_PTR(off)    ((const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + (off)))
