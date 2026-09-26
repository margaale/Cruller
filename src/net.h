// Wi-Fi: station mode with stored credentials, falling back to a setup access point.

#pragma once

#include <stdbool.h>

typedef enum {
    NET_STARTING,
    NET_JOINING,
    NET_CONNECTED,
    NET_PORTAL,
} net_state_t;

#include <stddef.h>
#include <stdint.h>

void net_start(void);            // starts the network task
net_state_t net_state(void);
const char *net_ip(void);        // dotted address, or "" when not up
const char *net_ssid(void);

// The setup access point ("Cruller_Setup", 192.168.4.1, open) is up: the real portal (NET_PORTAL,
// station off) or a test one next to the station link (net_portal_test).
bool net_portal_active(void);

// Opens the setup access point next to the station link for `minutes` (0 closes it), to try the
// portal from a phone without losing the network.
void net_portal_test(uint32_t minutes);

// Nearby networks as JSON: [{"ssid":"...","rssi":-50,"secure":true},...], strongest first. Scans now
// when the station interface is on (~3 s); in the real portal, the list taken just before it opened.
size_t net_scan_json(char *out, size_t size);
