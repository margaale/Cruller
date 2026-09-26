// Freeze recorder: where each core was before a watchdog reset.
//
// Every 250 ms a hardware timer interrupt on each core saves the interrupted PC and LR and the task
// running there into RAM kept across resets. After a watchdog reset the boot log shows the last
// samples of both cores. A core whose samples stop early had its interrupts disabled (spinning in
// a critical section, say); the addresses resolve with arm-none-eabi-addr2line -e cruller.elf.

#pragma once

#include <stddef.h>

// Log the record left by the previous run, if it ended in a watchdog reset, then start a new one.
// Call once, after log_init().
void freeze_report(void);

// Start sampling on both cores (spawns a short task pinned to each). Needs the scheduler.
void freeze_start(void);

// The current record as text (GET /debug/freeze), to check the sampling works.
size_t freeze_dump(char *out, size_t size);
