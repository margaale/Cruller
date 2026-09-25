#include "creds.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "flash_layout.h"
#include "flash_ops.h"
#include "lfs.h"

#define CREDS_MAGIC 0x46575243u // "CRWF"

typedef struct {
    uint32_t magic;
    uint32_t seq;
    wifi_creds_t creds;
    uint32_t crc;
} creds_record_t;

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}

static bool record_valid(const creds_record_t *r) {
    return r->magic == CREDS_MAGIC &&
        r->crc == crc32(r, offsetof(creds_record_t, crc)) &&
        memchr(r->creds.ssid, 0, sizeof(r->creds.ssid)) &&
        memchr(r->creds.pass, 0, sizeof(r->creds.pass));
}

static const creds_record_t *record_at(uint32_t offset) {
    return (const creds_record_t *)FLASH_RAW_PTR(offset);
}

bool creds_load(wifi_creds_t *out) {
    const creds_record_t *a = record_at(CREDS_SECTOR0_OFFSET);
    const creds_record_t *b = record_at(CREDS_SECTOR1_OFFSET);
    const bool va = record_valid(a), vb = record_valid(b);
    const creds_record_t *best = NULL;
    if (va && vb) best = (int32_t)(a->seq - b->seq) > 0 ? a : b;
    else if (va) best = a;
    else if (vb) best = b;
    if (!best || !best->creds.ssid[0]) return false;
    *out = best->creds;
    return true;
}

bool creds_save(const wifi_creds_t *creds) {
    const creds_record_t *a = record_at(CREDS_SECTOR0_OFFSET);
    const creds_record_t *b = record_at(CREDS_SECTOR1_OFFSET);
    const bool va = record_valid(a), vb = record_valid(b);
    // Overwrite the older (or invalid) copy, so a power cut never loses both.
    uint32_t seq = 1, target = CREDS_SECTOR0_OFFSET;
    if (va && (!vb || (int32_t)(a->seq - b->seq) > 0)) {
        seq = a->seq + 1;
        target = CREDS_SECTOR1_OFFSET;
    } else if (vb) {
        seq = b->seq + 1;
        target = CREDS_SECTOR0_OFFSET;
    }

    static uint8_t page[FLASH_PAGE_SIZE_B]; // RAM source for programming
    _Static_assert(sizeof(creds_record_t) <= FLASH_PAGE_SIZE_B, "creds record must fit one page");
    memset(page, 0xff, sizeof(page));
    creds_record_t *r = (creds_record_t *)page;
    memset(r, 0, sizeof(*r));
    r->magic = CREDS_MAGIC;
    r->seq = seq;
    r->creds = *creds;
    r->crc = crc32(r, offsetof(creds_record_t, crc));

    return flash_erase_safe(target, FLASH_SECTOR_SIZE_B) &&
        flash_program_safe(target, page, sizeof(page)) &&
        record_valid(record_at(target));
}

// --- DonutShop import -----------------------------------------------------------------------
// arduino-pico LittleFS geometry: 4 KB blocks, 256-byte read/prog, 256-byte cache/lookahead.

static int ds_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
    memcpy(buffer, FLASH_RAW_PTR(DS_FS_OFFSET + block * c->block_size + off), size);
    return LFS_ERR_OK;
}

static int ds_readonly(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c; (void)block; (void)off; (void)buffer; (void)size;
    return LFS_ERR_IO; // never write the old filesystem
}

static int ds_noerase(const struct lfs_config *c, lfs_block_t block) {
    (void)c; (void)block;
    return LFS_ERR_IO;
}

static int ds_sync(const struct lfs_config *c) {
    (void)c;
    return LFS_ERR_OK;
}

// Extracts a JSON string value for "key" (ArduinoJson output: {"ssid":"...","pass":"..."}).
static bool json_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'u': return false; // non-ASCII SSIDs/passwords are not expected from the portal
                default: ch = e; break; // \" \\ \/
            }
        }
        if (n + 1 >= out_size) return false;
        out[n++] = ch;
    }
    if (*p != '"') return false;
    out[n] = 0;
    return true;
}

bool creds_import_donutshop(wifi_creds_t *out) {
    static uint8_t read_buf[256], prog_buf[256], lookahead_buf[256];
    const struct lfs_config cfg = {
        .read = ds_read,
        .prog = ds_readonly,
        .erase = ds_noerase,
        .sync = ds_sync,
        .read_size = 256,
        .prog_size = 256,
        .block_size = FLASH_SECTOR_SIZE_B,
        .block_count = DS_FS_SIZE / FLASH_SECTOR_SIZE_B,
        .block_cycles = 16,
        .cache_size = 256,
        .lookahead_size = 256,
        .read_buffer = read_buf,
        .prog_buffer = prog_buf,
        .lookahead_buffer = lookahead_buf,
    };
    static lfs_t lfs;
    if (lfs_mount(&lfs, &cfg) != LFS_ERR_OK) {
        printf("import: no DonutShop filesystem\n");
        return false;
    }
    bool ok = false;
    static lfs_file_t file;
    static uint8_t file_buf[256];
    const struct lfs_file_config fcfg = {.buffer = file_buf};
    if (lfs_file_opencfg(&lfs, &file, "wifi.json", LFS_O_RDONLY, &fcfg) == LFS_ERR_OK) {
        char json[256];
        lfs_ssize_t n = lfs_file_read(&lfs, &file, json, sizeof(json) - 1);
        lfs_file_close(&lfs, &file);
        if (n > 0) {
            json[n] = 0;
            memset(out, 0, sizeof(*out));
            ok = json_string(json, "ssid", out->ssid, sizeof(out->ssid)) && out->ssid[0] &&
                json_string(json, "pass", out->pass, sizeof(out->pass));
        }
    }
    lfs_unmount(&lfs);
    printf("import: DonutShop wifi.json %s\n", ok ? "found" : "not usable");
    return ok;
}
