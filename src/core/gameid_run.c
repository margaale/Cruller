#include "gameid_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include "console.h"
#include "gameid.h"
#include "json.h"
#include "platform.h"
#include "power.h"
#include "rt4k_info.h"
#include "svs.h"

#define TASK_STACK    1536 // words
#define TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define PERIOD_MS     2000 // between rounds of asking
#define CONNECT_MS    800  // a console that's off doesn't answer: don't wait on it
#define IO_MS         1500
#define MISSES_OFF    2    // rounds without an answer before a console counts as off (a Wi-Fi hiccup isn't)
#define AFTER_SVS_MS  3000 // after an input change, so the RT4K's own S<n> loads first
#define AFTER_ON_MS   6000 // after the RT4K comes on, so it's settled
#define LOAD_MS       15000
#define SEEK_AFTER_MS 30000  // the console the RT4K may show, not answering this long: looked for by its MAC
#define SEEK_EVERY_MS 120000 // ... and again at most this often
#define SEEK_BATCH    6      // ARP requests at a time (lwIP's table, where the replies land, holds ARP_TABLE_SIZE)
#define SEEK_WAIT_MS  100    // for their replies

// --- asking a console ------------------------------------------------------------------------------------

static char reply[1536]; // (the task's alone)

// 1: it said what it runs (*rep); 0: it answered something else (changes nothing); -1: no answer. *ip: the
// address asked (network order).
static int ask(const gameid_console_t *c, gameid_report_t *rep, uint32_t *ip) {
    char host[GAMEID_HOST_MAX], port_s[6];
    uint16_t port;
    const char *path;
    if (!gameid_split_url(c->url, host, sizeof(host), &port, &path)) return 0;
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) return -1;
    *ip = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    // connect, given CONNECT_MS (a console that's off leaves the SYN unanswered)
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    connect(fd, res->ai_addr, res->ai_addrlen); // (in progress)
    freeaddrinfo(res);
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {.tv_sec = 0, .tv_usec = CONNECT_MS * 1000};
    int err = 0;
    socklen_t elen = sizeof(err);
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err) {
        closesocket(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    const struct timeval io = {.tv_sec = IO_MS / 1000, .tv_usec = (IO_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));
    char req[GAMEID_URL_MAX + GAMEID_HOST_MAX + 96];
    const int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Cruller\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (n <= 0 || (size_t)n >= sizeof(req) || send(fd, req, (size_t)n, 0) != n) {
        closesocket(fd);
        return -1;
    }
    size_t got = 0;
    for (;;) {
        const int k = recv(fd, reply + got, sizeof(reply) - 1 - got, 0);
        if (k <= 0) break; // (closed, or the time's up: what came is read)
        got += (size_t)k;
        if (got >= sizeof(reply) - 1) break;
    }
    closesocket(fd);
    reply[got] = 0;
    if (!got) return -1;
    const char *body;
    size_t len;
    return gameid_reply(reply, got, &body, &len) == 200 && gameid_read_report(body, len, rep) ? 1 : 0;
}

// --- a console's MAC: learned when it answers, and looked for when its address may have changed --------------

// The MAC lwIP's ARP table has for ip (network order), into mac. False when it has none: an address off the
// local network has none (it goes through the gateway).
static bool arp_mac(uint32_t ip, uint8_t mac[6]) {
    ip4_addr_t a;
    ip4_addr_set_u32(&a, ip);
    struct eth_addr *eth = NULL;
    const ip4_addr_t *at = NULL;
    bool ok = false;
    LOCK_TCPIP_CORE();
    if (netif_default && etharp_find_addr(netif_default, &a, &eth, &at) >= 0 && eth) {
        memcpy(mac, eth->addr, 6);
        ok = true;
    }
    UNLOCK_TCPIP_CORE();
    return ok;
}

// Looks for mac on the local network: an ARP request to each address of it (a /24 at most: the one around
// Cruller's own address when it's bigger), a few at a time, lwIP's table read after each few. Where it is
// (network order), or 0.
static uint32_t arp_seek(const uint8_t mac[6]) {
    uint32_t me = 0, mask = 0;
    LOCK_TCPIP_CORE();
    if (netif_default && netif_is_up(netif_default) && netif_is_link_up(netif_default)) {
        me = ip4_addr_get_u32(netif_ip4_addr(netif_default));
        mask = ip4_addr_get_u32(netif_ip4_netmask(netif_default));
    }
    UNLOCK_TCPIP_CORE();
    if (!me) return 0;
    const uint32_t m = lwip_ntohl(mask) | 0xFFFFFF00u, base = lwip_ntohl(me) & m, hosts = ~m; // (/24 at most)
    for (uint32_t h = 1; h < hosts; h += SEEK_BATCH) {
        LOCK_TCPIP_CORE();
        for (uint32_t j = h; j < h + SEEK_BATCH && j < hosts && netif_default; j++) {
            ip4_addr_t a;
            ip4_addr_set_u32(&a, lwip_htonl(base | j));
            if (ip4_addr_get_u32(&a) != me) etharp_request(netif_default, &a);
        }
        UNLOCK_TCPIP_CORE();
        vTaskDelay(pdMS_TO_TICKS(SEEK_WAIT_MS));
        uint32_t found = 0;
        LOCK_TCPIP_CORE();
        for (size_t i = 0; i < ARP_TABLE_SIZE && !found; i++) {
            ip4_addr_t *ip;
            struct netif *nif;
            struct eth_addr *eth;
            if (etharp_get_entry(i, &ip, &nif, &eth) && !memcmp(eth->addr, mac, 6)) found = ip4_addr_get_u32(ip);
        }
        UNLOCK_TCPIP_CORE();
        if (found) return found;
    }
    return 0;
}

// --- what it knows ----------------------------------------------------------------------------------------

static SemaphoreHandle_t lock; // the state below, between the task and GET /api/v1/gameid/state
static gameid_console_t con[GAMEID_CONSOLES_MAX];
static gameid_seen_t seen[GAMEID_CONSOLES_MAX];
static uint8_t misses[GAMEID_CONSOLES_MAX];
static int con_n;
static uint32_t con_version, counter, seq = 1;

static int king = -1, last_input = -1; // the console on screen; the SVS input last seen
static char rt4k_input[24];             // the RT4K's active input, by its name ("" not asked, or not known)
static int on_svs = -1;                 // whether it shows the SVS (gameid_on_svs); -1 not known
static char want[GAMEID_PROFILE_MAX];   // its profile ("" none)
static const char *want_from = "";      // "gamedb", "other" (its console's), "svs" (its input's S<n>)
static char pending[GAMEID_PROFILE_MAX]; // to load at pending_at ("" none)
static uint32_t pending_at;
static char loaded[GAMEID_PROFILE_MAX];  // what gameID loaded last
static char note[200];                   // what it last did, or why it waits
static uint32_t note_ms;
static bool rt4k_was_on;
static const char *last_device = "";    // the SVS input's console, as the bridge says it ("" none)
static uint32_t lost_at[GAMEID_CONSOLES_MAX], sought_at[GAMEID_CONSOLES_MAX]; // seek(): since when its console
                                        // hasn't answered (0: it does), when it was last looked for

static void say(const char *fmt, const char *a, const char *b, const char *c) {
    char s[sizeof(note)];
    snprintf(s, sizeof(s), fmt, a, b, c);
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool fresh = strcmp(s, note) != 0;
    if (fresh) {
        memcpy(note, s, strlen(s) + 1);
        note_ms = plat_ms();
        seq++;
    }
    xSemaphoreGive(lock);
    if (fresh) printf("gameid: %s\n", s);
}

// The consoles saved, again (the page or the API changed them, or the gameDB): what's known of one
// kept while its address stays, the one on screen too, so an edit loads only what it changes (and
// the one on screen disabled is as if it went off).
static void reload(void) {
    static gameid_console_t fresh[GAMEID_CONSOLES_MAX];
    const int n = gameid_consoles_load(fresh, GAMEID_CONSOLES_MAX);
    gameid_seen_t keep[GAMEID_CONSOLES_MAX] = {0};
    uint8_t keep_misses[GAMEID_CONSOLES_MAX] = {0};
    int keep_king = -1;
    for (int k = 0; k < n; k++) {
        for (int j = 0; j < con_n; j++) {
            if (!strcmp(fresh[k].url, con[j].url)) {
                keep[k] = seen[j];
                keep_misses[k] = misses[j];
                if (j == king && keep_king < 0) keep_king = k;
                break;
            }
        }
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(con, fresh, sizeof(con));
    memcpy(seen, keep, sizeof(seen));
    memcpy(misses, keep_misses, sizeof(misses));
    con_n = n;
    king = keep_king; // (its console gone: what it wanted goes on the next round)
    if (!n) { // (no rounds without consoles)
        want[0] = pending[0] = 0;
        want_from = "";
    }
    seq++;
    xSemaphoreGive(lock);
    memset(lost_at, 0, sizeof(lost_at)); // (by index: counted again)
    memset(sought_at, 0, sizeof(sought_at));
}

// Console k's gameID device, as the log names it: "MemCard PRO2 (on PS2)", or "PS2's gameID device".
static const char *device_of(int k) {
    static char s[2 * GAMEID_NAME_MAX + 24];
    if (con[k].device[0]) snprintf(s, sizeof(s), "%s (on %s)", con[k].device, con[k].name);
    else snprintf(s, sizeof(s), "%s's gameID device", con[k].name);
    return s;
}

// Console k's gameID device answered at ip: its MAC, saved when it's new (Cruller finds it by it if its
// address changes).
static void learn_mac(int k, uint32_t ip) {
    uint8_t mac[6];
    char text[GAMEID_MAC_MAX];
    if (!arp_mac(ip, mac)) return;
    gameid_mac_text(mac, text);
    if (!strcmp(con[k].mac, text)) return;
    if (gameid_console_found(con[k].url, NULL, text)) printf("gameid: %s: MAC %s\n", device_of(k), text);
}

static void ask_all(void) {
    for (int k = 0; k < con_n; k++) {
        gameid_report_t rep;
        uint32_t ip = 0;
        const int r = con[k].enabled ? ask(&con[k], &rep, &ip) : -1;
        if (r > 0) learn_mac(k, ip);
        xSemaphoreTake(lock, portMAX_DELAY);
        gameid_seen_t *s = &seen[k];
        if (r > 0) {
            if (!s->on || strcmp(s->game.id, rep.id)) {
                s->changed = ++counter;
                seq++;
            } else if (strcmp(s->game.name, rep.name) || strcmp(s->game.mode, rep.mode)) {
                seq++;
            }
            s->on = true;
            s->game = rep;
            const char *kind = gameid_kind(rep.mode, con[k].name);
            if (kind[0]) snprintf(s->kind, sizeof(s->kind), "%s", kind);
            misses[k] = 0;
        } else if (r < 0 && s->on && (!con[k].enabled || ++misses[k] >= MISSES_OFF)) {
            s->on = false;
            s->game = (gameid_report_t){0};
            seq++;
        }
        xSemaphoreGive(lock);
    }
}

// Each SVS input's profile as the page last read the card: input's file name ("" none).
static void svs_profile(int input, char *out, size_t size) {
    out[0] = 0;
    const char *t = svs_profiles_text();
    for (const char *l = t; *l;) {
        const char *e = strchr(l, '\n');
        if (!e) e = l + strlen(l);
        const char *tab = memchr(l, '\t', (size_t)(e - l));
        if (tab && atoi(l) == input && (size_t)(e - tab) <= size) {
            memcpy(out, tab + 1, (size_t)(e - tab - 1));
            out[e - tab - 1] = 0;
            return;
        }
        l = *e ? e + 1 : e;
    }
}

static bool same_path(const char *a, const char *b) { // (FAT: no case)
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

static void schedule(const char *profile, uint32_t at) {
    snprintf(pending, sizeof(pending), "%s", profile);
    pending_at = at;
}

static void done(bool ok) { // the pending profile: loaded (or not)
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ok) memcpy(loaded, pending, sizeof(loaded));
    pending[0] = 0;
    xSemaphoreGive(lock);
}

// What the RT4K has loaded, asked now: Cruller's own copy is asked again 3 s after the SVS's input
// changes (the RT4K's own S<n>), as late as a load after it, so it may be from before.
static bool rt4k_profile_now(char *out, size_t size) {
    char r[GAMEID_PROFILE_MAX + 48];
    if (!console_query("prof get", "prof loaded=", r, sizeof(r), 2000)) return rt4k_info_profile(out, size);
    const char *l = strstr(r, "prof loaded="), *f = strstr(r, " file=");
    snprintf(out, size, "%s", l && l[12] == '1' && f ? f + 6 : "");
    return true;
}

static void load(void) {
    char cur[GAMEID_PROFILE_MAX + 16] = "", cmd[GAMEID_PROFILE_MAX + 16], r[96] = "";
    if (rt4k_profile_now(cur, sizeof(cur)) && same_path(cur, pending)) { // it has it already
        say("%s loaded already%s%s", pending, "", "");
        done(true);
        return;
    }
    snprintf(cmd, sizeof(cmd), "prof load %s", pending);
    const bool ok = console_query(cmd, "prof", r, sizeof(r), LOAD_MS) && !strcmp(r, "prof load ok");
    if (ok) say("loaded %s%s%s", pending, "", "");
    else say("could not load %s (%s)%s", pending, r[0] ? r : "no answer", "");
    rt4k_info_profile_soon();
    done(ok);
}

// Whose game is on screen now, and what that wants loaded; then loads it when it's time.
static void decide(void) {
    static svs_switch_t sw;
    svs_state_t sv;
    int input = 0;
    const char *device = "";
    if (svs_get(&sv) && sv.input > 0) {
        input = sv.input;
        if (svs_get_switch(&sw) && input <= sw.inputs_n) device = sw.inputs[input - 1].device;
    }
    const bool on = power_state() == PWR_ON, came_on = on && !rt4k_was_on;
    rt4k_was_on = on;
    // With consoles not on the SVS, the RT4K's input tells whose game it shows: asked (a bare "input" only
    // reads it), since it says nothing when it changes.
    bool direct = false;
    for (int j = 0; j < con_n; j++) direct |= con[j].enabled && con[j].svs_input == GAMEID_NOT_ON_SVS;
    char in_name[sizeof(rt4k_input)] = "", r[64];
    if (direct && input > 0 && on && console_query("input", "input=", r, sizeof(r), 2000)) gameid_rt4k_input(r, in_name, sizeof(in_name));
    const int shows = gameid_on_svs(in_name, sw.has_output ? sw.output.kind : "");
    const uint32_t now = plat_ms();
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool rt4k_changed = on_svs >= 0 && shows >= 0 && shows != on_svs;
    memcpy(rt4k_input, in_name, sizeof(rt4k_input));
    on_svs = shows;
    const int k = gameid_pick(con, seen, con_n, input, device, on_svs);
    char w[GAMEID_PROFILE_MAX] = "";
    const char *from = "";
    // its game in the gameDB: looked up when the game or the gameDB changes, not every round
    static char found_id[GAMEID_ID_MAX];
    static uint32_t found_version;
    static bool found;
    static gameid_game_t g;
    if (k >= 0 && (strcmp(found_id, seen[k].game.id) || found_version != gameid_version())) {
        memcpy(found_id, seen[k].game.id, sizeof(found_id));
        found_version = gameid_version();
        found = gameid_game_find(found_id, &g);
    }
    // its console, when the gameDB doesn't say it: the one it's seen on (its device's, else its SVS input's)
    const char *kind = k < 0 ? "" : seen[k].kind[0] ? seen[k].kind : con[k].svs_input > 0 && con[k].svs_input == input ? device : "";
    static gameid_game_t tag;
    static char tagged[GAMEID_ID_MAX]; // (once a game: a write that fails isn't tried again each round)
    const bool tag_it = k >= 0 && found && !g.console[0] && kind[0] && strlen(kind) < sizeof(g.console) && strcmp(tagged, g.id);
    if (tag_it) {
        tag = g;
        snprintf(tag.console, sizeof(tag.console), "%s", kind);
        memcpy(tagged, g.id, sizeof(tagged));
    }
    if (k >= 0 && found) { snprintf(w, sizeof(w), "%s", g.profile); from = "gamedb"; }
    else if (k >= 0 && con[k].other[0]) { snprintf(w, sizeof(w), "%s", con[k].other); from = "other"; }
    else if (k < 0 && king >= 0 && input > 0 && input == last_input && on_svs != 0) { // its console went off: the input's own again
        char file[SVS_PROFILE_MAX + 1];
        svs_profile(input, file, sizeof(file));
        if (file[0] && snprintf(w, sizeof(w), "SVS/%s", file) < (int)sizeof(w)) from = "svs";
        else w[0] = 0;
    } else if (k < 0 && !strcmp(want_from, "svs") && input == last_input && on_svs != 0) { // (and so it stays, on that input)
        memcpy(w, want, sizeof(w));
        from = want_from;
    }
    // the SVS's input changed, or the RT4K's to or from the SVS's: what the RT4K loads for it first
    const bool input_changed = (last_input >= 0 && input != last_input) || rt4k_changed;
    if (k != king || strcmp(w, want)) {
        king = k;
        snprintf(want, sizeof(want), "%s", w);
        want_from = from;
        seq++;
        if (w[0]) schedule(w, input_changed ? now + AFTER_SVS_MS : now);
    } else if (input_changed && w[0]) {
        schedule(w, now + AFTER_SVS_MS); // back on its input: after the S<n> the RT4K loads for it
    }
    if (came_on && want[0] && !pending[0]) schedule(want, now + AFTER_ON_MS);
    last_input = input;
    last_device = device;
    xSemaphoreGive(lock);
    bool replaced;
    if (tag_it && gameid_game_put(&tag, &replaced)) printf("gameid: %s is a %s game\n", tag.name[0] ? tag.name : tag.id, tag.console);

    if (!pending[0] || (int32_t)(now - pending_at) < 0) return;
    if (!on) {
        say("%s waits for the RT4K to be on%s%s", pending, "", "");
        return; // (kept: loaded once it's on)
    }
    load();
}

// The console the RT4K may be showing (gameid_seek), while it's on, not answering for SEEK_AFTER_MS: looked
// for by its MAC (every SEEK_EVERY_MS at most), its address saved when it's elsewhere. One a round.
static void seek(void) {
    const uint32_t now = plat_ms();
    const bool on = power_state() == PWR_ON;
    for (int k = 0; k < con_n; k++) {
        if (!on || !gameid_seek(&con[k], &seen[k], last_input, last_device, on_svs)) {
            lost_at[k] = 0;
            continue;
        }
        if (!lost_at[k]) lost_at[k] = now | 1;
        if (now - lost_at[k] < SEEK_AFTER_MS || (sought_at[k] && now - sought_at[k] < SEEK_EVERY_MS)) continue;
        sought_at[k] = now | 1;
        uint8_t mac[6];
        if (!gameid_mac_bytes(con[k].mac, mac)) continue;
        const uint32_t ip = arp_seek(mac);
        char at[16] = "", url[GAMEID_URL_MAX];
        if (ip) {
            ip4_addr_t a;
            ip4_addr_set_u32(&a, ip);
            ip4addr_ntoa_r(&a, at, sizeof(at));
        }
        if (!ip) printf("gameid: %s, MAC %s, not found on the network\n", device_of(k), con[k].mac);
        else if (!gameid_url_moved(con[k].url, at, url, sizeof(url)) || !strcmp(url, con[k].url)) printf("gameid: %s found at %s, where it was\n", device_of(k), at);
        else if (gameid_console_found(con[k].url, url, NULL)) printf("gameid: %s moved to %s (found by its MAC): its address saved\n", device_of(k), at);
        return;
    }
}

static void task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000)); // the network first
    for (;;) {
        const uint32_t t0 = plat_ms();
        if (gameid_version() != con_version) {
            con_version = gameid_version();
            reload();
        }
        if (con_n) {
            ask_all();
            decide();
            seek();
        }
        const uint32_t took = plat_ms() - t0;
        vTaskDelay(pdMS_TO_TICKS(took < PERIOD_MS ? PERIOD_MS - took : 100));
    }
}

