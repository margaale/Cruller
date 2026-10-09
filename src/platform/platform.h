// What Cruller's common code (src/core) needs from the board it runs on, beyond FreeRTOS and lwIP's
// sockets (both available on every target). Each target implements it in src/platform/<target>/:
// rp2 (Raspberry Pi Pico 2 W, Pico SDK) and esp32 (ESP32-S3, ESP-IDF).
//
// The larger pieces have their own interfaces in src/core, implemented per target too: rt4k.h (the
// serial link to the RT4K), net.h (Wi-Fi, setup portal, mDNS), ota.h (firmware updates), tls.h (HTTPS
// client, for the updates from GitHub), store.h (settings kept across restarts), health.h (watchdog),
// freeze.h (freeze recorder), log.h (the in-memory log), status_led.h.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform_target.h" // per target: plat_lock_t, plat_sha256_t, PLAT_STACK()

// Task stacks are sized in 32-bit words, as in the FreeRTOS kernel; PLAT_STACK(words) is what this
// target's xTaskCreate() takes (ESP-IDF counts bytes).

// The developer tools: the /debug routes that act on the RT4K or the board (raw bytes, baud, flow
// control, the test portal, a wedged network, a fault), the RTL1 failure capture (/debug/lastfail) and
// the USB trace (/debug/usbtrace). On unless the build says CRULLER_DEBUG=0 (16 KB of the Pico 2 W's
// RAM). The diagnostics that only read (/debug/tasks, memory, tcp, console, freeze) stay either way.
#ifndef CRULLER_DEBUG
#define CRULLER_DEBUG 1
#endif

// --- time --------------------------------------------------------------------------------------------

uint32_t plat_ms(void); // milliseconds since boot (wraps after 49 days: compare differences)
uint32_t plat_us(void); // microseconds since boot, low 32 bits

// --- short critical sections (a few instructions; both cores, and interrupts, locked out) ---------

void plat_lock_init(plat_lock_t *lock);
void plat_lock_enter(plat_lock_t *lock);
void plat_lock_exit(plat_lock_t *lock);

// One board-wide critical section of the same kind, for modules with nothing to set a lock up.
void plat_critical_enter(void);
void plat_critical_exit(void);

// --- SHA-256 (hardware where there is some) -------------------------------------------------------

bool plat_sha256_start(plat_sha256_t *s); // false if the hardware is busy
void plat_sha256_update(plat_sha256_t *s, const void *data, size_t len);
void plat_sha256_finish(plat_sha256_t *s, uint8_t digest[32]);

// --- the board ---------------------------------------------------------------------------------------

// A stable id for this board, as hex ("E1C7E49F5FBA5DD8"): the mDNS TXT id.
void plat_board_id(char *out, size_t size);

// A clean restart (the Wi-Fi chip is powered down first where that matters). Doesn't return.
void plat_reboot(void) __attribute__((noreturn));

// Memory for /debug/memory: the FreeRTOS heap, and how RAM is split (0 where not known).
typedef struct {
    uint32_t heap_size, heap_free, heap_lowest, heap_largest;
    uint32_t ram_total, ram_data, ram_bss;
} plat_memory_t;
void plat_memory(plat_memory_t *out);
