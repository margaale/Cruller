// Clean reboots: the CYW43 must be powered off first, or it may not come back up properly.

#pragma once

// Delay between scheduling a reboot and the reset; platform_prepare_reboot() fits well inside it.
#define PLATFORM_REBOOT_DELAY_MS 400

// Powers the CYW43 off (WL_REG_ON low). Schedule the reboot first (see platform_reboot()), and
// stop feeding the watchdog (health_stop_feeding()), which would otherwise postpone it.
void platform_prepare_reboot(void);

// Normal reboot into the current image.
void platform_reboot(void) __attribute__((noreturn));