void gameid_run_start(void) {
    lock = xSemaphoreCreateMutex();
    xTaskCreate(task, "gameid", PLAT_STACK(TASK_STACK), NULL, TASK_PRIORITY, NULL);
}

uint32_t gameid_state_seq(void) {
    return seq;
}

// --- its state as JSON --------------------------------------------------------------------------------

static bool add(char *out, size_t size, size_t *n, const char *s) {
    const size_t k = strlen(s);
    if (*n + k >= size) return false;
    memcpy(out + *n, s, k + 1);
    *n += k;
    return true;
}

static bool add_str(char *out, size_t size, size_t *n, const char *s) {
    if (*n + 2 >= size) return false;
    out[(*n)++] = '"';
    const int k = json_escape_str(out + *n, size - *n - 1, s);
    if (k < 0) return false;
    *n += (size_t)k;
    return add(out, size, n, "\"");
}

size_t gameid_state_json(char *out, size_t size) {
    if (!lock) return 0;
    size_t n = 0;
    char num[48];
    snprintf(num, sizeof(num), "{\"version\":%lu,\"consoles\":[", (unsigned long)gameid_version());
    bool ok = add(out, size, &n, num);
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int k = 0; ok && k < con_n; k++) {
        const gameid_seen_t *s = &seen[k];
        ok = add(out, size, &n, k ? ",{\"name\":" : "{\"name\":") && add_str(out, size, &n, con[k].name) &&
            add(out, size, &n, s->on ? ",\"on\":true,\"game\":" : ",\"on\":false,\"game\":") && add_str(out, size, &n, s->game.id) &&
            add(out, size, &n, ",\"game_name\":") && add_str(out, size, &n, s->game.name) &&
            add(out, size, &n, ",\"kind\":") && add_str(out, size, &n, s->kind) &&
            add(out, size, &n, k == king ? ",\"on_screen\":true}" : ",\"on_screen\":false}");
    }
    snprintf(num, sizeof(num), "],\"svs_input\":%d,\"rt4k_input\":", last_input < 0 ? 0 : last_input);
    ok = ok && add(out, size, &n, num) && add_str(out, size, &n, rt4k_input) &&
        add(out, size, &n, on_svs < 0 ? ",\"on_svs\":null,\"playing\":" : on_svs ? ",\"on_svs\":true,\"playing\":" : ",\"on_svs\":false,\"playing\":");
    if (ok && king >= 0) {
        ok = add(out, size, &n, "{\"console\":") && add_str(out, size, &n, con[king].name) && add(out, size, &n, ",\"game\":") &&
            add_str(out, size, &n, seen[king].game.id) && add(out, size, &n, ",\"game_name\":") && add_str(out, size, &n, seen[king].game.name) &&
            add(out, size, &n, "}");
    } else if (ok) {
        ok = add(out, size, &n, "null");
    }
    ok = ok && add(out, size, &n, ",\"profile\":") && add_str(out, size, &n, want) && add(out, size, &n, ",\"from\":") &&
        add_str(out, size, &n, want_from) && add(out, size, &n, ",\"pending\":") && add_str(out, size, &n, pending) &&
        add(out, size, &n, ",\"loaded\":") && add_str(out, size, &n, loaded) && add(out, size, &n, ",\"note\":") && add_str(out, size, &n, note);
    snprintf(num, sizeof(num), ",\"note_age_s\":%lu}", note[0] ? (unsigned long)((plat_ms() - note_ms) / 1000) : 0UL);
    ok = ok && add(out, size, &n, num);
    xSemaphoreGive(lock);
    return ok ? n : 0;
}
