#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "boot/uf2.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "FreeRTOS.h"
#include "task.h"

#include "flash_layout.h"
#include "flash_ops.h"
#include "platform_reboot.h"
#include "health.h"

static struct {
    uint8_t block[512];      // UF2 block being assembled
    size_t fill;
    bool started;
    bool failed;
    bool quiet;              // flash_quiet_begin() held (from the first byte fed)
    const char *error;
    uint32_t num_blocks;
    uint32_t blocks_done;
    uint32_t part_start;     // physical offset of the target partition
    uint32_t part_size;
    int32_t sector;          // sector (relative to the partition) being assembled in sector_buf, -1 if none
    uint32_t ticks_restored; // flash_ticks_restored_ms() at the start
} ota;

// Pages are collected per sector and written with one erase and one program. Every flash_safe_execute()
// creates (and deletes) a lockout task on the other core; one per 256-byte page was ~2000 of them per
// image, faster than the idle task freed them, and the heap ran out mid-update.
static uint8_t __attribute__((aligned(4))) sector_buf[FLASH_SECTOR_SIZE_B];

static uint8_t __attribute__((aligned(4))) rom_workarea[4 * 1024];

// The boot ROM's functions check, on the way out, a canary taken on the way in with the calling core's
// own RCP salt. A task that FreeRTOS moves to the other core in between fails that check, and the RCP
// halts the chip until the watchdog resets it: the freezes right after an SVS input change or the RT4K
// powering off, when the status pushed to the page asked the ROM for the boot partition while USB
// bursts had the rt4k task (pinned to core 1) pushing the web tasks over to core 0. So a ROM call made
// while the scheduler runs holds its task on one core, and the boot info, which never changes, is read
// before the scheduler starts.
static UBaseType_t rom_pin(void) {
    const UBaseType_t affinity = vTaskCoreAffinityGet(NULL);
    vTaskCoreAffinitySet(NULL, 1u << get_core_num()); // moved meanwhile: it goes back to that core first
    return affinity;
}

static void rom_unpin(UBaseType_t affinity) {
    vTaskCoreAffinitySet(NULL, affinity);
}

static struct {
    int partition; // -1: unknown
    int type;      // BOOT_TYPE_*, or an error (< 0)
} boot;

// Before main(), like the SDK's unique board ID: no scheduler, no task to move.
static void __attribute__((constructor)) read_boot_info(void) {
    boot_info_t info = {0};
    boot.partition = rom_get_boot_info(&info) ? info.partition : -1;
    boot.type = rom_get_last_boot_type();
}

static bool fail(const char *why) {
    ota.failed = true;
    ota.error = why;
    printf("ota: %s\n", why);
    return false;
}

void ota_begin(void) {
    ota_abort();
    memset(&ota, 0, sizeof(ota));
    ota.sector = -1;
    ota.ticks_restored = flash_ticks_restored_ms();
}

