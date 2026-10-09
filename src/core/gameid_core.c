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
