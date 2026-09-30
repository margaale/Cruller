// WebSocket (RFC 6455) pieces with no sockets: handshake key, server frame headers, client frame
// parsing. ws.c does the I/O; tests/test_ws.c checks these on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

// Standard base64 with padding; out needs 4 * ceil(len / 3) + 1 bytes. Returns the length.
size_t ws_base64(const uint8_t *in, size_t len, char *out);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + RFC GUID)), 28 chars + NUL.
void ws_accept_key(const char *key, char out[29]);

// Header of an unmasked, final server frame. Returns its length (2, 4 or 10 bytes).
size_t ws_frame_header(uint8_t *out, uint8_t opcode, uint64_t payload_len);

typedef struct {
    uint8_t opcode;
    uint8_t *payload;   // points into the caller's buffer, unmasked in place
    size_t len;
} ws_frame_t;

// /api/v1/events' event types (docs/API.md, ws.h), as bits of a mask. A client gets only the types it
// names in ?types=, so a type added later never reaches a client that didn't ask for it.
#define WS_EVENT_STATE 0x1u  // "state": the /api/v1/state JSON
#define WS_EVENTS_ALL  WS_EVENT_STATE

// The types named in a comma-separated list ("state,later"; spaces around names allowed), names this
// Cruller doesn't have left out.
uint32_t ws_event_types(const char *list);

// The names of the types in a mask, as a JSON array (["state"]; "[]" for none). Returns its length, or
// 0 (and "") if it doesn't fit.
size_t ws_event_names(uint32_t types, char *out, size_t size);

// Parses one client frame at the start of buf. Returns the bytes it takes (> 0) and fills *f, 0 if
// the frame isn't complete yet, or -1 if the stream is unusable (unmasked, fragmented, reserved
// bits, or larger than max_payload): close the connection then.
long ws_parse(uint8_t *buf, size_t len, size_t max_payload, ws_frame_t *f);
