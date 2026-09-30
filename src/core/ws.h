// WebSocket endpoint (/ws): live terminal and the RT4K's OSD, mirrored.
//
// Server -> client, binary messages, first byte = type:
//   0x01 terminal text (raw bytes from the RT4K)
//   0x02 OSD plane: [plane 1|2][ready line length][ready line][2048 chars + 2048 colours, or nothing
//        when the plane is empty]
//   0x03 font: 4096 bytes (256 glyphs, 8x16, font[row * 256 + glyph], bit 0 = leftmost pixel)
//   0x04 Cruller log text; 0x05 the /status JSON (on connect, then every 5 s; every 0.5 s while an
//        upload to the RT4K runs, with its progress in "put")
//   0x06 Debug report: [5][JSON], every 2 s to pages showing their Debug tab:
//        {"status": the /status JSON, "serial": FT232R counters and modem lines, "mirror": OSD polls and
//        key -> screen times, "console": the last commands, "memory": clients, lwIP pools, heaps}
//        [6]["owner\tline\n"...]: every line from the RT4K as it comes, with the console owner it was
//        routed to (console.h: 0 page, 1 power, 2 query, 3 HTTP, 4 files, 5+ RFC 2217; -1 nobody's)
// Client -> server, text messages: one RT4K console command each ("remote menu", "ver", ...).
// Client -> server, binary: [0x10, 1|0] the page shows the live screen / doesn't (background tab, or
// another view); the mirror polls the RT4K only while some page shows it (clients count as showing it
// until they say otherwise). [0x11, 1|0] the page shows its Debug tab on screen / doesn't.

#pragma once

#include <stdbool.h>
#include <stdint.h>

void ws_start(void);

// /api/v1/events (docs/API.md), for Home Assistant and scripts: the same socket, but a client that
// gets JSON text messages with a "type": first "hello" (the API's version, the types this Cruller
// has, the ones it gets), then only the types it asked for (?types=, WS_EVENT_* in ws_proto.h; "state"
// by default). "state" is the /api/v1/state JSON: at once, as soon as the RT4K is plugged in or out
// or its power changes, and every minute. What it sends is ignored (reserved for messages a later v1
// announces in "hello"), and it doesn't count as a page showing the screen.
#define WS_EVENTS_MAX 2 // at most this many at once (a new one replaces the quietest)

// Takes over an HTTP connection after the 101 handshake, as a page or an events client with its event
// types (evicting the quietest client of its kind when full). False (the caller closes it) only if
// handovers are backing up.
bool ws_adopt(int fd, bool events, uint32_t types);

// Can a handover of this kind be queued?
bool ws_has_room(bool events);

// A remote key has gone out to the RT4K (console.c, from any sender): the mirror polls the menu soon
// and times key -> screen (in ws_debug).
void ws_key_sent(void);

// Connected clients (open pages), and the most allowed in *max.
int ws_clients(int *max);

// Connected /api/v1/events clients.
int ws_event_clients(void);

// Debug: zero the key -> screen and poll error counters (for a measurement run).
void ws_debug_reset(void);

// Debug: one line on where the ws and mirror tasks are.
#include <stddef.h>
void ws_debug(char *out, size_t size);
