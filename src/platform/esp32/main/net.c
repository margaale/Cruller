// net.h on the ESP32-S3 (esp_wifi): the same behavior as rp2's net.c. Station mode with the stored
// network; the setup portal ("Cruller_Setup", 192.168.4.1, open, with a catch-all DNS) when there is
// none or joining fails, which tries the stored network again now and then; the setup wizard's join
// with the portal still up; mDNS and DNS-SD.

#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"
#include "mdns.h"

#include "creds.h"
#include "dnsserver.h"
#include "http.h"
#include "platform.h"
#include "settings.h"
#include "status_led.h"
#include "version.h"

#define NET_TASK_STACK      2048 // words
#define NET_TASK_PRIORITY   (tskIDLE_PRIORITY + 3)
#define JOIN_TIMEOUT_MS     30000
#define JOIN_ATTEMPTS       3
#define LINK_CHECK_MS       2000
#define RECONNECT_AFTER_MS  20000   // link down this long -> rejoin
#define PORTAL_SSID         "Cruller_Setup"
#define SETUP_JOIN_MS       20000   // the setup wizard's join attempt
#define SETUP_CLOSE_MS      20000   // after it worked: the portal stays up this long, then Cruller restarts
#define RETRY_MS            60000   // in the portal: the saved network tried again this often (a router
#define RETRY_BUSY_MS       600000  // slower to boot than Cruller), or this seldom with a phone on the portal

#define GOT_IP_BIT          BIT0
#define DISCONNECTED_BIT    BIT1

static volatile net_state_t state = NET_STARTING;
static char ip_str[16];
static wifi_creds_t creds;
static char host_name[48], instance_name[48];
static esp_netif_t *sta_if, *ap_if;
static EventGroupHandle_t events;
static volatile uint8_t disconnect_reason;
static volatile bool link_up;
static volatile int rssi_now;

net_state_t net_state(void) { return state; }
const char *net_ip(void) { return ip_str; }
const char *net_ssid(void) { return state == NET_PORTAL ? PORTAL_SSID : creds.ssid; }
const char *net_hostname(void) { return host_name; }
int net_rssi(void) { return state == NET_CONNECTED ? rssi_now : 0; }

static void names_init(void) {
    settings_t s;
    settings_get(&s);
    settings_hostname(s.name, host_name, sizeof(host_name));
    snprintf(instance_name, sizeof(instance_name), "Cruller%s%s", s.name[0] ? " " : "", s.name);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        disconnect_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        link_up = false;
        xEventGroupSetBits(events, DISCONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        link_up = true;
        xEventGroupSetBits(events, GOT_IP_BIT);
    }
}

// Joins a network with the station interface: SETUP_OK once it has an address.
static net_setup_state_t sta_join(const char *ssid, const char *pass, uint32_t timeout_ms) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    xEventGroupClearBits(events, GOT_IP_BIT | DISCONNECTED_BIT);
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // any network by that name, open or not
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_netif_set_hostname(sta_if, host_name);
    if (esp_wifi_connect() != ESP_OK) return SETUP_FAILED;
    const EventBits_t bits = xEventGroupWaitBits(events, GOT_IP_BIT | DISCONNECTED_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if (bits & GOT_IP_BIT) return SETUP_OK;
    esp_wifi_disconnect();
    if (!(bits & DISCONNECTED_BIT)) return SETUP_FAILED;
    switch (disconnect_reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return SETUP_WRONG_PASSWORD;
        case WIFI_REASON_NO_AP_FOUND:
            return SETUP_NOT_FOUND;
        default:
            return SETUP_FAILED;
    }
}

static bool join(void) {
    state = NET_JOINING;
    status_led_set(LED_JOINING);
    for (int attempt = 1; attempt <= JOIN_ATTEMPTS; attempt++) {
        printf("net: joining \"%s\" (attempt %d)\n", creds.ssid, attempt);
        const net_setup_state_t r = sta_join(creds.ssid, creds.pass, JOIN_TIMEOUT_MS);
        if (r == SETUP_OK) {
            printf("net: connected, IP %s\n", ip_str);
            state = NET_CONNECTED;
            status_led_set(LED_CONNECTED);
            return true;
        }
        printf("net: join failed (reason %u)\n", disconnect_reason);
    }
    return false;
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
static plat_lock_t scan_lock;

// Blocks until the scan is over (~3 s).
static void scan_now(void) {
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        printf("net: scan failed\n");
        return;
    }
    static wifi_ap_record_t recs[24];
    uint16_t n = sizeof(recs) / sizeof(recs[0]);
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return;
    scan_entry_t list[SCAN_MAX];
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue; // hidden networks have no name to pick
        int j = 0;
        while (j < count && strcmp(list[j].ssid, ssid)) j++;
        if (j < count) {
            if (recs[i].rssi > list[j].rssi) {
                list[j].rssi = recs[i].rssi;
                list[j].channel = recs[i].primary;
            }
            continue;
        }
        if (count == SCAN_MAX) continue;
        snprintf(list[count].ssid, sizeof(list[count].ssid), "%s", ssid);
        list[count].rssi = recs[i].rssi;
        list[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        list[count].channel = recs[i].primary;
        count++;
    }
    plat_lock_enter(&scan_lock);
    memcpy(scan_list, list, sizeof(list));
    scan_count = count;
    plat_lock_exit(&scan_lock);
}

