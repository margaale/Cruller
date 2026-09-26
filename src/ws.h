// WebSocket endpoint (/ws): live terminal and the RT4K's OSD, mirrored.
//
// Server -> client, binary messages, first byte = type:
//   0x01 terminal text (raw bytes from the RT4K)
//   0x02 OSD plane: [plane 1|2][ready line length][ready line][2048 chars + 2048 colours, or nothing
//        when the plane is empty]
//   0x03 font: 4096 bytes (256 glyphs, 8x16, font[row * 256 + glyph], bit 0 = leftmost pixel)
//   0x04 Cruller log text; 0x05 the /status JSON (on connect, then every 5 s)
// Client -> server, text messages: one RT4K console command each ("remote menu", "ver", ...).
// Client -> server, binary: [0x10, 1|0] the page is visible / hidden; the mirror polls the RT4K
// only while some page is visible (clients count as visible until they say otherwise).

#pragma once

#include <stdbool.h>

void ws_start(void);

// Takes over an HTTP connection after the 101 handshake (evicting the quietest client when full).
// False (the caller closes it) only if handovers are backing up.
bool ws_adopt(int fd);

// Can a handover be queued?
bool ws_has_room(void);

// A remote key has gone out to the RT4K (console.c, from any sender): the mirror polls the menu soon
// and times key -> screen (in ws_debug).
void ws_key_sent(void);

// Connected clients (open pages), and the most allowed in *max.
int ws_clients(int *max);

// Debug: zero the key -> screen and poll error counters (for a measurement run).
void ws_debug_reset(void);

// Debug: one line on where the ws and mirror tasks are.
#include <stddef.h>
void ws_debug(char *out, size_t size);
