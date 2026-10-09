#include "gameid_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

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

// --- asking a console ------------------------------------------------------------------------------------

static char reply[1536]; // (the task's alone)

// 1: it said what it runs (*rep); 0: it answered something else (changes nothing); -1: no answer.
static int ask(const gameid_console_t *c, gameid_report_t *rep) {
    char host[GAMEID_HOST_MAX], port_s[6];
    uint16_t port;
    const char *path;
    if (!gameid_split_url(c->url, host, sizeof(host), &port, &path)) return 0;
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) return -1;
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

// --- what it knows ----------------------------------------------------------------------------------------

static SemaphoreHandle_t lock; // the state below, between the task and GET /api/v1/gameid/state
static gameid_console_t con[GAMEID_CONSOLES_MAX];
static gameid_seen_t seen[GAMEID_CONSOLES_MAX];
static uint8_t misses[GAMEID_CONSOLES_MAX];
static int con_n;
static uint32_t con_version, counter, seq = 1;

static int king = -1, last_input = -1; // the console on screen; the SVS input last seen
static char want[GAMEID_PROFILE_MAX];   // its profile ("" none)
static const char *want_from = "";      // "gamedb", "other" (its console's), "svs" (its input's S<n>)
static char pending[GAMEID_PROFILE_MAX]; // to load at pending_at ("" none)
static uint32_t pending_at;
static char loaded[GAMEID_PROFILE_MAX];  // what gameID loaded last
static char note[200];                   // what it last did, or why it waits
static uint32_t note_ms;
static bool rt4k_was_on;

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

// The consoles saved, again (the page or the API changed them): what's known of one kept while its
// address stays.
static void reload(void) {
    static gameid_console_t fresh[GAMEID_CONSOLES_MAX];
    const int n = gameid_consoles_load(fresh, GAMEID_CONSOLES_MAX);
    gameid_seen_t keep[GAMEID_CONSOLES_MAX] = {0};
    uint8_t keep_misses[GAMEID_CONSOLES_MAX] = {0};
    for (int k = 0; k < n; k++) {
        for (int j = 0; j < con_n; j++) {
            if (!strcmp(fresh[k].url, con[j].url)) { keep[k] = seen[j]; keep_misses[k] = misses[j]; break; }
        }
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(con, fresh, sizeof(con));
    memcpy(seen, keep, sizeof(seen));
    memcpy(misses, keep_misses, sizeof(misses));
    con_n = n;
    king = -1;
    want[0] = 0;
    seq++;
    xSemaphoreGive(lock);
}

static void ask_all(void) {
    for (int k = 0; k < con_n; k++) {
        gameid_report_t rep;
        const int r = con[k].enabled ? ask(&con[k], &rep) : -1;
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

static void load(void) {
    char cur[GAMEID_PROFILE_MAX + 16] = "", cmd[GAMEID_PROFILE_MAX + 16], r[96] = "";
    if (rt4k_info_profile(cur, sizeof(cur)) && same_path(cur, pending)) { // it has it already
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
    const uint32_t now = plat_ms();
    const bool on = power_state() == PWR_ON, came_on = on && !rt4k_was_on;
    rt4k_was_on = on;
    xSemaphoreTake(lock, portMAX_DELAY);
    const int k = gameid_pick(con, seen, con_n, input, device);
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
    if (k >= 0 && found) { snprintf(w, sizeof(w), "%s", g.profile); from = "gamedb"; }
    else if (k >= 0 && con[k].other[0]) { snprintf(w, sizeof(w), "%s", con[k].other); from = "other"; }
    else if (k < 0 && king >= 0 && input > 0 && input == last_input) { // its console went off: the input's own again
        char file[SVS_PROFILE_MAX + 1];
        svs_profile(input, file, sizeof(file));
        if (file[0] && snprintf(w, sizeof(w), "SVS/%s", file) < (int)sizeof(w)) from = "svs";
        else w[0] = 0;
    } else if (k < 0 && !strcmp(want_from, "svs") && input == last_input) { // (and so it stays, on that input)
        memcpy(w, want, sizeof(w));
        from = want_from;
    }
    const bool input_changed = last_input >= 0 && input != last_input;
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
    xSemaphoreGive(lock);

    if (!pending[0] || (int32_t)(now - pending_at) < 0) return;
    if (!on) {
        say("%s waits for the RT4K to be on%s%s", pending, "", "");
        return; // (kept: loaded once it's on)
    }
    load();
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
    bool ok = add(out, size, &n, "{\"consoles\":[");
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int k = 0; ok && k < con_n; k++) {
        const gameid_seen_t *s = &seen[k];
        ok = add(out, size, &n, k ? ",{\"name\":" : "{\"name\":") && add_str(out, size, &n, con[k].name) &&
            add(out, size, &n, s->on ? ",\"on\":true,\"game\":" : ",\"on\":false,\"game\":") && add_str(out, size, &n, s->game.id) &&
            add(out, size, &n, ",\"game_name\":") && add_str(out, size, &n, s->game.name) &&
            add(out, size, &n, ",\"kind\":") && add_str(out, size, &n, s->kind) &&
            add(out, size, &n, k == king ? ",\"on_screen\":true}" : ",\"on_screen\":false}");
    }
    snprintf(num, sizeof(num), "],\"svs_input\":%d,\"playing\":", last_input < 0 ? 0 : last_input);
    ok = ok && add(out, size, &n, num);
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
