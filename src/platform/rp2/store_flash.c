// store.h in the data partition's flash: two alternating sectors per record.
//
// A copy is its magic, a sequence number, the data, then a CRC-32 of all that (padding to 4 bytes
// included, as zeros). The newer valid copy wins; a save overwrites the other one. That's the format
// creds.c and settings.c wrote before store.h, so boards keep what they saved.

#include "store.h"

#include <stdint.h>
#include <string.h>

#include "flash_layout.h"
#include "flash_ops.h"

typedef struct {
    uint32_t magic;
    uint32_t seq;
} header_t;

static const struct {
    uint32_t magic;
    uint32_t sector[2];
} slots[STORE_KEYS] = {
    [STORE_CREDS] = {0x46575243u /* "CRWF" */, {CREDS_SECTOR0_OFFSET, CREDS_SECTOR1_OFFSET}},
    [STORE_SETTINGS] = {0x54535243u /* "CRST" */, {SETTINGS_SECTOR0_OFFSET, SETTINGS_SECTOR1_OFFSET}},
    [STORE_RT4K] = {0x4b525243u /* "CRRK" */, {RT4K_INFO_SECTOR0_OFFSET, RT4K_INFO_SECTOR1_OFFSET}},
};

_Static_assert(sizeof(header_t) + STORE_RECORD_MAX + 4 <= FLASH_PAGE_SIZE_B, "a record must fit one page");

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}

static size_t crc_offset(size_t size) {
    return (sizeof(header_t) + size + 3) & ~(size_t)3;
}

static bool valid(store_key_t key, uint32_t offset, size_t size, uint32_t *seq) {
    const uint8_t *p = FLASH_RAW_PTR(offset);
    header_t h;
    uint32_t crc;
    memcpy(&h, p, sizeof(h));
    memcpy(&crc, p + crc_offset(size), sizeof(crc));
    if (h.magic != slots[key].magic || crc != crc32(p, crc_offset(size))) return false;
    *seq = h.seq;
    return true;
}

// The newer valid copy (0 or 1) and its sequence number, or -1 when neither is valid.
static int newest(store_key_t key, size_t size, uint32_t *seq) {
    uint32_t sa = 0, sb = 0;
    const bool va = valid(key, slots[key].sector[0], size, &sa);
    const bool vb = valid(key, slots[key].sector[1], size, &sb);
    if (va && (!vb || (int32_t)(sa - sb) > 0)) {
        *seq = sa;
        return 0;
    }
    if (vb) {
        *seq = sb;
        return 1;
    }
    return -1;
}

bool store_load(store_key_t key, void *out, size_t size) {
    uint32_t seq;
    const int i = size <= STORE_RECORD_MAX ? newest(key, size, &seq) : -1;
    if (i < 0) return false;
    memcpy(out, FLASH_RAW_PTR(slots[key].sector[i]) + sizeof(header_t), size);
    return true;
}

bool store_save(store_key_t key, const void *data, size_t size) {
    if (size > STORE_RECORD_MAX) return false;
    uint32_t seq = 0;
    const int i = newest(key, size, &seq);
    // Overwrite the older (or invalid) copy, so a power cut never loses both.
    const uint32_t target = slots[key].sector[i == 0 ? 1 : 0];
    static uint8_t pages[STORE_KEYS][FLASH_PAGE_SIZE_B]; // RAM source for programming; one per record
    uint8_t *page = pages[key];
    memset(page, 0xff, FLASH_PAGE_SIZE_B);
    memset(page, 0, crc_offset(size));
    const header_t h = {slots[key].magic, i < 0 ? 1 : seq + 1};
    memcpy(page, &h, sizeof(h));
    memcpy(page + sizeof(h), data, size);
    const uint32_t crc = crc32(page, crc_offset(size));
    memcpy(page + crc_offset(size), &crc, sizeof(crc));
    uint32_t check;
    return flash_erase_safe(target, FLASH_SECTOR_SIZE_B) && flash_program_safe(target, page, FLASH_PAGE_SIZE_B) &&
        valid(key, target, size, &check);
}
