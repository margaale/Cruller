#include "gameid_core.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

static bool ends_with(const char *s, const char *tail) {
    const size_t n = strlen(s), k = strlen(tail);
    if (n < k) return false;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); // (FAT: no case)
        if (c != tail[i]) return false;
    }
    return true;
}

bool gameid_profile_ok(const char *path) {
    if (!path[0] || path[0] == '/' || strstr(path, "..") || strstr(path, "//")) return false;
    for (const char *p = path; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == '\\') return false;
    }
    return ends_with(path, ".rt4") || ends_with(path, ".rt6");
}

bool gameid_console_ok(const gameid_console_t *c, const char **why) {
    if (!c->name[0]) { *why = "a console needs a name"; return false; }
    if (strncmp(c->url, "http://", 7) || !c->url[7] || c->url[7] == '/') {
        *why = "a console's address starts with http:// and its host (https isn't supported yet)";
        return false;
    }
    for (const char *p = c->url; *p; p++) {
        if ((unsigned char)*p <= 0x20) { *why = "a console's address can't have spaces"; return false; }
    }
    if (c->other[0] && !gameid_profile_ok(c->other)) { *why = "a profile is a .rt4 or .rt6 under /profile"; return false; }
    if (c->svs_input < GAMEID_NOT_ON_SVS || c->svs_input > GAMEID_SVS_INPUTS) {
        *why = "the SVS input is 1 to 8, 0 (worked out) or -1 (not on the SVS)";
        return false;
    }
    if (!gameid_mac_ok(c->mac)) { *why = "a MAC is six hex pairs joined by colons"; return false; }
    return true;
}

bool gameid_game_ok(const gameid_game_t *g, const char **why) {
    if (!g->id[0]) { *why = "a game needs its ID"; return false; }
    if (!gameid_profile_ok(g->profile)) { *why = "a profile is a .rt4 or .rt6 under /profile"; return false; }
    for (const char *c = g->console; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) { *why = "a console as the SVS Bridge names them: \"ps2\", \"n64\""; return false; }
    }
    return true;
}

#define TOKENS (3 + GAMEID_CONSOLES_MAX * 16)

int gameid_consoles_parse(const char *json, size_t len, gameid_console_t *out, int max, const char **why) {
    static json_tok_t tok[TOKENS]; // (one caller at a time: the HTTP task, or gameid.c's lock)
    *why = "not the consoles as JSON";
    if (json_parse(json, len, tok, TOKENS) < 1 || tok[0].type != JSON_OBJECT) return -1;
    long v = 1;
    const int vi = json_get(json, tok, 0, "v");
    if (vi >= 0 && (!json_long(json, tok, vi, &v) || v != 1)) { *why = "consoles saved by a newer Cruller"; return -1; }
    const int arr = json_get(json, tok, 0, "consoles");
    if (arr < 0 || tok[arr].type != JSON_ARRAY) return -1;
    if (tok[arr].size > max) { *why = "too many consoles (10 at most)"; return -1; }
    for (int k = 0; k < tok[arr].size; k++) {
        const int o = json_at(tok, arr, k);
        gameid_console_t c = {.enabled = true};
        long input = 0;
        const int vin = json_get(json, tok, o, "svs_input"), ven = json_get(json, tok, o, "enabled"), vot = json_get(json, tok, o, "other");
        const int vmac = json_get(json, tok, o, "mac"), vdev = json_get(json, tok, o, "device");
        if (tok[o].type != JSON_OBJECT || !json_str(json, tok, json_get(json, tok, o, "name"), c.name, sizeof(c.name)) ||
            !json_str(json, tok, json_get(json, tok, o, "url"), c.url, sizeof(c.url)) ||
            (vot >= 0 && !json_str(json, tok, vot, c.other, sizeof(c.other))) ||
            (vin >= 0 && (!json_long(json, tok, vin, &input) || input < GAMEID_NOT_ON_SVS || input > GAMEID_SVS_INPUTS)) ||
            (ven >= 0 && !json_bool(json, tok, ven, &c.enabled)) || (vmac >= 0 && !json_str(json, tok, vmac, c.mac, sizeof(c.mac))) ||
            (vdev >= 0 && !json_str(json, tok, vdev, c.device, sizeof(c.device)))) {
            *why = "each console: a name and a url (strings that fit), other, mac and device strings, svs_input -1 to 8, enabled true or false";
            return -1;
        }
        c.svs_input = (int8_t)input;
        for (char *m = c.mac; *m; m++) if (*m >= 'A' && *m <= 'F') *m = (char)(*m - 'A' + 'a'); // (kept in lower case)
        if (!gameid_console_ok(&c, why)) return -1;
        out[k] = c;
    }
    return tok[arr].size;
}