size_t net_scan_json(char *out, size_t size) {
    if (state != NET_PORTAL) scan_now(); // in the real portal: the list from before it opened
    scan_entry_t list[SCAN_MAX];
    plat_lock_enter(&scan_lock);
    const int n = scan_count;
    memcpy(list, scan_list, sizeof(list));
    plat_lock_exit(&scan_lock);
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

// --- mDNS and DNS-SD (docs/SVS.md) ------------------------------------------------------------------

static void mdns_start(void) {
    if (mdns_init() != ESP_OK) {
        printf("net: mDNS failed to start\n");
        return;
    }
    mdns_hostname_set(host_name);
    mdns_instance_name_set(instance_name);
    char id[33];
    plat_board_id(id, sizeof(id));
    settings_t s;
    settings_get(&s);
    mdns_txt_item_t rt4k_txt[] = {
        {"id", id},
        {"ver", cruller_version},
        {"api", HTTP_API_VERSION},
        {"name", s.name},
    };
    mdns_txt_item_t rfc2217_txt[] = {{"id", id}};
    mdns_service_add(instance_name, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(instance_name, "_rt4k", "_tcp", 80, rt4k_txt, s.name[0] ? 4 : 3);
    mdns_service_add(instance_name, "_rfc2217", "_tcp", 2217, rfc2217_txt, 1);
}

// --- setup access point -------------------------------------------------------------------------------

static dns_server_t dns;
static volatile bool portal_up;
static volatile uint32_t portal_test_until_ms; // a test portal closes then (0: none)
static volatile int32_t portal_test_request = -1; // minutes asked for by net_portal_test (-1: none)

bool net_portal_active(void) { return portal_up; }

void net_portal_test(uint32_t minutes) { portal_test_request = (int32_t)minutes; }

// ESP-IDF's access point interface is 192.168.4.1/24 with a DHCP server; the catch-all DNS is ours,
// and the HTTP server sends every unknown address there to the setup page (captive portal). Next to
// a station link, the radio keeps the access point on the station's channel.
static void portal_open(uint8_t channel) {
    if (portal_up) return;
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "%s", PORTAL_SSID);
    cfg.ap.ssid_len = strlen(PORTAL_SSID);
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;
    cfg.ap.channel = channel ? channel : 1;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(ap_if, &info);
    ip_addr_t gw = IPADDR4_INIT(info.ip.addr);
    LOCK_TCPIP_CORE();
    dns_server_init(&dns, esp_netif_get_netif_impl(ap_if), &gw);
    UNLOCK_TCPIP_CORE();
    portal_up = true;
    printf("net: setup portal \"%s\" open\n", PORTAL_SSID);
}

static void portal_close(void) {
    if (!portal_up) return;
    LOCK_TCPIP_CORE();
    dns_server_deinit(&dns);
    UNLOCK_TCPIP_CORE();
    esp_wifi_set_mode(WIFI_MODE_STA);
    portal_up = false;
    printf("net: setup portal closed\n");
}

// --- the setup wizard's join (from the portal, which stays up meanwhile) ------------------------------

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
    const long left = st == SETUP_OK ? (long)(setup.restart_at_ms - plat_ms()) / 1000 : 0;
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
    net_setup_state_t result = sta_join(setup.ssid, setup.pass, SETUP_JOIN_MS);
    if (result == SETUP_OK) {
        snprintf(setup.ip, sizeof(setup.ip), "%s", ip_str);
        wifi_creds_t c = {0};
        snprintf(c.ssid, sizeof(c.ssid), "%s", setup.ssid);
        snprintf(c.pass, sizeof(c.pass), "%s", setup.pass);
        if (!creds_save(&c)) result = SETUP_FAILED;
        setup.restart_at_ms = plat_ms() + SETUP_CLOSE_MS;
    }
    printf("net: setup: \"%s\" %s%s%s\n", setup.ssid, result == SETUP_OK ? "joined, IP " : "failed",
        result == SETUP_OK ? setup.ip : "", result == SETUP_OK ? "; restarting in 20 s" : "");
    setup.st = result;
    setup.version++;
}

