// Host unit tests for gameID's model (src/core/gameid_core.c) and its files (src/core/gameid.c, over
// tests/cfgfs_ram.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "cfgfs.h"
#include "cfgfs_ram.h"
#include "gameid.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)
#define RUN(t) do { current = #t; t(); } while (0)

static const char *why;
static bool game(const char *json, gameid_game_t *g) { return gameid_game_parse(json, strlen(json), g, &why); }

static void test_profiles(void) {
    CHECK(gameid_profile_ok("PS2/God of War II.rt4") && gameid_profile_ok("SVS/S3_PS2.RT6") && gameid_profile_ok("a.rt4"));
    CHECK(!gameid_profile_ok("") && !gameid_profile_ok("/PS2/a.rt4") && !gameid_profile_ok("../a.rt4") && !gameid_profile_ok("a/../b.rt4"));
    CHECK(!gameid_profile_ok("a.txt") && !gameid_profile_ok("a.rt4 ") && !gameid_profile_ok("a\\b.rt4") && !gameid_profile_ok("a//b.rt4"));
}

static void test_consoles(void) {
    gameid_console_t c[GAMEID_CONSOLES_MAX];
    const char *json = "{\"consoles\":[{\"name\":\"PS2\",\"url\":\"http://10.10.10.88/api/currentState\",\"other\":\"PS2/Generic.rt4\","
                       "\"svs_input\":3,\"enabled\":true},{\"name\":\"N64 \\\"Digital\\\"\",\"url\":\"http://n64digital.local/gameid\"}]}";
    CHECK(gameid_consoles_parse(json, strlen(json), c, GAMEID_CONSOLES_MAX, &why) == 2);
    CHECK(!strcmp(c[0].name, "PS2") && c[0].svs_input == 3 && c[0].enabled && !strcmp(c[0].other, "PS2/Generic.rt4"));
    CHECK(!strcmp(c[1].name, "N64 \"Digital\"") && !c[1].other[0] && c[1].svs_input == 0 && c[1].enabled); // defaults
    // written out and read back
    char out[2048];
    const size_t n = gameid_consoles_json(c, 2, true, out, sizeof(out));
    gameid_console_t back[GAMEID_CONSOLES_MAX];
    CHECK(n && strstr(out, "\"v\":1") && gameid_consoles_parse(out, n, back, GAMEID_CONSOLES_MAX, &why) == 2 && !memcmp(back, c, 2 * sizeof(*c)));
    CHECK(!gameid_consoles_json(c, 2, false, out, 100)); // doesn't fit
    CHECK(gameid_consoles_parse("{\"consoles\":[]}", 15, c, GAMEID_CONSOLES_MAX, &why) == 0);
    // refused, with why
    const char *bad[] = {
        "[]", "{\"consoles\":{}}", "{\"v\":2,\"consoles\":[]}",
        "{\"consoles\":[{\"url\":\"http://a/\"}]}",                                // no name
        "{\"consoles\":[{\"name\":\"\",\"url\":\"http://a/\"}]}",                  // an empty one
        "{\"consoles\":[{\"name\":\"a\",\"url\":\"https://a/\"}]}",                // https, not yet
        "{\"consoles\":[{\"name\":\"a\",\"url\":\"http:///x\"}]}",                 // no host
        "{\"consoles\":[{\"name\":\"a\",\"url\":\"http://a b/\"}]}",
        "{\"consoles\":[{\"name\":\"a\",\"url\":\"http://a/\",\"svs_input\":9}]}",
        "{\"consoles\":[{\"name\":\"a\",\"url\":\"http://a/\",\"other\":\"x.txt\"}]}",
        "{\"consoles\":[{\"name\":\"a\",\"url\":\"http://a/\",\"enabled\":1}]}",
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
        why = NULL;
        const int r = gameid_consoles_parse(bad[k], strlen(bad[k]), c, GAMEID_CONSOLES_MAX, &why);
        if (r >= 0) printf("  accepted: %s\n", bad[k]);
        CHECK(r < 0 && why);
    }
    char many[2048] = "{\"consoles\":[";
    for (int k = 0; k < 11; k++) strcat(many, k ? ",{\"name\":\"a\",\"url\":\"http://a/\"}" : "{\"name\":\"a\",\"url\":\"http://a/\"}");
    strcat(many, "]}");
    CHECK(gameid_consoles_parse(many, strlen(many), c, GAMEID_CONSOLES_MAX, &why) < 0 && strstr(why, "10"));
}

static void test_game_text(void) {
    gameid_game_t g;
    const char *json = "{\"id\":\"SCUS-97481\",\"profile\":\"PS2/God of War II.rt4\",\"name\":\"God of War II\"}";
    CHECK(gameid_game_parse(json, strlen(json), &g, &why) && !strcmp(g.id, "SCUS-97481") && !strcmp(g.name, "God of War II"));
    CHECK(game("{\"id\":\"x\",\"profile\":\"a.rt4\"}", &g) && !g.name[0]); // the name optional
    CHECK(!game("{\"id\":\"\",\"profile\":\"a.rt4\"}", &g) && strstr(why, "ID"));
    CHECK(!game("{\"id\":\"x\",\"profile\":\"a\"}", &g) && strstr(why, ".rt4 or .rt6"));
    char l[512];
    g = (gameid_game_t){"3E5055B6-2E92DA52-N-45", "N64/Mario Kart 64.rt4", "Mario Kart \"64\""};
    const size_t n = gameid_game_line(&g, l, sizeof(l));
    gameid_game_t back;
    CHECK(n && !strchr(l, '\n') && gameid_game_from_line(l, &back) && !memcmp(&back, &g, sizeof(g)));
    CHECK(!gameid_game_from_line("{\"v\":1}", &back) && !gameid_game_from_line("[\"a\",\"b\"]", &back));
    CHECK(gameid_game_json(&g, l, sizeof(l)) && strstr(l, "\"name\":\"Mario Kart \\\"64\\\"\""));
}