// Appends the string s (escaped, in quotes) at *n; false when it doesn't fit.
static bool put_str(char *out, size_t size, size_t *n, const char *s) {
    if (*n + 2 >= size) return false;
    out[(*n)++] = '"';
    const int k = json_escape_str(out + *n, size - *n - 1, s);
    if (k < 0) return false;
    *n += (size_t)k;
    if (*n + 1 >= size) return false;
    out[(*n)++] = '"';
    out[*n] = 0;
    return true;
}

static bool put(char *out, size_t size, size_t *n, const char *s) {
    const size_t k = strlen(s);
    if (*n + k >= size) return false;
    memcpy(out + *n, s, k + 1);
    *n += k;
    return true;
}

size_t gameid_consoles_json(const gameid_console_t *c, int count, bool versioned, char *out, size_t size) {
    size_t n = 0;
    if (!put(out, size, &n, versioned ? "{\"v\":1,\"consoles\":[" : "{\"consoles\":[")) return 0;
    for (int k = 0; k < count; k++) {
        char tail[48];
        snprintf(tail, sizeof(tail), ",\"svs_input\":%d,\"enabled\":%s}", c[k].svs_input, c[k].enabled ? "true" : "false");
        if (!put(out, size, &n, k ? ",{\"name\":" : "{\"name\":") || !put_str(out, size, &n, c[k].name) ||
            !put(out, size, &n, ",\"url\":") || !put_str(out, size, &n, c[k].url) ||
            !put(out, size, &n, ",\"other\":") || !put_str(out, size, &n, c[k].other) ||
            !put(out, size, &n, ",\"mac\":") || !put_str(out, size, &n, c[k].mac) ||
            !put(out, size, &n, ",\"device\":") || !put_str(out, size, &n, c[k].device) || !put(out, size, &n, tail)) return 0;
    }
    return put(out, size, &n, "]}") ? n : 0;
}

bool gameid_game_parse(const char *json, size_t len, gameid_game_t *g, const char **why) {
    json_tok_t tok[12];
    *why = "a game: {\"id\", \"profile\", \"name\", \"console\"}, strings that fit";
    memset(g, 0, sizeof(*g));
    if (json_parse(json, len, tok, 12) < 1 || tok[0].type != JSON_OBJECT) return false;
    const int vn = json_get(json, tok, 0, "name"), vc = json_get(json, tok, 0, "console");
    if (!json_str(json, tok, json_get(json, tok, 0, "id"), g->id, sizeof(g->id)) ||
        !json_str(json, tok, json_get(json, tok, 0, "profile"), g->profile, sizeof(g->profile)) ||
        (vn >= 0 && !json_str(json, tok, vn, g->name, sizeof(g->name))) ||
        (vc >= 0 && !json_str(json, tok, vc, g->console, sizeof(g->console)))) return false;
    return gameid_game_ok(g, why);
}

bool gameid_game_from_line(const char *line, gameid_game_t *g) {
    json_tok_t tok[6];
    memset(g, 0, sizeof(*g));
    const int n = json_parse(line, strlen(line), tok, 6);
    return (n == 4 || n == 5) && tok[0].type == JSON_ARRAY && tok[0].size == n - 1 &&
        json_str(line, tok, 1, g->id, sizeof(g->id)) && json_str(line, tok, 2, g->profile, sizeof(g->profile)) &&
        json_str(line, tok, 3, g->name, sizeof(g->name)) && (n == 4 || json_str(line, tok, 4, g->console, sizeof(g->console)));
}

