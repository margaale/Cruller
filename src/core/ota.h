// Over-the-air update into the inactive A/B partition, fed with a UF2 stream.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// An update may quiet other work while it writes (on the Pico 2 W, the RT4K's USB host) from the
// first byte fed until ota_abort() or the reboot into it.
void ota_begin(void);
// Feeds any number of bytes; returns false on the first error (see ota_error()).
bool ota_feed(const uint8_t *data, size_t len);
// True when every UF2 block was written.
bool ota_finish(void);
// Gives up on an update that failed or was cut off: whatever was quieted runs again.
void ota_abort(void);
const char *ota_error(void);

// Reboots into the partition just written ("flash update" boot, try before you buy).
void ota_reboot_into_update(void);

// Call once the firmware is known to be healthy after a flash update boot.
// True when this boot is an update on trial (TBYB), not confirmed yet.
bool ota_is_trial_boot(void);

void ota_confirm_if_trial(void);

// Boot information for status pages.
int ota_boot_partition(void);
const char *ota_last_boot_type(void);
