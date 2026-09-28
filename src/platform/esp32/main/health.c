// health.h on the ESP32-S3: the task watchdog (8 s, then a reset; see sdkconfig.defaults) watches the
// wdt task, and the gateway pings stop it being fed once the network stops passing traffic. Unlike
// the Pico 2 W, an image on trial runs under the same watchdog: a reset before it is confirmed makes
// the bootloader roll back.

#include "health.h"

#include <stdio.h>
#include <string.h>

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"

#include "net.h"
#include "platform.h"

#define WDT_FEED_MS       1000
#define WDT_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

// The same check as rp2: once the gateway has answered a ping, NET_DEAD_MS without a reply stops
// feeding the watchdog.
#define PING_PERIOD_MS    2000
#define NET_DEAD_MS       10000
#define PING_ID           0xC3u

static volatile uint32_t last_reply_ms; // 0 = the gateway has never answered
static struct raw_pcb *ping_pcb;
static SemaphoreHandle_t ping_gate;
static uint16_t ping_seq;

static volatile uint32_t last_feed_ms;
volatile uint32_t health_feed_us;
static volatile bool rebooting;

static void wdt_task(void *param) {
    (void)param;
    esp_task_wdt_add(NULL);
    for (;;) {
        const uint32_t last = last_reply_ms;
        if (last && net_state() != NET_PORTAL && plat_ms() - last > NET_DEAD_MS) {
            printf("health: no reply from the gateway for %u s, letting the watchdog reset\n", NET_DEAD_MS / 1000);
            for (;;) vTaskDelay(portMAX_DELAY); // subscribed and never fed again
        }
        if (!rebooting) esp_task_wdt_reset();
        last_feed_ms = plat_ms();
        health_feed_us = plat_us();
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_MS));
    }
}

void health_start(bool trial) {
    xTaskCreate(wdt_task, "wdt", PLAT_STACK(768), NULL, WDT_TASK_PRIORITY, NULL);
    printf("health: task watchdog %d s%s\n", CONFIG_ESP_TASK_WDT_TIMEOUT_S, trial ? ", image on trial" : "");
}

void health_rearm_watchdog(void) {
}

void health_stop_feeding(void) {
    rebooting = true;
}

uint32_t health_last_feed_ms(void) {
    return last_feed_ms;
}

// lwIP (tcpip thread): take our echo replies, pass everything else on.
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg;
    (void)pcb;
    (void)addr;
    if (p->tot_len >= PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr) && pbuf_remove_header(p, PBUF_IP_HLEN) == 0) {
        const struct icmp_echo_hdr *echo = p->payload;
        if (ICMPH_TYPE(echo) == ICMP_ER && echo->id == PING_ID) {
            last_reply_ms = plat_ms() | 1; // never 0
            pbuf_free(p);
            return 1;
        }
        pbuf_add_header(p, PBUF_IP_HLEN);
    }
    return 0;
}

static void ping_gateway(void) {
    if (!netif_default || !netif_is_link_up(netif_default) || ip4_addr_isany_val(*netif_ip4_gw(netif_default))) return;
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr), PBUF_RAM);
    if (!p) return;
    struct icmp_echo_hdr *echo = p->payload;
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->id = PING_ID;
    echo->seqno = lwip_htons(++ping_seq);
    echo->chksum = 0;
    echo->chksum = inet_chksum(echo, sizeof(*echo));
    ip_addr_t gw;
    ip_addr_copy_from_ip4(gw, *netif_ip4_gw(netif_default));
    raw_sendto(ping_pcb, p, &gw);
    pbuf_free(p);
}

static void ping_task(void *param) {
    (void)param;
    LOCK_TCPIP_CORE();
    ping_pcb = raw_new(IP_PROTO_ICMP);
    if (ping_pcb) {
        raw_recv(ping_pcb, ping_recv, NULL);
        raw_bind(ping_pcb, IP_ADDR_ANY);
    }
    UNLOCK_TCPIP_CORE();
    if (!ping_pcb) {
        printf("health: no raw pcb, network check off\n");
        vTaskDelete(NULL);
    }
    for (;;) {
        xSemaphoreTake(ping_gate, portMAX_DELAY);
        LOCK_TCPIP_CORE();
        ping_gateway();
        UNLOCK_TCPIP_CORE();
        xSemaphoreGive(ping_gate);
        vTaskDelay(pdMS_TO_TICKS(PING_PERIOD_MS));
    }
}

// After esp_netif_init() (lwIP's tcpip thread is up).
void health_start_net_probe(void) {
    ping_gate = xSemaphoreCreateMutex();
    xTaskCreate(ping_task, "ping", PLAT_STACK(768), NULL, WDT_TASK_PRIORITY, NULL);
}

void health_stop_net_probe(void) {
    if (ping_gate) xSemaphoreTake(ping_gate, pdMS_TO_TICKS(100));
    last_reply_ms = 0;
}

// Self-test: holds lwIP's core lock for `seconds`, as the network freezes looked on rp2.
static void wedge_task(void *param) {
    const uint32_t seconds = (uint32_t)(uintptr_t)param;
    printf("health: self-test, holding the network lock for %lu s\n", (unsigned long)seconds);
    LOCK_TCPIP_CORE();
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    UNLOCK_TCPIP_CORE();
    printf("health: self-test released the network lock (watchdog did NOT fire)\n");
    vTaskDelete(NULL);
}

void health_wedge_network(uint32_t seconds) {
    xTaskCreate(wedge_task, "wedge", PLAT_STACK(512), (void *)(uintptr_t)seconds, tskIDLE_PRIORITY + 2, NULL);
}
