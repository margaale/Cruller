// Wi-Fi: station mode with stored credentials, falling back to a setup access point.

#pragma once

#include <stdbool.h>

typedef enum {
    NET_STARTING,
    NET_JOINING,
    NET_CONNECTED,
    NET_PORTAL,
} net_state_t;

void net_start(void);            // starts the network task
net_state_t net_state(void);
const char *net_ip(void);        // dotted address, or "" when not up
const char *net_ssid(void);
