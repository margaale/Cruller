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
int net_rssi(void);              // the station link's signal in dBm (0 when not connected)
const char *net_hostname(void);  // "cruller" or "cruller-<name>" (mDNS .local and DHCP)

// The setup wizard (portal only): tries a network with the portal still up. False if not in the
// portal or a try is already running. Progress in net_setup_json(); net_setup_version() changes with it.
typedef enum {
    SETUP_IDLE,
    SETUP_JOINING,
    SETUP_OK,             // joined and saved; Cruller restarts on it shortly
    SETUP_WRONG_PASSWORD,
    SETUP_NOT_FOUND,
    SETUP_FAILED,
} net_setup_state_t;
bool net_setup_start(const char *ssid, const char *pass);
size_t net_setup_json(char *out, size_t size); // {"state","ssid","rssi","ip","hostname","restart_in_s"}
uint32_t net_setup_version(void);

// The setup access point ("Cruller_Setup", 192.168.4.1, open) is up: the real portal (NET_PORTAL,
// station off) or a test one next to the station link (net_portal_test).
bool net_portal_active(void);

// Opens the setup access point next to the station link for `minutes` (0 closes it), to try the
// portal from a phone without losing the network.
void net_portal_test(uint32_t minutes);

// Nearby networks as JSON: [{"ssid":"...","rssi":-50,"secure":true},...], strongest first. Scans now
// when the station interface is on (~3 s); in the real portal, the list taken just before it opened.
size_t net_scan_json(char *out, size_t size);
