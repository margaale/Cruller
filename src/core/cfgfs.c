#include "cfgfs.h"

#include <stdio.h>
#include <string.h>

#include "cfgfs_port.h"

static lfs_t fs;
static bool mounted;

// One file read and one written at a time (both at once while a file is rewritten from the old one).
static lfs_file_t rfile, wfile;
static uint8_t rbuf[CFGFS_CACHE_SIZE], wbuf[CFGFS_CACHE_SIZE];
static const struct lfs_file_config rcfg = {.buffer = rbuf}, wcfg = {.buffer = wbuf};
static bool writing;
static char wpath[LFS_NAME_MAX + 1], wtemp[LFS_NAME_MAX + 5];

bool cfgfs_mount(void) {
    const struct lfs_config *cfg = cfgfs_port_config();
    if (!cfg) return false;
    cfgfs_port_lock();
    int err = lfs_mount(&fs, cfg);
    if (err) {
        printf("cfgfs: no filesystem (%d): formatting %lu KB\n", err, (unsigned long)(cfg->block_count * cfg->block_size / 1024));
        if (cfgfs_port_write_begin()) {
            err = lfs_format(&fs, cfg);
            cfgfs_port_write_end();
        }
        if (!err) err = lfs_mount(&fs, cfg);
    }
    mounted = !err;
    if (err) printf("cfgfs: could not mount (%d)\n", err);
    cfgfs_port_unlock();
    return mounted;
}

bool cfgfs_format(void) {
    const struct lfs_config *cfg = cfgfs_port_config();
    if (!cfg) return false;
    cfgfs_port_lock();
    if (mounted) lfs_unmount(&fs);
    int err = -1;
    if (cfgfs_port_write_begin()) {
        err = lfs_format(&fs, cfg);
        cfgfs_port_write_end();
    }
    if (!err) err = lfs_mount(&fs, cfg);
    mounted = !err;
    cfgfs_port_unlock();
    return mounted;
}

void cfgfs_hold(void) {
    cfgfs_port_lock();
}

void cfgfs_release(void) {
    cfgfs_port_unlock();
}

long cfgfs_read(const char *path, void *buf, size_t size) {
    long n = -1;
    cfgfs_port_lock();
    if (mounted && lfs_file_opencfg(&fs, &rfile, path, LFS_O_RDONLY, &rcfg) == 0) {
        const lfs_soff_t len = lfs_file_size(&fs, &rfile);
        if (len >= 0 && (size_t)len <= size && lfs_file_read(&fs, &rfile, buf, (lfs_size_t)len) == len) n = (long)len;
        lfs_file_close(&fs, &rfile);
    }
    cfgfs_port_unlock();
    return n;
}

bool cfgfs_create(const char *path) {
    cfgfs_port_lock();
    if (!mounted || writing || strlen(path) > LFS_NAME_MAX || !cfgfs_port_write_begin()) {
        cfgfs_port_unlock();
        return false;
    }
    snprintf(wpath, sizeof(wpath), "%s", path);
    snprintf(wtemp, sizeof(wtemp), "%s.new", path);
    if (lfs_file_opencfg(&fs, &wfile, wtemp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &wcfg) != 0) {
        cfgfs_port_write_end();
        cfgfs_port_unlock();
        return false;
    }
    writing = true;
    return true; // (the lock and the write held till cfgfs_commit or cfgfs_abort)
}

bool cfgfs_put(const void *data, size_t len) {
    return writing && lfs_file_write(&fs, &wfile, data, (lfs_size_t)len) == (lfs_ssize_t)len;
}

static void finish(void) {
    writing = false;
    cfgfs_port_write_end();
    cfgfs_port_unlock();
}

bool cfgfs_commit(void) {
    if (!writing) return false;
    const bool ok = lfs_file_close(&fs, &wfile) == 0 && lfs_rename(&fs, wtemp, wpath) == 0;
    if (!ok) lfs_remove(&fs, wtemp);
    finish();
    return ok;
}

void cfgfs_abort(void) {
    if (!writing) return;
    lfs_file_close(&fs, &wfile);
    lfs_remove(&fs, wtemp);
    finish();
}

bool cfgfs_write(const char *path, const void *data, size_t len) {
    if (!cfgfs_create(path)) return false;
    if (!cfgfs_put(data, len)) {
        cfgfs_abort();
        return false;
    }
    return cfgfs_commit();
}

bool cfgfs_remove(const char *path) {
    cfgfs_port_lock();
    bool ok = false;
    if (mounted && cfgfs_port_write_begin()) {
        const int err = lfs_remove(&fs, path);
        ok = err == 0 || err == LFS_ERR_NOENT;
        cfgfs_port_write_end();
    }
    cfgfs_port_unlock();
    return ok;
}

bool cfgfs_lines(const char *path, char *line, size_t size, bool (*fn)(const char *line, void *ctx), void *ctx) {
    cfgfs_port_lock();
    if (!mounted || !size || lfs_file_opencfg(&fs, &rfile, path, LFS_O_RDONLY, &rcfg) != 0) {
        cfgfs_port_unlock();
        return false;
    }
    size_t n = 0;
    bool skip = false, more = true;
    char chunk[64];
    lfs_ssize_t got;
    while (more && (got = lfs_file_read(&fs, &rfile, chunk, sizeof(chunk))) > 0) {
        for (lfs_ssize_t k = 0; more && k < got; k++) {
            const char c = chunk[k];
            if (c == '\n') {
                if (!skip) {
                    line[n] = 0;
                    more = fn(line, ctx);
                }
                n = 0;
                skip = false;
            } else if (!skip) {
                if (n + 1 < size) line[n++] = c;
                else skip = true; // too long: dropped whole
            }
        }
    }
    if (more && n && !skip) { // a last line without its '\n'
        line[n] = 0;
        fn(line, ctx);
    }
    lfs_file_close(&fs, &rfile);
    cfgfs_port_unlock();
    return true;
}
