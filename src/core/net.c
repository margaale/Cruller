// net.h, the same on every board (each one's radio behind net_port.h): the station link with the saved
// network, rejoined when it drops; the setup portal ("Cruller_Setup", 192.168.4.1, open) when there is
// none or joining fails, with the setup wizard's join and the saved network tried again now and then;
// the nearby networks.

#include "net.h"
#include "net_port.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "creds.h"
#include "json.h"
#include "platform.h"
#include "settings.h"
#include "status_led.h"

#define NET_TASK_STACK      2048 // words
#define NET_TASK_PRIORITY   (tskIDLE_PRIORITY + 3)
#define JOIN_TIMEOUT_MS     30000   // per attempt; a network not found yet is looked for again till then
#define JOIN_ATTEMPTS       3
#define LINK_CHECK_MS       2000
#define RECONNECT_AFTER_MS  20000   // link down this long -> rejoin
#define PORTAL_SSID         "Cruller_Setup"
#define PORTAL_IP           "192.168.4.1"
#define SETUP_JOIN_MS       20000   // the setup wizard's join attempt
#define SETUP_CLOSE_MS      20000   // after it worked: the portal stays up this long, then Cruller restarts
#define RETRY_MS            60000   // in the portal: the saved network tried again this often (a router
#define RETRY_BUSY_MS       600000  // slower to boot than Cruller), or this seldom with a phone on the portal

static const char *const setup_names[] = {"idle", "joining", "ok", "wrong_password", "not_found", "failed"};

static volatile net_state_t state = NET_STARTING;
static char ip_str[16];
static wifi_creds_t creds;
// Names from the settings (settings.c): "cruller-living" (mDNS, DHCP) and "Cruller Living" (DNS-SD).
static char host_name[48], instance_name[48];
// The signal, read by the net task with its link check: other tasks (the status JSON, pushed every
// 0.5 s during an upload) only read the copy. Asking the Pico's CYW43 from the ws task during an upload
// to the RT4K coincided with the board freezing.
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

// --- scan -------------------------------------------------------------------------------------------

#define SCAN_MAX 16
typedef struct {
    char ssid[33];
    int16_t rssi;
    bool secure;
    uint8_t channel; // of its strongest access point
} scan_entry_t;
static scan_entry_t scan_list[SCAN_MAX];
static int scan_count; // (both under plat_critical_enter(): seldom touched, briefly)

// One access point heard (several per network): a network once, with its strongest one.
void net_scan_add(const char *ssid, int rssi, bool secure, uint8_t channel) {
    if (!ssid[0] || strlen(ssid) >= sizeof(scan_list[0].ssid)) return; // hidden networks have no name to pick
    plat_critical_enter();
    int i = 0;
    while (i < scan_count && strcmp(scan_list[i].ssid, ssid)) i++;
    if (i < scan_count) {
        if (rssi > scan_list[i].rssi) {
            scan_list[i].rssi = (int16_t)rssi;
            scan_list[i].channel = channel;
        }
    } else if (scan_count < SCAN_MAX) {
        scan_entry_t *e = &scan_list[scan_count++];
        strcpy(e->ssid, ssid);
        e->rssi = (int16_t)rssi;
        e->secure = secure;
        e->channel = channel;
    }
    plat_critical_exit();
}

static void scan(void) {
    plat_critical_enter();
    scan_count = 0;
    plat_critical_exit();
    net_port_scan();
}

// The list, strongest first.
static int scan_copy(scan_entry_t list[SCAN_MAX]) {
    plat_critical_enter();
    const int n = scan_count;
    memcpy(list, scan_list, sizeof(scan_list));
    plat_critical_exit();
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && list[j].rssi > list[j - 1].rssi; j--) {
            const scan_entry_t t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    }
    return n;
}

size_t net_scan_json(char *out, size_t size) {
    if (state != NET_PORTAL) scan(); // in the real portal the station is off: the list from before it opened
    scan_entry_t list[SCAN_MAX];
    const int count = scan_copy(list);
    size_t n = 0;
    if (size < 3 || !json_put(out, size - 1, &n, "[")) return 0; // (room kept for the "]")
    for (int i = 0; i < count; i++) {
        char item[256]; // each network whole, or not at all
        size_t k = 0;
        if (!json_put(item, sizeof(item), &k, i ? ",{\"ssid\":" : "{\"ssid\":") || !json_put_str(item, sizeof(item), &k, list[i].ssid) ||
            !json_putf(item, sizeof(item), &k, ",\"rssi\":%d,\"secure\":%s}", list[i].rssi, list[i].secure ? "true" : "false") ||
            !json_put(out, size - 1, &n, item)) {
            break;
        }
    }
    json_put(out, size, &n, "]");
    return n;
}

// --- joining ----------------------------------------------------------------------------------------

// persist: a network not found is looked for again until timeout_ms (a router still starting up).
static net_setup_state_t join_network(const char *ssid, const char *pass, uint32_t timeout_ms, bool persist) {
    const uint32_t t0 = plat_ms();
    for (;;) {
        const net_setup_state_t r = net_port_join(ssid, pass, timeout_ms - (plat_ms() - t0));
        if (r != SETUP_NOT_FOUND || !persist) return r;
        vTaskDelay(pdMS_TO_TICKS(250));
        if (plat_ms() - t0 >= timeout_ms) return r;
    }
}

