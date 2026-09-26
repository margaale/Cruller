// WebSocket endpoint (/ws): live terminal and the RT4K's OSD, mirrored.
//
// Server -> client, binary messages, first byte = type:
//   0x01 terminal text (raw bytes from the RT4K)
//   0x02 OSD plane: [plane 1|2][ready line length][ready line][2048 chars + 2048 colours, or nothing
//        when the plane is empty]
//   0x03 font: 4096 bytes (256 glyphs, 8x16, font[row * 256 + glyph], bit 0 = leftmost pixel)
// Client -> server, text messages: one RT4K console command each ("remote menu", "ver", ...).

#pragma once

#include <stdbool.h>

void ws_start(void);

// Takes over an HTTP connection after the 101 handshake (evicting the quietest client when full).
// False (the caller closes it) only if handovers are backing up.
bool ws_adopt(int fd);

// Can a handover be queued?
bool ws_has_room(void);

// Debug: one line on where the ws and mirror tasks are.
#include <stddef.h>
void ws_debug(char *out, size_t size);
