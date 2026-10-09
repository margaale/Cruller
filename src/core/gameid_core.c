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
    if (c->svs_input > GAMEID_SVS_INPUTS) { *why = "the SVS input is 1 to 8, or 0 (worked out)"; return false; }
    return true;
}

bool gameid_game_ok(const gameid_game_t *g, const char **why) {
    if (!g->id[0]) { *why = "a game needs its ID"; return false; }
    if (!gameid_profile_ok(g->profile)) { *why = "a profile is a .rt4 or .rt6 under /profile"; return false; }
    return true;
}

#define TOKENS (3 + GAMEID_CONSOLES_MAX * 12)

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
        if (tok[o].type != JSON_OBJECT || !json_str(json, tok, json_get(json, tok, o, "name"), c.name, sizeof(c.name)) ||
            !json_str(json, tok, json_get(json, tok, o, "url"), c.url, sizeof(c.url)) ||
            (vot >= 0 && !json_str(json, tok, vot, c.other, sizeof(c.other))) ||
            (vin >= 0 && (!json_long(json, tok, vin, &input) || input < 0 || input > GAMEID_SVS_INPUTS)) ||
            (ven >= 0 && !json_bool(json, tok, ven, &c.enabled))) {
            *why = "each console: a name and a url (strings that fit), other a string, svs_input 0 to 8, enabled true or false";
            return -1;
        }
        c.svs_input = (uint8_t)input;
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
        snprintf(tail, sizeof(tail), ",\"svs_input\":%u,\"enabled\":%s}", (unsigned)c[k].svs_input, c[k].enabled ? "true" : "false");
        if (!put(out, size, &n, k ? ",{\"name\":" : "{\"name\":") || !put_str(out, size, &n, c[k].name) ||
            !put(out, size, &n, ",\"url\":") || !put_str(out, size, &n, c[k].url) ||
            !put(out, size, &n, ",\"other\":") || !put_str(out, size, &n, c[k].other) || !put(out, size, &n, tail)) return 0;
    }
    return put(out, size, &n, "]}") ? n : 0;
}

bool gameid_game_parse(const char *json, size_t len, gameid_game_t *g, const char **why) {
    json_tok_t tok[8];
    *why = "a game: {\"id\", \"profile\", \"name\"}, strings that fit";
    memset(g, 0, sizeof(*g));
    if (json_parse(json, len, tok, 8) < 1 || tok[0].type != JSON_OBJECT) return false;
    const int vn = json_get(json, tok, 0, "name");
    if (!json_str(json, tok, json_get(json, tok, 0, "id"), g->id, sizeof(g->id)) ||
        !json_str(json, tok, json_get(json, tok, 0, "profile"), g->profile, sizeof(g->profile)) ||
        (vn >= 0 && !json_str(json, tok, vn, g->name, sizeof(g->name)))) return false;
    return gameid_game_ok(g, why);
}

bool gameid_game_from_line(const char *line, gameid_game_t *g) {
    json_tok_t tok[5];
    memset(g, 0, sizeof(*g));
    return json_parse(line, strlen(line), tok, 5) == 4 && tok[0].type == JSON_ARRAY && tok[0].size == 3 &&
        json_str(line, tok, 1, g->id, sizeof(g->id)) && json_str(line, tok, 2, g->profile, sizeof(g->profile)) &&
        json_str(line, tok, 3, g->name, sizeof(g->name));
}

size_t gameid_game_line(const gameid_game_t *g, char *out, size_t size) {
    size_t n = 0;
    return put(out, size, &n, "[") && put_str(out, size, &n, g->id) && put(out, size, &n, ",") && put_str(out, size, &n, g->profile) &&
        put(out, size, &n, ",") && put_str(out, size, &n, g->name) && put(out, size, &n, "]") ? n : 0;
}

size_t gameid_game_json(const gameid_game_t *g, char *out, size_t size) {
    size_t n = 0;
    return put(out, size, &n, "{\"id\":") && put_str(out, size, &n, g->id) && put(out, size, &n, ",\"profile\":") &&
        put_str(out, size, &n, g->profile) && put(out, size, &n, ",\"name\":") && put_str(out, size, &n, g->name) &&
        put(out, size, &n, "}") ? n : 0;
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

int gameid_pick(const gameid_console_t *c, const gameid_seen_t *seen, int n, int svs_input, const char *svs_device) {
    int best = -1;
    for (int k = 0; k < n; k++) {
        if (!c[k].enabled || !seen[k].on || !seen[k].game.id[0]) continue;
        if (svs_input > 0) {
            const bool on_it = c[k].svs_input ? c[k].svs_input == svs_input
                : !seen[k].kind[0] || !svs_device || !svs_device[0] || !strcmp(seen[k].kind, svs_device);
            if (!on_it) continue;
        }
        if (best < 0 || seen[k].changed > seen[best].changed) best = k;
    }
    return best;
}