// The saved network, at boot or after the link dropped.
static bool join(void) {
    state = NET_JOINING;
    status_led_set(LED_JOINING);
    for (int attempt = 1; attempt <= JOIN_ATTEMPTS; attempt++) {
        printf("net: joining \"%s\" (attempt %d)\n", creds.ssid, attempt);
        const net_setup_state_t r = join_network(creds.ssid, creds.pass, JOIN_TIMEOUT_MS, true);
        if (r == SETUP_OK) {
            net_port_ip(ip_str);
            printf("net: connected, IP %s\n", ip_str);
            state = NET_CONNECTED;
            status_led_set(LED_CONNECTED);
            net_port_mdns(host_name, instance_name);
            return true;
        }
        printf("net: join failed (%s)\n", setup_names[r]);
    }
    return false;
}

// --- setup access point -------------------------------------------------------------------------------

static volatile bool portal_up;
static volatile uint32_t portal_test_until_ms; // a test portal closes then (0: none)
static volatile int32_t portal_test_request = -1; // minutes asked for by net_portal_test (-1: none)

bool net_portal_active(void) { return portal_up; }

void net_portal_test(uint32_t minutes) { portal_test_request = (int32_t)minutes; }

static void portal_open(uint8_t channel) {
    if (portal_up) return;
    net_port_portal_open(PORTAL_SSID, channel);
    portal_up = true;
    printf("net: setup portal \"%s\" open\n", PORTAL_SSID);
}

static void portal_close(void) {
    if (!portal_up) return;
    net_port_portal_close();
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
    const net_setup_state_t st = setup.st;
    const long left = st == SETUP_OK ? (long)(setup.restart_at_ms - plat_ms()) / 1000 : 0;
    size_t n = 0;
    return json_putf(out, size, &n, "{\"state\":\"%s\",\"ssid\":", setup_names[st]) && json_put_str(out, size, &n, setup.ssid) &&
        json_putf(out, size, &n, ",\"rssi\":%d,\"ip\":\"%s\",\"hostname\":\"%s\",\"restart_in_s\":%ld}", setup.rssi, setup.ip,
            host_name, left > 0 ? left : 0)
        ? n : 0;
}

static void setup_join(void) {
    scan_entry_t list[SCAN_MAX];
    const int count = scan_copy(list);
    setup.rssi = 0;
    for (int i = 0; i < count; i++) {
        if (!strcmp(list[i].ssid, setup.ssid)) setup.rssi = list[i].rssi;
    }
    printf("net: setup: joining \"%s\" with the portal up\n", setup.ssid);
    net_setup_state_t result = join_network(setup.ssid, setup.pass, SETUP_JOIN_MS, false);
    if (result == SETUP_OK) {
        net_port_ip(setup.ip);
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

static void retry_saved(void) {
    printf("net: trying \"%s\" again\n", creds.ssid);
    const net_setup_state_t r = join_network(creds.ssid, creds.pass, SETUP_JOIN_MS, false);
    if (r == SETUP_OK) {
        printf("net: \"%s\" is back, restarting on it\n", creds.ssid);
        plat_reboot();
    }
    printf("net: \"%s\" still not there (%s)\n", creds.ssid, setup_names[r]);
}

static void portal_forever(void) {
    printf("net: starting setup portal \"%s\"\n", PORTAL_SSID);
    scan(); // the station interface is still on: networks to offer in the portal
    net_port_sta_off();
    // The access point on the strongest network's channel: most likely the one the wizard joins, so
    // the radio needn't hop channels then (which drops the phone off the setup network for a moment).
    scan_entry_t list[SCAN_MAX];
    const int count = scan_copy(list);
    int best = 0;
    while (best < count && (list[best].channel < 1 || list[best].channel > 13)) best++;
    if (best < count) printf("net: portal on channel %u, as \"%s\"\n", list[best].channel, list[best].ssid);
    portal_open(best < count ? list[best].channel : 0);
    snprintf(ip_str, sizeof(ip_str), "%s", PORTAL_IP);
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
            if (plat_ms() - tried_ms >= RETRY_BUSY_MS || !net_port_portal_busy()) {
                retry_saved();
                tried_ms = tick_ms = plat_ms();
            }
        }
    }
}

// A test portal next to the station link (net_portal_test), from the net task.
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
    net_port_start();
    if (!creds.ssid[0] || !join()) portal_forever();

    // Keep the link up: rejoin after the link has been down for a while; the portal takes over
    // only when rejoining fails too.
    uint32_t down_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_MS));
        portal_test_step();
        int rssi = rssi_now, why = 0;
        if (net_port_link(&rssi, &why)) {
            rssi_now = rssi;
            down_ms = 0;
            continue;
        }
        down_ms += LINK_CHECK_MS;
        if (down_ms >= RECONNECT_AFTER_MS) {
            printf("net: link lost (%d), rejoining\n", why);
            ip_str[0] = 0;
            if (!join()) portal_forever();
            down_ms = 0;
        }
    }
}

void net_start(void) {
    xTaskCreate(net_task, "net", PLAT_STACK(NET_TASK_STACK), NULL, NET_TASK_PRIORITY, NULL);
}
