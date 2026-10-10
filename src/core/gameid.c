#include "gameid.h"

#include <stdio.h>
#include <string.h>

#include "cfgfs.h"

#define CONSOLES_FILE "gameid-consoles.json"
#define GAMES_FILE    "gameid-games.jsonl"
#define GAMES_HEAD    "{\"v\":1}\n"
#define LINE_MAX      (2 * (GAMEID_ID_MAX + GAMEID_PROFILE_MAX + GAMEID_NAME_MAX)) // escaped, at worst

static volatile uint32_t version = 1;

// What goes through these is the files' lock's (cfgfs_hold, or a cfgfs call that holds it): one task at
// a time.
static char text[GAMEID_CONSOLES_MAX * 2 * (2 * GAMEID_NAME_MAX + GAMEID_URL_MAX + GAMEID_PROFILE_MAX + GAMEID_MAC_MAX + 64)]; // the consoles' file
static gameid_console_t consoles[GAMEID_CONSOLES_MAX];
static char line[LINE_MAX], out_line[LINE_MAX];

// The consoles' file, read and parsed (text, consoles): how many.
static int load(void) {
    const long n = cfgfs_read(CONSOLES_FILE, text, sizeof(text) - 1);
    const char *why;
    const int k = n < 0 ? 0 : gameid_consoles_parse(text, (size_t)n, consoles, GAMEID_CONSOLES_MAX, &why);
    return k < 0 ? 0 : k; // (none saved, or unreadable: no consoles to the user)
}

int gameid_consoles_load(gameid_console_t *out, int max) {
    cfgfs_hold();
    const int n = load();
    const int k = n < max ? n : max;
    memcpy(out, consoles, (size_t)k * sizeof(*out));
    cfgfs_release();
    return k;
}

size_t gameid_consoles_get_json(char *out, size_t size) {
    cfgfs_hold();
    const size_t n = gameid_consoles_json(consoles, load(), false, out, size);
    cfgfs_release();
    return n;
}

// The consoles as they were, for what a new list doesn't say: each one's MAC, kept while its address stays.
static gameid_console_t kept[GAMEID_CONSOLES_MAX];

bool gameid_consoles_put_json(const char *json, size_t len, const char **why) {
    cfgfs_hold();
    const int was = load();
    memcpy(kept, consoles, (size_t)was * sizeof(*kept));
    const int n = gameid_consoles_parse(json, len, consoles, GAMEID_CONSOLES_MAX, why);
    bool ok = n >= 0;
    for (int k = 0; ok && k < n; k++) {
        for (int j = 0; j < was && !consoles[k].mac[0]; j++) {
            if (!strcmp(consoles[k].url, kept[j].url)) memcpy(consoles[k].mac, kept[j].mac, sizeof(consoles[k].mac));
        }
    }
    if (ok) {
        const size_t k = gameid_consoles_json(consoles, n, true, text, sizeof(text));
        ok = k && cfgfs_write(CONSOLES_FILE, text, k);
        if (!ok) *why = "could not save them";
        else version++;
    }
    cfgfs_release();
    return ok;
}

bool gameid_console_found(const char *url, const char *new_url, const char *mac) {
    cfgfs_hold();
    const int n = load();
    bool ok = true, changed = false;
    for (int k = 0; k < n; k++) {
        if (strcmp(consoles[k].url, url)) continue;
        if (new_url && new_url[0] && strcmp(consoles[k].url, new_url)) {
            snprintf(consoles[k].url, sizeof(consoles[k].url), "%s", new_url);
            changed = true;
        }
        if (mac && mac[0] && strcmp(consoles[k].mac, mac)) {
            snprintf(consoles[k].mac, sizeof(consoles[k].mac), "%s", mac);
            changed = true;
        }
        break;
    }
    if (changed) {
        const size_t len = gameid_consoles_json(consoles, n, true, text, sizeof(text));
        ok = len && cfgfs_write(CONSOLES_FILE, text, len);
        if (ok) version++;
    }
    cfgfs_release();
    return ok;
}

