#include "net.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"
#include "lwip/tcpip.h"

#include "creds.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "platform.h"
#include "settings.h"
#include "status_led.h"

#define NET_TASK_STACK      2048
#define NET_TASK_PRIORITY   (tskIDLE_PRIORITY + 3)
#define JOIN_TIMEOUT_MS     30000
#define JOIN_ATTEMPTS       3
#define LINK_CHECK_MS       2000
#define RECONNECT_AFTER_MS  20000   // link down this long -> rejoin
#define PORTAL_SSID         "Cruller_Setup"
#define SETUP_JOIN_MS       20000   // the setup wizard's join attempt
#define SETUP_CLOSE_MS      20000   // after it worked: the portal stays up this long, then Cruller restarts

static volatile net_state_t state = NET_STARTING;
static char ip_str[16];
static wifi_creds_t creds;
// Names from the settings (settings.c): "cruller-living" (mDNS, DHCP) and "Cruller Living" (DNS-SD).
static char host_name[48], instance_name[48];

net_state_t net_state(void) { return state; }
const char *net_ip(void) { return ip_str; }
const char *net_ssid(void) { return state == NET_PORTAL ? PORTAL_SSID : creds.ssid; }
const char *net_hostname(void) { return host_name; }

static void names_init(void) {
    settings_t s;
    settings_get(&s);
    settings_hostname(s.name, host_name, sizeof(host_name));
    snprintf(instance_name, sizeof(instance_name), "Cruller%s%s", s.name[0] ? " " : "", s.name);
}

// The station's DHCP host name (the router's client list) follows the mDNS one.
static void sta_hostname(void) {
    cyw43_arch_lwip_begin();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], host_name);
    cyw43_arch_lwip_end();
}

static void set_ip(const ip4_addr_t *addr) {
    if (addr) ip4addr_ntoa_r(addr, ip_str, sizeof(ip_str));
    else ip_str[0] = 0;
}

// --- scan -------------------------------------------------------------------------------------------

#define SCAN_MAX 16
typedef struct {
    char ssid[33];
    int16_t rssi;
    bool secure;
    uint8_t channel; // of its strongest access point
} scan_entry_t;
static scan_entry_t scan_list[SCAN_MAX];
static int scan_count;

// CYW43 async context: one result at a time, several per network (one per access point).
static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r || !r->ssid_len || r->ssid_len > 32) return 0; // hidden networks have no name to pick
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = 0;
    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_list[i].ssid, ssid)) continue;
        if (r->rssi > scan_list[i].rssi) { // the strongest access point
            scan_list[i].rssi = r->rssi;
            scan_list[i].channel = (uint8_t)r->channel;
        }
        return 0;
    }
    if (scan_count < SCAN_MAX) {
        scan_entry_t *e = &scan_list[scan_count++];
        memcpy(e->ssid, ssid, sizeof(e->ssid));
        e->rssi = r->rssi;
        e->secure = r->auth_mode != 0;
        e->channel = (uint8_t)r->channel;
    }
    return 0;
}

// Needs the station interface up. Blocks until the scan is over (~3 s, at most 10 s).
static bool scan_now(void) {
    cyw43_wifi_scan_options_t opts = {0};
    cyw43_arch_lwip_begin();
    scan_count = 0;
    const int rc = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb);
    cyw43_arch_lwip_end();
    if (rc) {
        printf("net: scan failed (%d)\n", rc);
        return false;
    }
    for (int i = 0; i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        cyw43_arch_lwip_begin();
        const bool active = cyw43_wifi_scan_active(&cyw43_state);
        cyw43_arch_lwip_end();
        if (!active) break;
    }
    return true;
}

