// DonutShop's Wi-Fi network, read from its LittleFS on the first boot after migrating (docs/DESIGN.md).

#include "donutshop.h"

#include <stdio.h>
#include <string.h>

#include "flash_layout.h"
#include "lfs.h"

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

bool donutshop_import_creds(wifi_creds_t *out) {
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
