// platform.h for the Raspberry Pi Pico 2 W.

#include "platform.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "platform_reboot.h"

uint32_t plat_ms(void) { return to_ms_since_boot(get_absolute_time()); }
uint32_t plat_us(void) { return time_us_32(); }

void plat_lock_init(plat_lock_t *lock) { critical_section_init(lock); }
void plat_lock_enter(plat_lock_t *lock) { critical_section_enter_blocking(lock); }
void plat_lock_exit(plat_lock_t *lock) { critical_section_exit(lock); }

bool plat_sha256_start(plat_sha256_t *s) { return pico_sha256_start_blocking(s, SHA256_BIG_ENDIAN, false) == PICO_OK; }
void plat_sha256_update(plat_sha256_t *s, const void *data, size_t len) { pico_sha256_update_blocking(s, data, len); }
void plat_sha256_finish(plat_sha256_t *s, uint8_t digest[32]) {
    sha256_result_t r;
    pico_sha256_finish(s, &r);
    memcpy(digest, r.bytes, 32);
}

void plat_board_id(char *out, size_t size) {
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    snprintf(out, size, "%s", id);
}

void plat_reboot(void) { platform_reboot(); }

extern char __data_start__, __data_end__, __bss_start__, __bss_end__, __StackTop;

void plat_memory(plat_memory_t *out) {
    HeapStats_t hs;
    vPortGetHeapStats(&hs);
    out->heap_size = configTOTAL_HEAP_SIZE;
    out->heap_free = (uint32_t)hs.xAvailableHeapSpaceInBytes;
    out->heap_lowest = (uint32_t)hs.xMinimumEverFreeBytesRemaining;
    out->heap_largest = (uint32_t)hs.xSizeOfLargestFreeBlockInBytes;
    out->ram_data = (uint32_t)(&__data_end__ - &__data_start__);
    out->ram_bss = (uint32_t)(&__bss_end__ - &__bss_start__);
    out->ram_total = out->ram_data + out->ram_bss + (uint32_t)(&__StackTop - &__bss_end__);
}
