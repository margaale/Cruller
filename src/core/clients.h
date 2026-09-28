// One budget for long-lived clients, shared by web pages (WebSocket) and RFC 2217 connections: any
// mix up to CLIENTS_MAX. When it's full, a newcomer replaces one of its own kind (the quietest page,
// the oldest RFC 2217 client); if its kind has none, it's turned away rather than taking a live
// client of the other kind. Dead clients don't hold slots for long: pages are pinged, RFC 2217 uses
// TCP keepalive. lwIP's pools are sized for this plus HTTP (see lwipopts.h).

#pragma once

#include <stdbool.h>

#define CLIENTS_MAX 8

bool clients_take(void); // a slot, if one is free
void clients_give(void); // a client left
int clients_used(void);
