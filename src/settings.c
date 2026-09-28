#include "settings.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flash_layout.h"
#include "flash_ops.h"

#define SETTINGS_MAGIC 0x54535243u // "CRST"

typedef struct {
    uint32_t magic;
    uint32_t seq;
    settings_t s;
    uint32_t crc;
} settings_record_t;

_Static_assert(sizeof(settings_record_t) <= FLASH_PAGE_SIZE_B, "settings record must fit one page");

static settings_t cached;
static bool loaded;

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}

static const settings_record_t *record_at(uint32_t offset) {
    return (const settings_record_t *)FLASH_RAW_PTR(offset);
}

static bool record_valid(const settings_record_t *r) {
    return r->magic == SETTINGS_MAGIC && r->crc == crc32(r, offsetof(settings_record_t, crc)) &&
        memchr(r->s.name, 0, sizeof(r->s.name)) && memchr(r->s.svs_bridge, 0, sizeof(r->s.svs_bridge));
}

// The newer valid copy, or NULL.
static const settings_record_t *newest(void) {
    const settings_record_t *a = record_at(SETTINGS_SECTOR0_OFFSET), *b = record_at(SETTINGS_SECTOR1_OFFSET);
    const bool va = record_valid(a), vb = record_valid(b);
    if (va && vb) return (int32_t)(a->seq - b->seq) > 0 ? a : b;
    return va ? a : vb ? b : NULL;
}

void settings_get(settings_t *out) {
    if (!loaded) {
        const settings_record_t *r = newest();
        if (r) cached = r->s;
        else memset(&cached, 0, sizeof(cached));
        loaded = true;
    }
    *out = cached;
}

bool settings_save(const settings_t *s) {
    const settings_record_t *cur = newest();
    // Overwrite the older (or invalid) copy, so a power cut never loses both.
    const uint32_t target = cur == record_at(SETTINGS_SECTOR0_OFFSET) ? SETTINGS_SECTOR1_OFFSET : SETTINGS_SECTOR0_OFFSET;
    static uint8_t page[FLASH_PAGE_SIZE_B]; // RAM source for programming
    memset(page, 0xff, sizeof(page));
    settings_record_t *r = (settings_record_t *)page;
    memset(r, 0, sizeof(*r));
    r->magic = SETTINGS_MAGIC;
    r->seq = cur ? cur->seq + 1 : 1;
    r->s = *s;
    r->s.name[SETTINGS_NAME_MAX] = 0;
    r->s.svs_bridge[SETTINGS_NAME_MAX] = 0;
    r->crc = crc32(r, offsetof(settings_record_t, crc));
    const bool ok = flash_erase_safe(target, FLASH_SECTOR_SIZE_B) && flash_program_safe(target, page, sizeof(page)) &&
        record_valid(record_at(target));
    if (ok) {
        cached = r->s;
        loaded = true;
    }
    return ok;
}

bool settings_clear(void) {
    const settings_t none = {0};
    return settings_save(&none);
}

void settings_hostname(const char *name, char *out, size_t size) {
    size_t n = (size_t)snprintf(out, size, "cruller");
    bool hyphen = true; // one "-" before the name, none doubled, none trailing
    for (const char *p = name; *p && n + 2 < size; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            if (hyphen) out[n++] = '-';
            out[n++] = (char)tolower(c);
            hyphen = false;
        } else {
            hyphen = true;
        }
    }
    out[n < size ? n : size - 1] = 0;
}

bool settings_name_ok(const char *name) {
    const size_t len = strlen(name);
    if (!len || len > SETTINGS_NAME_MAX) return false;
    bool any = false;
    for (const char *p = name; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c)) any = true;
        else if (c != ' ' && c != '-' && c != '_') return false;
    }
    return any;
}
