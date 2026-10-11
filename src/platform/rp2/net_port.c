// net_port.h on the Pico 2 W: the CYW43 (pico_cyw43_arch), its access point with picow_ap's DHCP
// server and catch-all DNS, lwIP's mDNS responder.

#include "net_port.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"
#include "lwip/tcpip.h"

#include "dhcpserver.h"
#include "donutshop.h"
#include "dnsserver.h"
#include "http.h"
#include "platform.h"
#include "settings.h"
#include "version.h"

#define STA_NETIF (&cyw43_state.netif[CYW43_ITF_STA])

// The station's DHCP host name (the router's client list) follows the mDNS one.
static void sta_hostname(void) {
    cyw43_arch_lwip_begin();
    netif_set_hostname(STA_NETIF, net_hostname());
    cyw43_arch_lwip_end();
}

void net_port_start(void) {
    cyw43_arch_enable_sta_mode();
    sta_hostname();
}

bool net_port_import_creds(wifi_creds_t *out) {
    return donutshop_import_creds(out);
}

net_setup_state_t net_port_join(const char *ssid, const char *pass, uint32_t timeout_ms) {
    cyw43_arch_enable_sta_mode(); // (the real portal turned it off)
    sta_hostname();
    const uint32_t auth = pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    net_setup_state_t result = SETUP_FAILED;
    if (cyw43_arch_wifi_connect_async(ssid, pass[0] ? pass : NULL, auth) == 0) {
        for (const uint32_t t0 = plat_ms(); plat_ms() - t0 < timeout_ms;) {
            vTaskDelay(pdMS_TO_TICKS(250));
            const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link == CYW43_LINK_UP) { result = SETUP_OK; break; }
            if (link == CYW43_LINK_BADAUTH) { result = SETUP_WRONG_PASSWORD; break; }
            if (link == CYW43_LINK_NONET) { result = SETUP_NOT_FOUND; break; }
            if (link == CYW43_LINK_FAIL) break;
        }
    }
    if (result != SETUP_OK) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    return result;
}

void net_port_ip(char out[16]) {
    cyw43_arch_lwip_begin();
    ip4addr_ntoa_r(netif_ip4_addr(STA_NETIF), out, 16);
    cyw43_arch_lwip_end();
}

bool net_port_link(int *rssi, int *why) {
    const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (link != CYW43_LINK_UP) {
        *why = link;
        return false;
    }
    int32_t r;
    if (!cyw43_wifi_get_rssi(&cyw43_state, &r)) *rssi = (int)r;
    return true;
}

// --- mDNS and DNS-SD: the web page, "_rt4k._tcp" for other boards and Home Assistant to find the RT4K
// bridge (TXT: id, ver, api: the API's version, name), "_rfc2217._tcp" for serial over the network
// (docs/SVS.md) ---------------------------------------------------------------------------------------

// One TXT item, key=value.
static void txt_add(struct mdns_service *service, const char *key, const char *value) {
    char item[8 + SETTINGS_NAME_MAX]; // (the longest: a name)
    const int n = snprintf(item, sizeof(item), "%s=%s", key, value);
    if (n > 0) mdns_resp_add_service_txtitem(service, item, (u8_t)(n < (int)sizeof(item) ? n : (int)sizeof(item) - 1));
}

static void txt_id(struct mdns_service *service) {
    char id[33];
    plat_board_id(id, sizeof(id));
    txt_add(service, "id", id);
}

static void rt4k_txt(struct mdns_service *service, void *userdata) {
    (void)userdata;
    txt_id(service);
    txt_add(service, "ver", cruller_version);
    txt_add(service, "api", HTTP_API_VERSION);
    settings_t s;
    settings_get(&s);
    if (s.name[0]) txt_add(service, "name", s.name);
}

static void rfc2217_txt(struct mdns_service *service, void *userdata) {
    (void)userdata;
    txt_id(service);
}