static int listed;
static char listed_ids[8][GAMEID_ID_MAX];
static bool list(const gameid_game_t *g, void *ctx) {
    (void)ctx;
    if (listed < 8) snprintf(listed_ids[listed], sizeof(listed_ids[0]), "%s", g->id);
    listed++;
    return true;
}

static void test_files(void) {
    cfgfs_ram_fill(0xff);
    CHECK(cfgfs_mount());
    // consoles: none, then saved and read back
    gameid_console_t c[GAMEID_CONSOLES_MAX];
    CHECK(gameid_consoles_load(c, GAMEID_CONSOLES_MAX) == 0);
    const uint32_t v0 = gameid_version();
    const char *json = "{\"consoles\":[{\"name\":\"PS2\",\"url\":\"http://10.10.10.88/api/currentState\"}]}";
    CHECK(gameid_consoles_put_json(json, strlen(json), &why) && gameid_version() != v0);
    CHECK(gameid_consoles_load(c, GAMEID_CONSOLES_MAX) == 1 && !strcmp(c[0].url, "http://10.10.10.88/api/currentState"));
    char out[512];
    CHECK(gameid_consoles_get_json(out, sizeof(out)) && !strstr(out, "\"v\"") && strstr(out, "\"name\":\"PS2\""));
    CHECK(!gameid_consoles_put_json("{\"consoles\":[{}]}", 17, &why) && gameid_consoles_load(c, GAMEID_CONSOLES_MAX) == 1); // refused: kept
    // games: added, found, replaced, deleted, in order
    gameid_game_t g, f;
    bool replaced, found;
    listed = 0;
    CHECK(gameid_games_each(list, NULL) == 0 && !gameid_game_find("SCUS-97481", &f));
    const char *ids[] = {"SLUS-00214", "SCUS-97481", "GM4E0100"};
    for (int k = 0; k < 3; k++) {
        g = (gameid_game_t){0};
        snprintf(g.id, sizeof(g.id), "%s", ids[k]);
        snprintf(g.profile, sizeof(g.profile), "P/%d.rt4", k);
        CHECK(gameid_game_put(&g, &replaced) && !replaced);
    }
    CHECK(gameid_game_find("SCUS-97481", &f) && !strcmp(f.profile, "P/1.rt4") && !gameid_game_find("scus-97481", &f)); // as written
    g = (gameid_game_t){"SCUS-97481", "PS2/God of War II.rt4", "God of War II"};
    CHECK(gameid_game_put(&g, &replaced) && replaced && gameid_game_find("SCUS-97481", &f) && !strcmp(f.name, "God of War II"));
    listed = 0;
    CHECK(gameid_games_each(list, NULL) == 3 && !strcmp(listed_ids[0], "SLUS-00214") && !strcmp(listed_ids[1], "SCUS-97481")); // kept its place
    CHECK(gameid_game_delete("SLUS-00214", &found) && found && !gameid_game_find("SLUS-00214", &f));
    CHECK(gameid_game_delete("SLUS-00214", &found) && !found);
    g.profile[0] = 0;
    CHECK(!gameid_game_put(&g, &replaced)); // not a valid game: refused before writing
    listed = 0;
    CHECK(gameid_games_each(list, NULL) == 2);
    // survives a restart
    CHECK(cfgfs_mount() && gameid_game_find("GM4E0100", &f) && gameid_consoles_load(c, GAMEID_CONSOLES_MAX) == 1);
    CHECK(gameid_wipe() && !gameid_game_find("GM4E0100", &f) && gameid_consoles_load(c, GAMEID_CONSOLES_MAX) == 0);
}

// The gameDB full: 1000 games (long names, as they may be), and the 1001st refused.
static void test_full(void) {
    cfgfs_ram_fill(0xff);
    CHECK(cfgfs_format());
    gameid_game_t g = {0};
    bool replaced;
    int stored = 0;
    for (int k = 0; k < GAMEID_GAMES_MAX + 1; k++) {
        snprintf(g.id, sizeof(g.id), "SLUS-%05d", k);
        snprintf(g.profile, sizeof(g.profile), "PS1/Some long folder name/Game number %05d.rt4", k);
        snprintf(g.name, sizeof(g.name), "A game with a long-ish name, number %05d", k);
        if (gameid_game_put(&g, &replaced)) stored++;
        if (k % 250 == 0 && k) printf("  %d games...\n", k);
    }
    printf("  stored %d of %d\n", stored, GAMEID_GAMES_MAX + 1);
    gameid_game_t f;
    CHECK(stored == GAMEID_GAMES_MAX && gameid_game_find("SLUS-00999", &f) && !gameid_game_find("SLUS-01000", &f));
}

int main(void) {
    RUN(test_profiles);
    RUN(test_consoles);
    RUN(test_game_text);
    RUN(test_files);
    RUN(test_full);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
