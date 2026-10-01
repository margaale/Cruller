// store.h in the data partition's flash: two alternating sectors per record (flash_layout.h).
//
// A copy is its magic, a sequence number, the data, then a CRC-32 of all that (padding to 4 bytes
// included, as zeros), in as many pages as it takes. The newer valid copy wins; a save overwrites the
// other one, or nothing when the newer already holds the same. That's the format creds.c and
// settings.c wrote before store.h, so boards keep what they saved.

#include "store.h"

#include <stdint.h>
#include <string.h>

#include "flash_layout.h"
#include "flash_ops.h"

typedef struct {
    uint32_t magic;
    uint32_t seq;
} header_t;

static const uint32_t magics[STORE_KEYS] = {
    [STORE_CREDS] = 0x46575243u,        // "CRWF"
    [STORE_SETTINGS] = 0x54535243u,     // "CRST"
    [STORE_RT4K] = 0x4b525243u,         // "CRRK"
    [STORE_SVS_PROFILES] = 0x50535243u, // "CRSP"
};

_Static_assert(sizeof(header_t) + STORE_RECORD_MAX + 4 <= FLASH_SECTOR_SIZE_B, "a record must fit one sector");
_Static_assert(2u * STORE_KEYS * FLASH_SECTOR_SIZE_B <= DATA_PART_SIZE, "the records must fit the data partition");

static uint32_t crc32_add(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return crc;
}

static uint32_t crc32(const void *data, size_t len) {
    return ~crc32_add(0xffffffffu, data, len);
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
    if (h.magic != magics[key] || crc != crc32(p, crc_offset(size))) return false;
    *seq = h.seq;
    return true;
}

// The newer valid copy (0 or 1) and its sequence number, or -1 when neither is valid.
static int newest(store_key_t key, size_t size, uint32_t *seq) {
    uint32_t sa = 0, sb = 0;
    const bool va = valid(key, STORE_SECTOR_OFFSET(key, 0), size, &sa);
    const bool vb = valid(key, STORE_SECTOR_OFFSET(key, 1), size, &sb);
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
    memcpy(out, FLASH_RAW_PTR(STORE_SECTOR_OFFSET(key, i)) + sizeof(header_t), size);
    return true;
}

// Byte at of a copy: the header, the data, zeros up to the CRC, the CRC.
static uint8_t copy_byte(const header_t *h, const uint8_t *data, size_t size, uint32_t crc, size_t at) {
    if (at < sizeof(*h)) return ((const uint8_t *)h)[at];
    at -= sizeof(*h);
    if (at < size) return data[at];
    const size_t pad = crc_offset(size) - sizeof(*h) - size;
    return at < size + pad ? 0 : ((const uint8_t *)&crc)[at - size - pad];
}

bool store_save(store_key_t key, const void *data, size_t size) {
    if (size > STORE_RECORD_MAX) return false;
    uint32_t seq = 0;
    const int i = newest(key, size, &seq);
    if (i >= 0 && !memcmp(FLASH_RAW_PTR(STORE_SECTOR_OFFSET(key, i)) + sizeof(header_t), data, size)) return true;
    // Overwrite the older (or invalid) copy, so a power cut never loses both.
    const uint32_t target = STORE_SECTOR_OFFSET(key, i == 0 ? 1 : 0);
    const header_t h = {magics[key], i < 0 ? 1 : seq + 1};
    static const uint8_t zeros[4];
    uint32_t crc = crc32_add(0xffffffffu, &h, sizeof(h));
    crc = crc32_add(crc, data, size);
    crc = ~crc32_add(crc, zeros, crc_offset(size) - sizeof(h) - size);
    // Programmed a page at a time, from RAM; the USB host held off once for the lot.
    static uint8_t pages[STORE_KEYS][FLASH_PAGE_SIZE_B]; // one per record
    uint8_t *page = pages[key];
    const size_t total = crc_offset(size) + sizeof(crc);
    if (!flash_quiet_begin()) return false;
    bool ok = flash_erase_safe(target, FLASH_SECTOR_SIZE_B);
    for (size_t at = 0; ok && at < total; at += FLASH_PAGE_SIZE_B) {
        memset(page, 0xff, FLASH_PAGE_SIZE_B);
        for (size_t k = 0; k < FLASH_PAGE_SIZE_B && at + k < total; k++) page[k] = copy_byte(&h, data, size, crc, at + k);
        ok = flash_program_safe(target + at, page, FLASH_PAGE_SIZE_B);
    }
    flash_quiet_end();
    uint32_t check;
    return ok && valid(key, target, size, &check);
}
