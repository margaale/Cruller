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
#include "status_led.h"

#define NET_TASK_STACK      2048
#define NET_TASK_PRIORITY   (tskIDLE_PRIORITY + 3)
#define JOIN_TIMEOUT_MS     30000
#define JOIN_ATTEMPTS       3
#define LINK_CHECK_MS       2000
#define RECONNECT_AFTER_MS  20000   // link down this long -> rejoin
#define PORTAL_SSID         "Cruller_Setup"
#define MDNS_HOSTNAME       "cruller"

static volatile net_state_t state = NET_STARTING;
static char ip_str[16];
static wifi_creds_t creds;

net_state_t net_state(void) { return state; }
const char *net_ip(void) { return ip_str; }
const char *net_ssid(void) { return state == NET_PORTAL ? PORTAL_SSID : creds.ssid; }

static void set_ip(const ip4_addr_t *addr) {
    if (addr) ip4addr_ntoa_r(addr, ip_str, sizeof(ip_str));
    else ip_str[0] = 0;
}

static void mdns_start(struct netif *nif) {
    static bool started = false;
    LOCK_TCPIP_CORE();
    if (!started) {
        mdns_resp_init();
        started = true;
    }
    mdns_resp_add_netif(nif, MDNS_HOSTNAME);
    mdns_resp_add_service(nif, "Cruller", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
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

static void portal_forever(void) {
    printf("net: starting setup portal \"%s\"\n", PORTAL_SSID);
    cyw43_arch_disable_sta_mode();
    cyw43_arch_enable_ap_mode(PORTAL_SSID, NULL, CYW43_AUTH_OPEN);

    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    // The CYW43 driver gives the AP netif 192.168.4.1/24; serve DHCP and a catch-all DNS on it.
    static dhcp_server_t dhcp;
    static dns_server_t dns;
    struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
    cyw43_arch_lwip_begin();
    dhcp_server_init(&dhcp, ap, &gw, &mask);
    dns_server_init(&dns, ap, &gw);
    cyw43_arch_lwip_end();
    set_ip(&gw);
    state = NET_PORTAL;
    status_led_set(LED_PORTAL);
    for (;;) vTaskDelay(portMAX_DELAY); // HTTP task serves the portal
}

static void net_task(void *param) {
    (void)param;
    if (!creds_load(&creds)) {
        wifi_creds_t imported;
        if (creds_import_donutshop(&imported)) {
            creds = imported;
            printf("net: imported Wi-Fi \"%s\" from DonutShop, saving: %s\n", creds.ssid, creds_save(&creds) ? "ok" : "FAILED");
        }
    }

    cyw43_arch_enable_sta_mode();
    if (!creds.ssid[0] || !join()) portal_forever();
    mdns_start(&cyw43_state.netif[CYW43_ITF_STA]);

    // Keep the link up: rejoin after the link has been down for a while; the portal takes over
    // only when rejoining fails too.
    uint32_t down_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_MS));
        const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
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
