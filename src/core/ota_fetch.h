// Firmware updates Cruller downloads itself from its GitHub releases: the page picks the release and
// hands over the asset's URL, SHA-256 and size (POST /update/fetch), since GitHub doesn't let pages
// read release assets. The image goes into the inactive partition as it arrives (ota.h), on its own
// task; Cruller restarts into it only once all of it came and its SHA-256 matches.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Starts a download. False, with the reason, if it can't start: one running already, a URL outside
// this project's release assets, a bad SHA-256 or size, no memory.
bool ota_fetch_start(const char *url, const char *sha256_hex, uint32_t size, const char **why);
bool ota_fetch_running(void);

// The download for the status JSON, as an object: {"from":"github","got","size"} while it runs
// ("done":true once written, right before the restart), {"from":"github","failed":"<why>"} after one
// failed, until the next. False when none has run since the start.
bool ota_fetch_json(char *out, size_t size);

// Changes whenever the download's state does, so the status goes out at once.
uint32_t ota_fetch_version(void);
