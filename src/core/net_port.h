// What net.c (net.h, the same on every board) needs from each board's radio: rp2's CYW43, the ESP32's
// esp_wifi. All of it from the net task, but net_port_scan() (from the HTTP task too).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "creds.h"
#include "net.h"

// Once, first: the radio on and the station interface up (named net_hostname() for DHCP).
void net_port_start(void);

// Wi-Fi credentials an earlier firmware left on this board (the DonutShop's on the Pico 2 W), for one
// never set up. False when there are none.
bool net_port_import_creds(wifi_creds_t *out);

// Joins a network with the station (an access point that is up stays up): SETUP_OK once it has an
// address (net_port_ip()); SETUP_WRONG_PASSWORD, SETUP_NOT_FOUND or SETUP_FAILED, within timeout_ms,
// and the station leaves it.
net_setup_state_t net_port_join(const char *ssid, const char *pass, uint32_t timeout_ms);
void net_port_ip(char out[16]); // the station's address, dotted

// The station link: true while it's up, with *rssi its signal when known; false with *why, the
// board's reason code (for the log).
bool net_port_link(int *rssi, int *why);

// After each join: mDNS (net_hostname().local) and the DNS-SD services (docs/SVS.md) started, or
// the new address announced.
void net_port_mdns(const char *host, const char *instance);

// Scans with the station interface on, blocking (~3 s): each network heard goes to net_scan_add().
void net_port_scan(void);
void net_scan_add(const char *ssid, int rssi, bool secure, uint8_t channel); // (net.c's)

// The station off, for the real portal.
void net_port_sta_off(void);

// The setup access point (192.168.4.1, open) with DHCP and a catch-all DNS: the HTTP server sends
// every unknown address there to the setup page (captive portal). On `channel`, or (0) next to the
// station link, on its channel.
void net_port_portal_open(const char *ssid, uint8_t channel);
void net_port_portal_close(void);
bool net_port_portal_busy(void); // something (a phone) is on it
