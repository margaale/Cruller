// health.h's network check, the same on every board. The network can die while everything else keeps
// running (seen on the Pico 2 W with heavy USB host traffic: OTA flash writes, RTL1 bursts): the board is
// then unreachable but not hung, and asking the CYW43 for its RSSI still worked. What counts is
// traffic, so the gateway is pinged; once it has answered (some routers ignore pings), going
// NET_DEAD_MS without a reply makes the watchdog's feeder stop (health_net_dead()).

#include "health.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"

#include "net.h"
#include "platform.h"

#define PING_PERIOD_MS     2000
#define NET_DEAD_MS        10000  // ~5 unanswered pings
#define PING_ID            0xC3u
#define PING_TASK_STACK    512    // words: 512 bytes used on the Pico 2 W; ESP-IDF's own ping task has 2 KB
#define PING_TASK_PRIORITY (tskIDLE_PRIORITY + 2) // the feeder's

static volatile uint32_t last_reply_ms; // 0 = the gateway has never answered
static struct raw_pcb *ping_pcb;
static uint16_t ping_seq;

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
        LOCK_TCPIP_CORE();
        ping_gateway();
        UNLOCK_TCPIP_CORE();
        vTaskDelay(pdMS_TO_TICKS(PING_PERIOD_MS));
    }
}

void health_start_net_probe(void) {
    xTaskCreate(ping_task, "ping", PLAT_STACK(PING_TASK_STACK), NULL, PING_TASK_PRIORITY, NULL);
}

bool health_net_dead(void) {
    const uint32_t last = last_reply_ms;
    if (!last || net_state() == NET_PORTAL || plat_ms() - last <= NET_DEAD_MS) return false;
    printf("health: no reply from the gateway for %u s, letting the watchdog reset\n", NET_DEAD_MS / 1000);
    return true;
}

// Self-test: freezes the network the way the real failures looked (lwIP's lock held, the CPU fine) and
// lets go after `seconds`. With a working check the watchdog resets the board first.
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
