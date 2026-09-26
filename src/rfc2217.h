// RFC 2217 server on TCP port 2217: the RT4K's serial console for network clients, e.g.
// pyserial's "rfc2217://cruller.local:2217" (Home Assistant's hass-RT4K). One client at a time;
// a new connection replaces the old one. Text only, shared with the web terminal: the client sees
// what the terminal sees, and its lines go to the RT4K between Cruller's own transfers.

#pragma once

#include <stdbool.h>
#include <stddef.h>

void rfc2217_start(void);

// The connected client's address ("" when none), for /status.
void rfc2217_client(char *out, size_t size);
