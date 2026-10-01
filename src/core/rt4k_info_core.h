// What the RT4K last said about itself, without an RTOS: its firmware version (from "ver"), its
// model (from "model"), and the profile it has loaded (from "prof get"). rt4k_info.c drives it; the
// host unit tests (tests/test_rt4k_info.c) drive it directly.
//
// Any reply counts, whoever asked: Cruller's own power probes, the page, Home Assistant. When the RT4K
// comes on, whatever it hasn't said since is asked for. The firmware and model are kept across
// restarts (rt4k_info.c), so the page knows them while the RT4K sleeps; "fresh" tells the two apart.
// The loaded profile is only known while it's on: the RT4K doesn't say when it loads one (from its
// menu, its remote, Auto Load SVS), so it's asked every RT4K_INFO_PROFILE_MS without hearing it, and
// soon after what may have changed it. Those replies prove it's on too, so the power probe's "ver"
// isn't needed meanwhile (power_core.h): one question every 10 s either way.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RT4K_INFO_VERSION_MAX 23
#define RT4K_INFO_MODEL_MAX   39

typedef struct {
    char version[RT4K_INFO_VERSION_MAX + 1]; // "1.89.0" ("": never seen)
    char model[RT4K_INFO_MODEL_MAX + 1];     // the "model" reply after its first word ("": never seen)
} rt4k_info_t;

#define RT4K_INFO_RETRY_MS 5000 // an unanswered question is asked again after this long
#define RT4K_INFO_TRIES    3    // times each question is asked per power-on

#define RT4K_INFO_PROFILE_MAX 191   // the loaded profile's path under /profile
#define RT4K_INFO_PROFILE_MS  10000 // asked again after this long without hearing it
#define RT4K_INFO_SOON_MS     3000  // ... and this long after what may have changed it

// saved: what was kept from before (NULL: nothing).
void rt4k_info_core_init(const rt4k_info_t *saved);

// Every text line from the RT4K.
void rt4k_info_core_line(const char *line);

// Often (the power task's tick): on is whether the RT4K answers. Returns the command to send now
// ("ver", "model" or "prof get"), or NULL.
const char *rt4k_info_core_poll(bool on, uint32_t now_ms);

// Something that may have changed the loaded profile (the SVS switched inputs): ask soon.
void rt4k_info_core_profile_soon(void);

// The loaded profile's path under /profile ("": none) into out. False when it isn't known (the RT4K
// isn't on, or hasn't said since it came on).
bool rt4k_info_core_profile(char *out, size_t size);

// What's known. True when all of it was said since the RT4K last came on.
bool rt4k_info_core_get(rt4k_info_t *out);

// True once after what's known changed from what was kept, when nothing is left to ask (so both
// answers make one write): *out is what to keep now. The same values again never count.
bool rt4k_info_core_take_changed(rt4k_info_t *out);

// Changes whenever what rt4k_info_core_get() or rt4k_info_core_profile() gives changes (a value, or
// its freshness).
uint32_t rt4k_info_core_seq(void);