size_t gameid_game_line(const gameid_game_t *g, char *out, size_t size) {
    size_t n = 0;
    return put(out, size, &n, "[") && put_str(out, size, &n, g->id) && put(out, size, &n, ",") && put_str(out, size, &n, g->profile) &&
        put(out, size, &n, ",") && put_str(out, size, &n, g->name) &&
        (!g->console[0] || (put(out, size, &n, ",") && put_str(out, size, &n, g->console))) && put(out, size, &n, "]") ? n : 0;
}

size_t gameid_game_json(const gameid_game_t *g, char *out, size_t size) {
    size_t n = 0;
    return put(out, size, &n, "{\"id\":") && put_str(out, size, &n, g->id) && put(out, size, &n, ",\"profile\":") &&
        put_str(out, size, &n, g->profile) && put(out, size, &n, ",\"name\":") && put_str(out, size, &n, g->name) &&
        put(out, size, &n, ",\"console\":") && put_str(out, size, &n, g->console) && put(out, size, &n, "}") ? n : 0;
}

// --- asking a console ----------------------------------------------------------------------------------

static bool host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

bool gameid_split_url(const char *url, char *host, size_t host_size, uint16_t *port, const char **path) {
    if (strncmp(url, "http://", 7)) return false;
    const char *h = url + 7, *p = h;
    while (host_char(*p)) p++;
    const size_t n = (size_t)(p - h);
    if (!n || n >= host_size) return false;
    long pt = 80;
    if (*p == ':') {
        pt = 0;
        const char *d = ++p;
        while (*p >= '0' && *p <= '9' && pt <= 65535) pt = pt * 10 + (*p++ - '0');
        if (p == d || pt < 1 || pt > 65535) return false;
    }
    if (*p != '/') return false;
    for (const char *q = p; *q; q++) {
        if ((unsigned char)*q <= 0x20 || *q == 0x7f) return false; // (it goes into the request line as it is)
    }
    memcpy(host, h, n);
    host[n] = 0;
    *port = (uint16_t)pt;
    *path = p;
    return true;
}

// The head's line starting with name (any case), its value; NULL when there's none.
static const char *header(const char *head, const char *end, const char *name) {
    const size_t k = strlen(name);
    for (const char *l = head; l < end;) {
        const char *e = memchr(l, '\n', (size_t)(end - l));
        if (!e) e = end;
        if ((size_t)(e - l) > k && l[k] == ':') {
            bool same = true;
            for (size_t i = 0; i < k && same; i++) {
                char a = l[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                same = a == b;
            }
            if (same) {
                const char *v = l + k + 1;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                return v;
            }
        }
        l = e + 1;
    }
    return NULL;
}

int gameid_reply(char *buf, size_t len, const char **body, size_t *body_len) {
    if (len < 12 || strncmp(buf, "HTTP/1.", 7) || buf[8] != ' ') return 0;
    int status = 0;
    for (int k = 9; k < 12; k++) {
        if (buf[k] < '0' || buf[k] > '9') return 0;
        status = status * 10 + (buf[k] - '0');
    }
    char *end = NULL;
    for (size_t k = 0; k + 3 < len; k++) {
        if (buf[k] == '\r' && buf[k + 1] == '\n' && buf[k + 2] == '\r' && buf[k + 3] == '\n') { end = buf + k + 4; break; }
    }
    if (!end) return 0;
    char *b = end, *const stop = buf + len;
    size_t n = (size_t)(stop - end);
    const char *te = header(buf, end, "transfer-encoding");
    if (te && !strncmp(te, "chunked", 7)) { // decoded in place: each chunk's size line dropped
        char *in = end, *out = end;
        for (;;) {
            size_t size = 0;
            int digits = 0;
            for (; in < stop; in++, digits++) {
                const char c = *in;
                const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0) break;
                size = size * 16 + (size_t)v;
            }
            char *eol = in;
            while (eol < stop && *eol != '\n') eol++;
            if (!digits || eol >= stop) return 0;
            in = eol + 1;
            if (!size) break;
            if ((size_t)(stop - in) < size) return 0;
            memmove(out, in, size);
            out += size;
            in += size;
            if (in < stop && *in == '\r') in++;
            if (in < stop && *in == '\n') in++;
        }
        n = (size_t)(out - end);
    } else {
        const char *cl = header(buf, end, "content-length");
        if (cl) {
            size_t want = 0;
            while (*cl >= '0' && *cl <= '9') want = want * 10 + (size_t)(*cl++ - '0');
            if (want < n) n = want;
        }
    }
    *body = b;
    *body_len = n;
    return status;
}

