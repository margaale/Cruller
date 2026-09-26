// lwIP options for Cruller: lwIP runs in its own tcpip thread (NO_SYS=0) and application code
// enters it only through the core lock (LWIP_TCPIP_CORE_LOCKING).
// Based on raspberrypi/pico-examples pico_w/wifi/lwipopts_examples_common.h (BSD-3-Clause).

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_TCPIP_CORE_LOCKING     1
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1
#define LWIP_TIMEVAL_PRIVATE        0
#define LWIP_SO_RCVTIMEO            1
#define LWIP_SO_SNDTIMEO            1

#define TCPIP_THREAD_STACKSIZE      4096
#define DEFAULT_THREAD_STACKSIZE    1024
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8
#define TCPIP_MBOX_SIZE             16

// MEM_LIBC_MALLOC is incompatible with the non-polling cyw43_arch variants
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
// Sized for 8 long-lived clients (web pages + RFC 2217, clients.h) plus HTTP. Sockets: 2 listeners +
// 8 clients + 1 request + 4 waiting = 15. TCP connections: those plus TIME_WAIT. At 12/12/16 KB,
// 3 pages + 3 RFC 2217 + a burst of requests peaked at every limit, with failed allocations
// (GET /debug/memory). The FreeRTOS heap had over 100 KB to spare.
#define MEM_SIZE                    32000
#define MEMP_NUM_TCP_SEG            64
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_NETCONN            20
#define MEMP_NUM_TCP_PCB            24
#define PBUF_POOL_SIZE              24

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_CHKSUM_ALGORITHM       3

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// mDNS responder (cruller.local)
#define LWIP_MDNS_RESPONDER         1
#define LWIP_IGMP                   1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define MDNS_MAX_SERVICES           2
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

// Pool and heap use for GET /debug/memory (used, peak, size, failed allocations).
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   1
#define MEMP_STATS                  1
#define SYS_STATS                   0
#define LINK_STATS                  0
#define ETHARP_STATS                0
#define IP_STATS                    0
#define ICMP_STATS                  0
#define UDP_STATS                   0
#define TCP_STATS                   0
#define IGMP_STATS                  0

#endif // LWIPOPTS_H
