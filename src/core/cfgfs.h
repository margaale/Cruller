// Files kept across restarts, for what doesn't fit store.h's small records (gameID's consoles and its
// games): a littlefs filesystem in the board's flash (rp2: the data partition after store.h's records;
// esp32: its "cruller" partition; cfgfs_port.h). A file is replaced whole: written beside it, then
// renamed over it, so a power cut leaves the old one or the new one, never half of either. One task at
// a time (a lock held through each call, and from cfgfs_create to cfgfs_commit or cfgfs_abort).

#pragma once

#include <stdbool.h>
#include <stddef.h>

// At start: mounts the filesystem, or formats it when there's none (a new board) or it's broken. False
// when there's no flash for it; every other call then fails.
bool cfgfs_mount(void);

// Empties it (a factory reset).
bool cfgfs_format(void);

// A file read whole into buf: its length, or -1 when there's none or it's bigger than size.
long cfgfs_read(const char *path, void *buf, size_t size);

// A file replaced whole (or made).
bool cfgfs_write(const char *path, const void *data, size_t len);

// True when it's gone, or wasn't there.
bool cfgfs_remove(const char *path);

// Each line of a file (its '\n' dropped, 0-terminated in line) to fn, until fn returns false or the
// file ends; a line longer than size - 1 is skipped. False when there's no such file.
bool cfgfs_lines(const char *path, char *line, size_t size, bool (*fn)(const char *line, void *ctx), void *ctx);

// Every other task held off across several calls (each taken again inside: the same task may), for a
// caller with state of its own to keep whole (a buffer the files go through).
void cfgfs_hold(void);
void cfgfs_release(void);

// A file written in pieces (one at a time): it replaces path on cfgfs_commit, cfgfs_abort drops it.
// The lock is held from cfgfs_create to either, so the same task may read the old one meanwhile
// (cfgfs_lines), and no other task sees half.
bool cfgfs_create(const char *path);
bool cfgfs_put(const void *data, size_t len);
bool cfgfs_commit(void);
void cfgfs_abort(void);