bool gameid_read_report(const char *body, size_t len, gameid_report_t *out) {
    memset(out, 0, sizeof(*out));
    size_t a = 0, z = len;
    while (a < z && (body[a] == ' ' || body[a] == '\t' || body[a] == '\r' || body[a] == '\n')) a++;
    while (z > a && (body[z - 1] == ' ' || body[z - 1] == '\t' || body[z - 1] == '\r' || body[z - 1] == '\n')) z--;
    if (a < z && body[a] == '{') {
        json_tok_t tok[48];
        if (json_parse(body + a, z - a, tok, 48) < 1) return false;
        const char *j = body + a;
        const int vi = json_get(j, tok, 0, "gameID"), vn = json_get(j, tok, 0, "gameName"), vm = json_get(j, tok, 0, "currentMode");
        if (vi < 0 || !json_str(j, tok, vi, out->id, sizeof(out->id))) return false;
        if (vn >= 0 && !json_str(j, tok, vn, out->name, sizeof(out->name))) out->name[0] = 0;
        if (vm >= 0 && !json_str(j, tok, vm, out->mode, sizeof(out->mode))) out->mode[0] = 0;
        out->json = true;
        return true;
    }
    // text: one line, printable, short enough to be an ID ("" when the console runs nothing it can tell)
    if (z - a >= sizeof(out->id)) return false;
    for (size_t k = a; k < z; k++) {
        if ((unsigned char)body[k] < 0x20 || body[k] == '<') return false; // (an HTML error page: no)
    }
    memcpy(out->id, body + a, z - a);
    out->id[z - a] = 0;
    return true;
}

// a's lowercase copy contains b
static bool has(const char *a, const char *b) {
    char low[GAMEID_NAME_MAX];
    size_t n = 0;
    for (; a[n] && n + 1 < sizeof(low); n++) low[n] = (char)(a[n] >= 'A' && a[n] <= 'Z' ? a[n] - 'A' + 'a' : a[n]);
    low[n] = 0;
    return strstr(low, b) != NULL;
}

const char *gameid_kind(const char *mode, const char *name) {
    // what a MemCard PRO's currentMode says, then what a name may (the SVS Bridge's ids)
    static const char *const known[][2] = {
        {"ps2", "ps2"}, {"ps1", "ps1"}, {"psx", "ps1"}, {"playstation 2", "ps2"}, {"playstation", "ps1"}, {"gamecube", "gamecube"},
        {"gc", "gamecube"}, {"ngc", "gamecube"}, {"n64", "n64"}, {"nintendo 64", "n64"}, {"dreamcast", "dreamcast"}, {"saturn", "saturn"},
    };
    for (int pass = 0; pass < 2; pass++) {
        const char *s = pass ? name : mode;
        if (!s || !s[0]) continue;
        for (size_t k = 0; k < sizeof(known) / sizeof(*known); k++) {
            if (pass == 0 ? has(s, known[k][0]) && strlen(s) <= strlen(known[k][0]) + 1 : has(s, known[k][0])) return known[k][1];
        }
    }
    return "";
}

int gameid_pick(const gameid_console_t *c, const gameid_seen_t *seen, int n, int svs_input, const char *svs_device, int on_svs) {
    int best = -1;
    for (int k = 0; k < n; k++) {
        if (!c[k].enabled || !seen[k].on || !seen[k].game.id[0]) continue;
        if (svs_input > 0) {
            const int at = c[k].svs_input;
            const bool its_kind = !seen[k].kind[0] || !svs_device || !svs_device[0] || !strcmp(seen[k].kind, svs_device);
            bool on_it;
            if (on_svs == 0) on_it = at <= 0;              // another input: those not on the SVS, and on Auto
            else if (at > 0) on_it = at == svs_input;      // the SVS: those set to its input
            else on_it = (at == 0 || on_svs < 0) && its_kind; // ... and on Auto of its console
            if (!on_it) continue;
        }
        if (best < 0 || seen[k].changed > seen[best].changed) best = k;
    }
    return best;
}

