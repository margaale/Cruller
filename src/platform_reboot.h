// Clean reboots: the CYW43 must be powered off first, or it may not come back up properly.

#pragma once

// Powers the CYW43 off (WL_REG_ON low); call right before any software reboot.
void platform_prepare_reboot(void);

// Normal reboot into the current image.
void platform_reboot(void) __attribute__((noreturn));