size_t net_scan_json(char *out, size_t size) {
    if (state != NET_PORTAL) scan_now(); // in the real portal the station is off: the list from before
    scan_entry_t list[SCAN_MAX];
    cyw43_arch_lwip_begin();
    const int n = scan_count;
    memcpy(list, scan_list, sizeof(list));
    cyw43_arch_lwip_end();
    for (int i = 1; i < n; i++) { // strongest first
        for (int j = i; j > 0 && list[j].rssi > list[j - 1].rssi; j--) {
            const scan_entry_t t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    }
    size_t o = (size_t)snprintf(out, size, "[");
    for (int i = 0; i < n && o < size; i++) {
        char esc[70];
        size_t k = 0;
        for (const char *c = list[i].ssid; *c && k + 7 < sizeof(esc); c++) {
            if (*c == '"' || *c == '\\') { esc[k++] = '\\'; esc[k++] = *c; }
            else if ((unsigned char)*c < 0x20) k += (size_t)snprintf(esc + k, sizeof(esc) - k, "\\u%04x", *c);
            else esc[k++] = *c;
        }
        esc[k] = 0;
        o += (size_t)snprintf(out + o, size - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", i ? "," : "", esc,
            list[i].rssi, list[i].secure ? "true" : "false");
    }
    if (o < size) o += (size_t)snprintf(out + o, size - o, "]");
    return o < size ? o : size - 1;
}

// DNS-SD services (docs/SVS.md): the web page, "_rt4k._tcp" for other boards and Home Assistant to
// find the RT4K bridge (TXT: id, ver, api), and "_rfc2217._tcp" for serial over the network.
static void txt_add(struct mdns_service *service, const char *item) {
    mdns_resp_add_service_txtitem(service, item, (u8_t)strlen(item));
}

static void txt_id(struct mdns_service *service) {
    char id[33], item[8 + sizeof(id)];
    plat_board_id(id, sizeof(id));
    snprintf(item, sizeof(item), "id=%s", id);
    txt_add(service, item);
}

static void rt4k_txt(struct mdns_service *service, void *userdata) {
    (void)userdata;
    txt_id(service);
    txt_add(service, "ver=" CRULLER_VERSION);
    txt_add(service, "api=/api");
    settings_t s;
    settings_get(&s);
    if (s.name[0]) {
        char item[8 + SETTINGS_NAME_MAX];
        snprintf(item, sizeof(item), "name=%s", s.name);
        txt_add(service, item);
    }
}

static void rfc2217_txt(struct mdns_service *service, void *userdata) {
    (void)userdata;
    txt_id(service);
}

static void mdns_start(struct netif *nif) {
    static bool started = false;
    LOCK_TCPIP_CORE();
    if (!started) {
        mdns_resp_init();
        started = true;
    }
    mdns_resp_add_netif(nif, host_name);
    mdns_resp_add_service(nif, instance_name, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
    mdns_resp_add_service(nif, instance_name, "_rt4k", DNSSD_PROTO_TCP, 80, rt4k_txt, NULL);
    mdns_resp_add_service(nif, instance_name, "_rfc2217", DNSSD_PROTO_TCP, 2217, rfc2217_txt, NULL);
    UNLOCK_TCPIP_CORE();
}

static bool join(void) {
    state = NET_JOINING;
    status_led_set(LED_JOINING);
    const uint32_t auth = creds.pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    for (int attempt = 1; attempt <= JOIN_ATTEMPTS; attempt++) {
        printf("net: joining \"%s\" (attempt %d)\n", creds.ssid, attempt);
        const int rc = cyw43_arch_wifi_connect_timeout_ms(creds.ssid, creds.pass[0] ? creds.pass : NULL, auth, JOIN_TIMEOUT_MS);
        if (rc == 0) {
            struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
            set_ip(netif_ip4_addr(nif));
            printf("net: connected, IP %s\n", ip_str);
            state = NET_CONNECTED;
            status_led_set(LED_CONNECTED);
            return true;
        }
        printf("net: join failed (%d)\n", rc);
    }
    return false;
}

// --- setup access point -------------------------------------------------------------------------------

static dhcp_server_t dhcp;
static dns_server_t dns;
static volatile bool portal_up;
static volatile uint32_t portal_test_until_ms; // a test portal closes then (0: none)
static volatile int32_t portal_test_request = -1; // minutes asked for by net_portal_test (-1: none)

bool net_portal_active(void) { return portal_up; }

void net_portal_test(uint32_t minutes) { portal_test_request = (int32_t)minutes; }

static uint32_t now_ms(void) { return plat_ms(); }

// The signal, read by the net task with its link check: other tasks (the status JSON, pushed every
// 0.5 s during an upload) only read the copy. Asking the CYW43 from the ws task during an upload to the
// RT4K coincided with the board freezing.
static volatile int rssi_now;

int net_rssi(void) {
    return state == NET_CONNECTED ? rssi_now : 0;
}

// The channel the station link is on (0 if unknown). The CYW43 has one radio: an access point next
// to the station link must use the same channel, or the station stops passing traffic (seen: the
// driver's default AP channel 3 cut the link, and the gateway watchdog reset the board).
static uint32_t station_channel(void) {
    uint8_t buf[12] = {0}; // channel_info_t: hw_channel, target_channel, scan_channel
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(buf), buf, CYW43_ITF_STA)) return 0;
    const uint32_t ch = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    return ch >= 1 && ch <= 14 ? ch : 0;
}