bool gameid_rt4k_input(const char *reply, char *name, size_t size) {
    const char *p = strstr(reply, "input=");
    if (!p || !size) return false;
    p += 6;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    const char *end = strstr(p, " ic="); // (from the number on: no name before it, none)
    while (*p == ' ') p++;
    size_t len = !end ? strlen(p) : end > p ? (size_t)(end - p) : 0;
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
    if (!len || len >= size) return false;
    memcpy(name, p, len);
    name[len] = 0;
    return true;
}

int gameid_on_svs(const char *input, const char *svs_out) {
    if (!input || !input[0]) return -1;
    if (!strncmp(input, "HDMI", 4)) return 0;
    // the RT4K's inputs by their connector, as it names them: the one the SVS's output goes to
    const char *port = !svs_out ? NULL : !strcmp(svs_out, "vga") ? "HD15" : !strcmp(svs_out, "scart") ? "SCART"
        : !strcmp(svs_out, "component") ? "RCA YPbPr" : NULL;
    if (!port) return -1;
    return !strncmp(input, port, strlen(port));
}

bool gameid_mac_ok(const char *mac) {
    if (!mac[0]) return true;
    for (int k = 0; k < 17; k++) {
        const char c = mac[k];
        if (k % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return !mac[17];
}

void gameid_mac_text(const uint8_t mac[6], char *out) {
    snprintf(out, GAMEID_MAC_MAX, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool gameid_mac_bytes(const char *mac, uint8_t out[6]) {
    if (!mac[0] || !gameid_mac_ok(mac)) return false;
    for (int k = 0; k < 6; k++) {
        const char *p = mac + 3 * k;
        const int hi = p[0] <= '9' ? p[0] - '0' : p[0] - 'a' + 10, lo = p[1] <= '9' ? p[1] - '0' : p[1] - 'a' + 10;
        out[k] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

// An IPv4 address as written: four numbers of up to three digits, joined by dots.
static bool ipv4(const char *s) {
    int dots = 0, digits = 0;
    for (; *s; s++) {
        if (*s == '.') {
            if (!digits) return false;
            dots++;
            digits = 0;
        } else if (*s >= '0' && *s <= '9' && digits < 3) {
            digits++;
        } else {
            return false;
        }
    }
    return dots == 3 && digits;
}

bool gameid_url_moved(const char *url, const char *ip, char *out, size_t size) {
    char host[GAMEID_HOST_MAX];
    uint16_t port;
    const char *path;
    if (!gameid_split_url(url, host, sizeof(host), &port, &path) || !ipv4(host) || !ipv4(ip)) return false;
    char at[8] = "";
    if (port != 80) snprintf(at, sizeof(at), ":%u", (unsigned)port);
    const int n = snprintf(out, size, "http://%s%s%s", ip, at, path);
    return n > 0 && (size_t)n < size;
}

bool gameid_seek(const gameid_console_t *c, const gameid_seen_t *s, int svs_input, const char *svs_device, int on_svs) {
    if (!c->enabled || !c->mac[0] || s->on) return false;
    if (svs_input > 0 && on_svs != 0) {
        if (c->svs_input > 0) return c->svs_input == svs_input;
        return c->svs_input == 0 && s->kind[0] && svs_device && !strcmp(s->kind, svs_device);
    }
    return c->svs_input == GAMEID_NOT_ON_SVS;
}

const char *gameid_model(const gameid_report_t *r, const char *kind) {
    if (r->json) {
        const char *k = r->mode[0] ? gameid_kind(r->mode, NULL) : "";
        return !strcmp(k, "ps2") ? "MemCard PRO2" : !strcmp(k, "gamecube") ? "MemCard PRO GC" : !strcmp(k, "ps1") ? "MemCard PRO" : "";
    }
    return !kind ? "" : !strcmp(kind, "ps1") ? "PS1Digital" : !strcmp(kind, "n64") ? "N64Digital" : "";
}
