// net_port.h on the ESP32-S3: esp_wifi with ESP-IDF's netifs (the access point's DHCP server
// included), the catch-all DNS from picow_ap, ESP-IDF's mDNS.

#include "net_port.h"

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

#include "dnsserver.h"
#include "http.h"
#include "platform.h"
#include "settings.h"
#include "version.h"

#define GOT_IP_BIT          BIT0
#define DISCONNECTED_BIT    BIT1

static esp_netif_t *sta_if, *ap_if;
static EventGroupHandle_t events;
static volatile uint8_t disconnect_reason;
static volatile bool link_up;
static char sta_ip[16];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        disconnect_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        link_up = false;
        xEventGroupSetBits(events, DISCONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        link_up = true;
        xEventGroupSetBits(events, GOT_IP_BIT);
    }
}

// After esp_netif_init() and esp_event_loop_create_default() (main.c).
void net_port_start(void) {
    events = xEventGroupCreate();
    sta_if = esp_netif_create_default_wifi_sta();
    ap_if = esp_netif_create_default_wifi_ap();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // credentials live in store.h, not in the driver's NVS
    esp_wifi_set_ps(WIFI_PS_NONE);          // latency over power: the board is mains-powered
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

net_setup_state_t net_port_join(const char *ssid, const char *pass, uint32_t timeout_ms) {
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
    esp_netif_set_hostname(sta_if, net_hostname());
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

void net_port_ip(char out[16]) {
    snprintf(out, 16, "%s", sta_ip);
}

bool net_port_link(int *rssi, int *why) {
    if (!link_up) {
        *why = disconnect_reason;
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) *rssi = ap.rssi;
    return true;
}

// mDNS and DNS-SD (docs/SVS.md); ESP-IDF's follows the link itself after a rejoin.
void net_port_mdns(const char *host, const char *instance) {
    static bool started;
    if (started) return;
    if (mdns_init() != ESP_OK) {
        printf("net: mDNS failed to start\n");
        return;
    }
    started = true;
    mdns_hostname_set(host);
    mdns_instance_name_set(instance);
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
    mdns_service_add(instance, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(instance, "_rt4k", "_tcp", 80, rt4k_txt, s.name[0] ? 4 : 3);
    mdns_service_add(instance, "_rfc2217", "_tcp", 2217, rfc2217_txt, 1);
}

// One record at a time, so the driver's list needs no copy.
void net_port_scan(void) {
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        printf("net: scan failed\n");
        return;
    }
    wifi_ap_record_t rec;
    while (esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
        net_scan_add((const char *)rec.ssid, rec.rssi, rec.authmode != WIFI_AUTH_OPEN, rec.primary);
    }
    esp_wifi_clear_ap_list();
}

void net_port_sta_off(void) {
    esp_wifi_disconnect();
}

// --- setup access point -------------------------------------------------------------------------------

static dns_server_t dns;

// ESP-IDF's access point interface is 192.168.4.1/24 with a DHCP server; the catch-all DNS is ours.
// Next to a station link, the radio keeps the access point on the station's channel.
void net_port_portal_open(const char *ssid, uint8_t channel) {
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "%s", ssid);
    cfg.ap.ssid_len = strlen(ssid);
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
}

void net_port_portal_close(void) {
    LOCK_TCPIP_CORE();
    dns_server_deinit(&dns);
    UNLOCK_TCPIP_CORE();
    esp_wifi_set_mode(WIFI_MODE_STA);
}

bool net_port_portal_busy(void) {
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}
