// Freeze recorder: where each core was before a watchdog reset.
//
// Every 250 ms a hardware timer interrupt on each core saves the interrupted PC and LR, the task
// running there, and whether it was in an interrupt handler or had interrupts masked, into RAM kept
// across resets. After a watchdog reset the boot log shows the last samples of both cores. On the
// Pico 2 W the interrupt is an NMI, so a core spinning with its interrupts off is sampled too; its
// samples stop early only if it hangs on a bus access or locks up. The addresses resolve with
// arm-none-eabi-addr2line -e cruller.elf (the ELF of that very build).

#pragma once

#include <stddef.h>

// Log the record left by the previous run, if it ended in a watchdog reset, then start a new one.
// Call once, after log_init().
void freeze_report(void);

// Start sampling on both cores (spawns a short task pinned to each). Needs the scheduler.
void freeze_start(void);

// The current record as text (GET /debug/freeze), to check the sampling works.
size_t freeze_dump(char *out, size_t size);