// The CYW43 driver gives the AP netif 192.168.4.1/24; DHCP and a catch-all DNS are served on it, and
// the HTTP server sends every unknown address there to the setup page (captive portal).
static void portal_open(void) {
    if (portal_up) return;
    if (state == NET_CONNECTED) {
        const uint32_t ch = station_channel();
        if (ch) cyw43_wifi_ap_set_channel(&cyw43_state, ch);
        printf("net: station on channel %lu, access point on the same\n", (unsigned long)ch);
    }
    cyw43_arch_enable_ap_mode(PORTAL_SSID, NULL, CYW43_AUTH_OPEN);
    if (state == NET_CONNECTED) {
        // The driver makes each interface it brings up lwIP's default route: the AP took over, and
        // everything beyond the local subnet (the gateway pings, clients on other subnets) went out
        // through it. The station link stays the default.
        cyw43_arch_lwip_begin();
        netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
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
    portal_up = true;
    printf("net: setup portal \"%s\" open\n", PORTAL_SSID);
}

static void portal_close(void) {
    if (!portal_up) return;
    cyw43_arch_lwip_begin();
    dns_server_deinit(&dns);
    dhcp_server_deinit(&dhcp);
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();
    portal_up = false;
    printf("net: setup portal closed\n");
}

// --- the setup wizard's join (from the portal, which stays up meanwhile) ------------------------------
//
// The wizard asks with net_setup_start(); the net task tries the network with the access point still
// up, so the phone on it hears how it went (wrong password, not found) and can try again. On success
// the credentials are saved and, SETUP_CLOSE_MS later, Cruller restarts on its new network.

static struct {
    volatile net_setup_state_t st;
    volatile bool requested;
    char ssid[CREDS_SSID_MAX + 1], pass[CREDS_PASS_MAX + 1];
    char ip[16];
    int rssi;                // the network's signal from the portal's scan (0: not in it)
    uint32_t restart_at_ms;  // after a success
    volatile uint32_t version;
} setup;

uint32_t net_setup_version(void) { return setup.version; }

bool net_setup_start(const char *ssid, const char *pass) {
    if (state != NET_PORTAL || setup.st == SETUP_JOINING || setup.st == SETUP_OK || !ssid[0]) return false;
    snprintf(setup.ssid, sizeof(setup.ssid), "%s", ssid);
    snprintf(setup.pass, sizeof(setup.pass), "%s", pass ? pass : "");
    setup.ip[0] = 0;
    setup.st = SETUP_JOINING;
    setup.requested = true;
    setup.version++;
    return true;
}

size_t net_setup_json(char *out, size_t size) {
    static const char *names[] = {"idle", "joining", "ok", "wrong_password", "not_found", "failed"};
    const net_setup_state_t st = setup.st;
    char ssid[2 * CREDS_SSID_MAX + 2];
    size_t e = 0;
    for (const char *p = setup.ssid; *p && e + 2 < sizeof(ssid); p++) { // JSON-escape the name
        if (*p == '"' || *p == '\\') ssid[e++] = '\\';
        ssid[e++] = (unsigned char)*p < 0x20 ? '?' : *p;
    }
    ssid[e] = 0;
    const uint32_t now = now_ms();
    const long left = st == SETUP_OK ? (long)(setup.restart_at_ms - now) / 1000 : 0;
    const int n = snprintf(out, size, "{\"state\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"hostname\":\"%s\",\"restart_in_s\":%ld}",
        names[st], ssid, setup.rssi, setup.ip, host_name, left > 0 ? left : 0);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

static void setup_join(void) {
    setup.rssi = 0;
    for (int i = 0; i < scan_count; i++) {
        if (!strcmp(scan_list[i].ssid, setup.ssid)) setup.rssi = scan_list[i].rssi;
    }
    printf("net: setup: joining \"%s\" with the portal up\n", setup.ssid);
    cyw43_arch_enable_sta_mode();
    sta_hostname();
    const uint32_t auth = setup.pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    net_setup_state_t result = SETUP_FAILED;
    if (cyw43_arch_wifi_connect_async(setup.ssid, setup.pass[0] ? setup.pass : NULL, auth) == 0) {
        for (const uint32_t t0 = now_ms(); now_ms() - t0 < SETUP_JOIN_MS;) {
            vTaskDelay(pdMS_TO_TICKS(250));
            const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link == CYW43_LINK_UP) { result = SETUP_OK; break; }
            if (link == CYW43_LINK_BADAUTH) { result = SETUP_WRONG_PASSWORD; break; }
            if (link == CYW43_LINK_NONET) { result = SETUP_NOT_FOUND; break; }
            if (link == CYW43_LINK_FAIL) break;
        }
    }
    if (result == SETUP_OK) {
        cyw43_arch_lwip_begin();
        ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), setup.ip, sizeof(setup.ip));
        cyw43_arch_lwip_end();
        wifi_creds_t c = {0};
        snprintf(c.ssid, sizeof(c.ssid), "%s", setup.ssid);
        snprintf(c.pass, sizeof(c.pass), "%s", setup.pass);
        if (!creds_save(&c)) result = SETUP_FAILED;
        setup.restart_at_ms = now_ms() + SETUP_CLOSE_MS;
    } else {
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    }
    printf("net: setup: \"%s\" %s%s%s\n", setup.ssid, result == SETUP_OK ? "joined, IP " : "failed",
        result == SETUP_OK ? setup.ip : "", result == SETUP_OK ? "; restarting in 20 s" : "");
    setup.st = result;
    setup.version++;
}