void net_port_mdns(const char *host, const char *instance) {
    static bool started;
    LOCK_TCPIP_CORE();
    if (started) {
        mdns_resp_netif_settings_changed(STA_NETIF);
    } else {
        mdns_resp_init();
        mdns_resp_add_netif(STA_NETIF, host);
        mdns_resp_add_service(STA_NETIF, instance, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
        mdns_resp_add_service(STA_NETIF, instance, "_rt4k", DNSSD_PROTO_TCP, 80, rt4k_txt, NULL);
        mdns_resp_add_service(STA_NETIF, instance, "_rfc2217", DNSSD_PROTO_TCP, 2217, rfc2217_txt, NULL);
        started = true;
    }
    UNLOCK_TCPIP_CORE();
}

// --- scan -------------------------------------------------------------------------------------------

// CYW43 async context: one result at a time, several per network (one per access point).
static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (r && r->ssid_len && r->ssid_len <= 32) {
        char ssid[33];
        memcpy(ssid, r->ssid, r->ssid_len);
        ssid[r->ssid_len] = 0;
        net_scan_add(ssid, r->rssi, r->auth_mode != 0, (uint8_t)r->channel);
    }
    return 0;
}

void net_port_scan(void) {
    cyw43_wifi_scan_options_t opts = {0};
    cyw43_arch_lwip_begin();
    const int rc = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb);
    cyw43_arch_lwip_end();
    if (rc) {
        printf("net: scan failed (%d)\n", rc);
        return;
    }
    for (int i = 0; i < 100; i++) { // ~3 s, at most 10 s
        vTaskDelay(pdMS_TO_TICKS(100));
        cyw43_arch_lwip_begin();
        const bool active = cyw43_wifi_scan_active(&cyw43_state);
        cyw43_arch_lwip_end();
        if (!active) break;
    }
}

void net_port_sta_off(void) {
    cyw43_arch_disable_sta_mode();
}

// --- setup access point -------------------------------------------------------------------------------

static dhcp_server_t dhcp;
static dns_server_t dns;

// The channel the station link is on (0 if unknown). The CYW43 has one radio: an access point next
// to the station link must use the same channel, or the station stops passing traffic (seen: the
// driver's default AP channel 3 cut the link, and the gateway watchdog reset the board).
static uint32_t station_channel(void) {
    uint8_t buf[12] = {0}; // channel_info_t: hw_channel, target_channel, scan_channel
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(buf), buf, CYW43_ITF_STA)) return 0;
    const uint32_t ch = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    return ch >= 1 && ch <= 14 ? ch : 0;
}

// The CYW43 driver gives the AP netif 192.168.4.1/24.
void net_port_portal_open(const char *ssid, uint8_t channel) {
    const bool sta_up = net_state() == NET_CONNECTED;
    if (!channel && sta_up) {
        channel = (uint8_t)station_channel();
        printf("net: station on channel %u, access point on the same\n", channel);
    }
    if (channel) cyw43_wifi_ap_set_channel(&cyw43_state, channel);
    cyw43_arch_enable_ap_mode(ssid, NULL, CYW43_AUTH_OPEN);
    if (sta_up) {
        // The driver makes each interface it brings up lwIP's default route: the AP took over, and
        // everything beyond the local subnet (the gateway pings, clients on other subnets) went out
        // through it. The station link stays the default.
        cyw43_arch_lwip_begin();
        netif_set_default(STA_NETIF);
        cyw43_arch_lwip_end();
    }
    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
    cyw43_arch_lwip_begin();
    dhcp_server_init(&dhcp, ap, &gw, &mask);
    dns_server_init(&dns, ap, &gw);
    cyw43_arch_lwip_end();
}

void net_port_portal_close(void) {
    cyw43_arch_lwip_begin();
    dns_server_deinit(&dns);
    dhcp_server_deinit(&dhcp);
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();
}

bool net_port_portal_busy(void) {
    int n = 8;
    uint8_t macs[8 * 6];
    cyw43_wifi_ap_get_stas(&cyw43_state, &n, macs);
    return n > 0;
}