static bool flush_sector(void) {
    if (ota.sector < 0) return true;
    const uint32_t off = ota.part_start + (uint32_t)ota.sector * FLASH_SECTOR_SIZE_B;
    if (!flash_erase_safe(off, FLASH_SECTOR_SIZE_B)) return fail("flash erase failed");
    if (!flash_program_safe(off, sector_buf, FLASH_SECTOR_SIZE_B)) return fail("flash program failed");
    if (memcmp(FLASH_RAW_PTR(off), sector_buf, FLASH_SECTOR_SIZE_B) != 0) return fail("flash verify failed");
    if ((ota.blocks_done & 0x7f) == 0) {
        printf("ota: %lu/%lu blocks, heap free %u (min %u)\n",(unsigned long)ota.blocks_done, (unsigned long)ota.num_blocks,
            (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());
    }
    ota.sector = -1;
    vTaskDelay(1); // let the idle task free the lockout tasks
    return true;
}

static bool start(const struct uf2_block *b) {
    if (!(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) || b->file_size != RP2350_ARM_S_FAMILY_ID) {
        return fail("not an RP2350 Arm image (UF2 family)");
    }
    resident_partition_t part;
    const UBaseType_t affinity = rom_pin();
    rom_flash_flush_cache();
    const int rc = rom_get_uf2_target_partition(rom_workarea, sizeof(rom_workarea), RP2350_ARM_S_FAMILY_ID, &part);
    rom_unpin(affinity);
    if (rc < 0) return fail("no partition to update (is the partition table installed?)");
    const uint32_t first = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    const uint32_t last = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    ota.part_start = first * FLASH_SECTOR_SIZE_B;
    ota.part_size = (last + 1 - first) * FLASH_SECTOR_SIZE_B;
    ota.num_blocks = b->num_blocks;
    ota.started = true;
    printf("ota: writing %lu blocks to partition at 0x%06lx (%lu KB)\n",
        (unsigned long)ota.num_blocks, (unsigned long)ota.part_start, (unsigned long)(ota.part_size / 1024));
    return true;
}

static bool write_block(const struct uf2_block *b) {
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END) {
        return fail("bad UF2 block");
    }
    // SDK UF2s for the RP2350 start with an extra "absolute" block (RP2350-E10 workaround) that
    // targets the end of flash; it is not part of the application image.
    if ((b->flags & UF2_FLAG_FAMILY_ID_PRESENT) && b->file_size == ABSOLUTE_FAMILY_ID) return true;
    if (!ota.started && !start(b)) return false;
    if (b->block_no != ota.blocks_done || b->num_blocks != ota.num_blocks) return fail("UF2 blocks out of order");
    if (b->file_size != RP2350_ARM_S_FAMILY_ID) return fail("UF2 family changed mid-file");
    if (b->payload_size != FLASH_PAGE_SIZE_B || (b->target_addr & (FLASH_PAGE_SIZE_B - 1))) return fail("unexpected UF2 payload layout");
    if (b->target_addr < XIP_BASE || b->target_addr + FLASH_PAGE_SIZE_B > XIP_BASE + ota.part_size) {
        return fail("image does not fit the partition");
    }
    const uint32_t rel = b->target_addr - XIP_BASE;
    const int32_t sector = (int32_t)(rel / FLASH_SECTOR_SIZE_B);
    // UF2 files from the SDK are ordered by address: a sector is complete once a later one starts.
    if (sector != ota.sector) {
        if (sector < ota.sector) return fail("UF2 addresses not ascending");
        if (!flush_sector()) return false;
        memset(sector_buf, 0xff, sizeof(sector_buf));
        ota.sector = sector;
    }
    memcpy(sector_buf + rel % FLASH_SECTOR_SIZE_B, b->data, FLASH_PAGE_SIZE_B);
    ota.blocks_done++;
    return true;
}

bool ota_feed(const uint8_t *data, size_t len) {
    if (ota.failed) return false;
    if (len && !ota.quiet) {
        if (!flash_quiet_begin()) return fail("could not stop the USB host");
        ota.quiet = true;
    }
    while (len) {
        size_t take = sizeof(ota.block) - ota.fill;
        if (take > len) take = len;
        memcpy(ota.block + ota.fill, data, take);
        ota.fill += take;
        data += take;
        len -= take;
        if (ota.fill == sizeof(ota.block)) {
            ota.fill = 0;
            if (ota.started && ota.blocks_done >= ota.num_blocks) return fail("data after the last UF2 block");
            if (!write_block((const struct uf2_block *)ota.block)) return false;
        }
    }
    return true;
}

bool ota_finish(void) {
    if (ota.failed) return false;
    if (!ota.started || ota.fill != 0 || ota.blocks_done != ota.num_blocks) return fail("incomplete UF2 image");
    if (!flush_sector()) return false;
    printf("ota: image complete, %lu ms of ticks made up after the flash writes\n",
        (unsigned long)(flash_ticks_restored_ms() - ota.ticks_restored));
    return true;
}

void ota_abort(void) {
    if (ota.quiet) flash_quiet_end();
    ota.quiet = false;
}

const char *ota_error(void) {
    return ota.error ? ota.error : "ok";
}

void ota_reboot_into_update(void) {
    const uint32_t update_base = XIP_BASE + ota.part_start;
    // Schedule the reboot first (the boot ROM runs it on the watchdog), then tidy up: if the tidying
    // blocks, the board reboots anyway.
    health_stop_feeding();
    rom_pin(); // for good: the reboot is coming
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, PLATFORM_REBOOT_DELAY_MS, update_base, 0);
    platform_prepare_reboot();
    for (;;) tight_loop_contents();
}

bool ota_is_trial_boot(void) {
    return boot.type == BOOT_TYPE_FLASH_UPDATE;
}

void ota_confirm_if_trial(void) {
    if (boot.type != BOOT_TYPE_FLASH_UPDATE) return;
    const UBaseType_t affinity = rom_pin();
    const int ret = rom_explicit_buy(rom_workarea, sizeof(rom_workarea));
    rom_unpin(affinity);
    printf("ota: update confirmed (explicit buy %d)\n", ret);
}

int ota_boot_partition(void) {
    return boot.partition;
}

const char *ota_last_boot_type(void) {
    switch (boot.type) {
        case BOOT_TYPE_NORMAL: return "normal";
        case BOOT_TYPE_BOOTSEL: return "bootsel";
        case BOOT_TYPE_RAM_IMAGE: return "ram image";
        case BOOT_TYPE_FLASH_UPDATE: return "flash update";
        case BOOT_TYPE_PC_SP: return "pc/sp";
        default: return "other";
    }
}