static void portal_forever(void) {
    printf("net: starting setup portal \"%s\"\n", PORTAL_SSID);
    scan_now(); // the station interface is still on: networks to offer in the portal
    cyw43_arch_disable_sta_mode();
    // The access point on the strongest network's channel: most likely the one the wizard joins, so
    // the radio needn't hop channels then (which drops the phone off the setup network for a moment).
    int best = -1;
    for (int i = 0; i < scan_count; i++) {
        if (scan_list[i].channel >= 1 && scan_list[i].channel <= 13 && (best < 0 || scan_list[i].rssi > scan_list[best].rssi)) best = i;
    }
    if (best >= 0) {
        cyw43_wifi_ap_set_channel(&cyw43_state, scan_list[best].channel);
        printf("net: portal on channel %u, as \"%s\"\n", scan_list[best].channel, scan_list[best].ssid);
    }
    portal_open();
    ip4_addr_t gw;
    IP4_ADDR(&gw, 192, 168, 4, 1);
    set_ip(&gw);
    state = NET_PORTAL;
    status_led_set(LED_PORTAL);
    for (;;) { // the HTTP task serves the portal; the wizard's join runs here
        vTaskDelay(pdMS_TO_TICKS(200));
        if (setup.requested) {
            setup.requested = false;
            setup_join();
        }
        if (setup.st == SETUP_OK && (int32_t)(now_ms() - setup.restart_at_ms) >= 0) {
            printf("net: setup done, restarting on \"%s\"\n", setup.ssid);
            plat_reboot();
        }
    }
}

// A test portal next to the station link (net_portal_test), from the net task.
static void portal_test_step(void) {
    const int32_t req = portal_test_request;
    if (req >= 0) {
        portal_test_request = -1;
        if (req > 0) {
            portal_open();
            portal_test_until_ms = (now_ms() + (uint32_t)req * 60000u) | 1;
            printf("net: test portal for %ld min\n", (long)req);
        } else {
            portal_test_until_ms = 0;
            portal_close();
        }
    }
    if (portal_test_until_ms && (int32_t)(now_ms() - portal_test_until_ms) >= 0) {
        portal_test_until_ms = 0;
        portal_close();
    }
}

static void net_task(void *param) {
    (void)param;
    if (!creds_load(&creds) && !creds_present()) { // never set up (a factory reset leaves a record)
        wifi_creds_t imported;
        if (creds_import_donutshop(&imported)) {
            creds = imported;
            printf("net: imported Wi-Fi \"%s\" from DonutShop, saving: %s\n", creds.ssid, creds_save(&creds) ? "ok" : "FAILED");
        }
    }

    names_init();
    cyw43_arch_enable_sta_mode();
    sta_hostname();
    if (!creds.ssid[0] || !join()) portal_forever();
    mdns_start(&cyw43_state.netif[CYW43_ITF_STA]);

    // Keep the link up: rejoin after the link has been down for a while; the portal takes over
    // only when rejoining fails too.
    uint32_t down_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_MS));
        portal_test_step();
        const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            int32_t rssi = 0;
            if (!cyw43_wifi_get_rssi(&cyw43_state, &rssi)) rssi_now = (int)rssi;
            down_ms = 0;
            continue;
        }
        down_ms += LINK_CHECK_MS;
        if (down_ms >= RECONNECT_AFTER_MS) {
            printf("net: link lost (%d), rejoining\n", link);
            set_ip(NULL);
            if (!join()) portal_forever();
            LOCK_TCPIP_CORE();
            mdns_resp_netif_settings_changed(&cyw43_state.netif[CYW43_ITF_STA]);
            UNLOCK_TCPIP_CORE();
            down_ms = 0;
        }
    }
}

void net_start(void) {
    xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, NULL);
}