// --- the gameDB, a line at a time ---------------------------------------------------------------------

typedef struct {
    const char *id;
    gameid_game_t *out;
    bool found;
} find_t;

static bool find_one(const char *l, void *ctx) {
    find_t *f = ctx;
    gameid_game_t g;
    if (!gameid_game_from_line(l, &g) || strcmp(g.id, f->id)) return true;
    *f->out = g;
    f->found = true;
    return false;
}

bool gameid_game_find(const char *id, gameid_game_t *out) {
    find_t f = {id, out, false};
    cfgfs_lines(GAMES_FILE, line, sizeof(line), find_one, &f);
    return f.found;
}

// Rewriting it: each old game kept but the one with the ID (replaced or dropped).
typedef struct {
    const char *id;
    const gameid_game_t *with; // NULL: dropped
    bool found, ok;
    int count;
} edit_t;

static bool put_line(const gameid_game_t *g) {
    const size_t n = gameid_game_line(g, out_line, sizeof(out_line) - 1);
    if (!n) return false;
    out_line[n] = '\n';
    return cfgfs_put(out_line, n + 1);
}

static bool edit_one(const char *l, void *ctx) {
    edit_t *e = ctx;
    gameid_game_t g;
    if (!gameid_game_from_line(l, &g)) return true; // (the head, or a line no Cruller wrote)
    if (!strcmp(g.id, e->id)) {
        if (e->found) return true; // (a second one with it: gone)
        e->found = true;
        if (!e->with) return true;
        g = *e->with;
    }
    e->count++;
    e->ok = put_line(&g);
    return e->ok;
}

static bool rewrite(const char *id, const gameid_game_t *with, bool *found) {
    edit_t e = {id, with, false, true, 0};
    if (!cfgfs_create(GAMES_FILE)) return false; // (the lock held till the commit or the abort)
    if (!cfgfs_put(GAMES_HEAD, strlen(GAMES_HEAD))) {
        cfgfs_abort();
        return false;
    }
    cfgfs_lines(GAMES_FILE, line, sizeof(line), edit_one, &e); // (none yet: nothing to keep)
    if (e.ok && with && !e.found) {
        if (e.count >= GAMEID_GAMES_MAX) e.ok = false; // full
        else e.ok = put_line(with);
    }
    if (!e.ok) {
        cfgfs_abort();
        return false;
    }
    if (!cfgfs_commit()) return false;
    if (found) *found = e.found;
    version++;
    return true;
}

bool gameid_game_put(const gameid_game_t *g, bool *replaced) {
    const char *why;
    return gameid_game_ok(g, &why) && rewrite(g->id, g, replaced);
}

bool gameid_game_delete(const char *id, bool *found) {
    gameid_game_t g;
    if (!gameid_game_find(id, &g)) { // nothing to rewrite
        *found = false;
        return true;
    }
    return rewrite(id, NULL, found);
}

typedef struct {
    bool (*fn)(const gameid_game_t *g, void *ctx);
    void *ctx;
    int count;
} each_t;

static bool each_one(const char *l, void *ctx) {
    each_t *e = ctx;
    gameid_game_t g;
    if (!gameid_game_from_line(l, &g)) return true;
    e->count++;
    return e->fn(&g, e->ctx);
}

int gameid_games_each(bool (*fn)(const gameid_game_t *g, void *ctx), void *ctx) {
    each_t e = {fn, ctx, 0};
    cfgfs_lines(GAMES_FILE, line, sizeof(line), each_one, &e); // (none saved: none read)
    return e.count;
}

uint32_t gameid_version(void) {
    return version;
}

bool gameid_wipe(void) {
    const bool ok = cfgfs_remove(CONSOLES_FILE) && cfgfs_remove(GAMES_FILE);
    version++;
    return ok;
}
