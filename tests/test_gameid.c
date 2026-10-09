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

static void test_asking(void) {
    char host[GAMEID_HOST_MAX];
    uint16_t port = 0;
    const char *path = NULL;
    CHECK(gameid_split_url("http://10.10.10.88/api/currentState", host, sizeof(host), &port, &path) && !strcmp(host, "10.10.10.88") && port == 80 &&
        !strcmp(path, "/api/currentState"));
    CHECK(gameid_split_url("http://n64digital.local:8080/gameid?x=1", host, sizeof(host), &port, &path) && port == 8080 && !strcmp(path, "/gameid?x=1"));
    CHECK(!gameid_split_url("https://a/", host, sizeof(host), &port, &path) && !gameid_split_url("http://a", host, sizeof(host), &port, &path));
    CHECK(!gameid_split_url("http://u@a/", host, sizeof(host), &port, &path) && !gameid_split_url("http://a:0/", host, sizeof(host), &port, &path) &&
        !gameid_split_url("http://a:99999/", host, sizeof(host), &port, &path) && !gameid_split_url("http://a/b c", host, sizeof(host), &port, &path));

    // a reply: its status and body (Content-Length, chunked, neither)
    const char *body;
    size_t n;
    char r1[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 156\r\n\r\n"
                "{\n\t\"currentMode\":\t\"PS2\",\n\t\"gameName\":\t\"God of War II\",\n\t\"gameID\":\t\"SCUS-97481\",\n\t\"currentChannel\":\t1,\n"
                "\t\"playCount\":\t2,\n\t\"rssi\":\t-60,\n\t\"currentSize\":\t\"8MB\"\n}";
    CHECK(gameid_reply(r1, strlen(r1), &body, &n) == 200 && n == strlen(r1) - (size_t)(strstr(r1, "\r\n\r\n") + 4 - r1));
    gameid_report_t rep;
    CHECK(gameid_read_report(body, n, &rep) && !strcmp(rep.id, "SCUS-97481") && !strcmp(rep.name, "God of War II") && !strcmp(rep.mode, "PS2"));
    char r2[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\n3E5055\r\n10\r\nB6-2E92DA52-N-45\r\n0\r\n\r\n";
    CHECK(gameid_reply(r2, strlen(r2), &body, &n) == 200 && n == 22 && !memcmp(body, "3E5055B6-2E92DA52-N-45", 22));
    CHECK(gameid_read_report(body, n, &rep) && !strcmp(rep.id, "3E5055B6-2E92DA52-N-45") && !rep.mode[0]);
    char r3[] = "HTTP/1.0 404 Not Found\r\n\r\n<html>no</html>";
    CHECK(gameid_reply(r3, strlen(r3), &body, &n) == 404 && !gameid_read_report(body, n, &rep)); // an error page: not a game
    char r4[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nshort";
    CHECK(gameid_reply(r4, strlen(r4), &body, &n) == 0); // cut short
    char r5[] = "SSH-2.0-dropbear\r\n";
    CHECK(gameid_reply(r5, strlen(r5), &body, &n) == 0);
    CHECK(gameid_read_report("\r\n", 2, &rep) && !rep.id[0]);                                  // no game: an empty ID
    CHECK(!gameid_read_report("{\"currentMode\":\"PS2\"}", 21, &rep));                      // JSON without gameID
    CHECK(gameid_read_report("{\"gameID\":\"\",\"currentMode\":\"PS2\"}", 33, &rep) && !rep.id[0] && !strcmp(rep.mode, "PS2"));

    // the console it is
    CHECK(!strcmp(gameid_kind("PS2", "Living room"), "ps2") && !strcmp(gameid_kind("PS1", ""), "ps1") && !strcmp(gameid_kind("GC", ""), "gamecube"));
    CHECK(!strcmp(gameid_kind("", "N64Digital"), "n64") && !strcmp(gameid_kind("", "My PlayStation 2"), "ps2") && !strcmp(gameid_kind("", "PS1 Digital"), "ps1"));
    CHECK(!strcmp(gameid_kind("", "Console 1"), "") && !strcmp(gameid_kind("PS3", "x"), ""));
}

static void test_pick(void) {
    gameid_console_t c[3] = {{"PS2", "http://a/", "", 0, true}, {"N64", "http://b/", "", 0, true}, {"PS2 two", "http://c/", "", 5, true}};
    gameid_seen_t s[3] = {{true, {"SCUS-97481", "", "PS2"}, "ps2", 10}, {true, {"3E5055B6", "", ""}, "n64", 20}, {false, {"", "", ""}, "", 0}};
    CHECK(gameid_pick(c, s, 3, 0, NULL) == 1);        // no SVS: the last that changed
    CHECK(gameid_pick(c, s, 3, 2, "ps2") == 0);       // input 2 is the PS2's: the N64 isn't on screen
    CHECK(gameid_pick(c, s, 3, 3, "n64") == 1);
    CHECK(gameid_pick(c, s, 3, 4, "snes") == -1);     // neither's input
    CHECK(gameid_pick(c, s, 3, 4, "") == 1);          // the input's console not known: both count, the last
    s[2] = (gameid_seen_t){true, {"SLUS-20946", "", "PS2"}, "ps2", 30};
    CHECK(gameid_pick(c, s, 3, 5, "ps2") == 2 && gameid_pick(c, s, 3, 2, "ps2") == 0); // set to input 5: there only
    s[1].game.id[0] = 0;                                // the N64 runs no game it can tell
    CHECK(gameid_pick(c, s, 3, 0, NULL) == 2);
    c[2].enabled = false;
    CHECK(gameid_pick(c, s, 3, 0, NULL) == 0);
    s[1] = (gameid_seen_t){true, {"X", "", ""}, "", 40}; // a console whose kind isn't known counts on any input
    CHECK(gameid_pick(c, s, 3, 2, "ps2") == 1);
}

int main(void) {
    RUN(test_asking);
    RUN(test_pick);
    RUN(test_profiles);
    RUN(test_consoles);
    RUN(test_game_text);
    RUN(test_files);
    RUN(test_full);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