// --- the saved network, tried again from the portal ---------------------------------------------------
//
// After a power cut Cruller can start before the router, fail to join and land in the portal: the
// saved network is tried every RETRY_MS, and once it's back Cruller restarts on it. Trying takes the
// radio off the portal's channel for a few seconds, so with a phone on the portal (someone in the
// wizard, maybe) it waits up to RETRY_BUSY_MS.

static bool portal_busy(void) {
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}

static void retry_saved(void) {
    printf("net: trying \"%s\" again\n", creds.ssid);
    const net_setup_state_t r = sta_join(creds.ssid, creds.pass, SETUP_JOIN_MS);
    if (r == SETUP_OK) {
        printf("net: \"%s\" is back, restarting on it\n", creds.ssid);
        plat_reboot();
    }
    printf("net: \"%s\" still not there (reason %u)\n", creds.ssid, disconnect_reason);
}

static void portal_forever(void) {
    printf("net: starting setup portal \"%s\"\n", PORTAL_SSID);
    esp_wifi_disconnect();
    scan_now(); // networks to offer in the portal
    // The access point on the strongest network's channel: most likely the one the wizard joins.
    int best = -1;
    for (int i = 0; i < scan_count; i++) {
        if (scan_list[i].channel >= 1 && scan_list[i].channel <= 13 && (best < 0 || scan_list[i].rssi > scan_list[best].rssi)) best = i;
    }
    portal_open(best >= 0 ? scan_list[best].channel : 0);
    snprintf(ip_str, sizeof(ip_str), "192.168.4.1");
    state = NET_PORTAL;
    status_led_set(LED_PORTAL);
    uint32_t tick_ms = plat_ms(), tried_ms = tick_ms;
    for (;;) { // the HTTP task serves the portal; the wizard's join and the saved network's retry run here
        vTaskDelay(pdMS_TO_TICKS(200));
        if (setup.requested) {
            setup.requested = false;
            setup_join();
        }
        if (setup.st == SETUP_OK && (int32_t)(plat_ms() - setup.restart_at_ms) >= 0) {
            printf("net: setup done, restarting on \"%s\"\n", setup.ssid);
            plat_reboot();
        }
        if (creds.ssid[0] && plat_ms() - tick_ms >= RETRY_MS && setup.st != SETUP_JOINING && setup.st != SETUP_OK) {
            tick_ms = plat_ms();
            if (plat_ms() - tried_ms >= RETRY_BUSY_MS || !portal_busy()) {
                retry_saved();
                tried_ms = tick_ms = plat_ms();
            }
        }
    }
}

static void portal_test_step(void) {
    const int32_t req = portal_test_request;
    if (req >= 0) {
        portal_test_request = -1;
        if (req > 0) {
            portal_open(0);
            portal_test_until_ms = (plat_ms() + (uint32_t)req * 60000u) | 1;
            printf("net: test portal for %ld min\n", (long)req);
        } else {
            portal_test_until_ms = 0;
            portal_close();
        }
    }
    if (portal_test_until_ms && (int32_t)(plat_ms() - portal_test_until_ms) >= 0) {
        portal_test_until_ms = 0;
        portal_close();
    }
}

static void net_task(void *param) {
    (void)param;
    creds_load(&creds);
    names_init();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    if (!creds.ssid[0] || !join()) portal_forever();
    mdns_start();

    // Keep the link up: rejoin after it has been down for a while; the portal takes over only when
    // rejoining fails too.
    uint32_t down_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_MS));
        portal_test_step();
        if (link_up) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi_now = ap.rssi;
            down_ms = 0;
            continue;
        }
        down_ms += LINK_CHECK_MS;
        if (down_ms >= RECONNECT_AFTER_MS) {
            printf("net: link lost (reason %u), rejoining\n", disconnect_reason);
            ip_str[0] = 0;
            if (!join()) portal_forever();
            down_ms = 0;
        }
    }
}

// After esp_netif_init() and esp_event_loop_create_default() (main.c).
void net_start(void) {
    plat_lock_init(&scan_lock);
    events = xEventGroupCreate();
    sta_if = esp_netif_create_default_wifi_sta();
    ap_if = esp_netif_create_default_wifi_ap();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // credentials live in store.h, not in the driver's NVS
    esp_wifi_set_ps(WIFI_PS_NONE);          // latency over power: the board is mains-powered
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    xTaskCreate(net_task, "net", PLAT_STACK(NET_TASK_STACK), NULL, NET_TASK_PRIORITY, NULL);
}
