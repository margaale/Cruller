// gameID's model (docs/GAMEID.md): the consoles it asks which game they run, and the games (the gameDB),
// each with the RT4K profile to load. What's checked, and how each is written as JSON: in the API's
// bodies and in the files (gameid.c). No I/O here (tests/test_gameid.c).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAMEID_CONSOLES_MAX 10
#define GAMEID_GAMES_MAX    1000
#define GAMEID_NAME_MAX     48  // a console's or a game's name, its 0 included
#define GAMEID_URL_MAX      128 // http://<address>[:port]/<path>
#define GAMEID_ID_MAX       64  // a game as a console reports it (SCUS-97481, an N64's CRCs...)
#define GAMEID_PROFILE_MAX  128 // a profile's path under the SD card's /profile ("PS2/God of War II.rt4")
#define GAMEID_SVS_INPUTS   8
#define GAMEID_NOT_ON_SVS   (-1) // a console's svs_input: straight to the RT4K, not through the SVS

typedef struct {
    char name[GAMEID_NAME_MAX];
    char url[GAMEID_URL_MAX];      // what's asked: its reply is JSON with "gameID", or the ID as text
    char other[GAMEID_PROFILE_MAX]; // the profile for a game the gameDB hasn't ('' none)
    int8_t svs_input;              // the SVS input it's on, 1-8; 0 (Auto): worked out from the console it is;
                                   // GAMEID_NOT_ON_SVS: not on the SVS
    bool enabled;
} gameid_console_t;

typedef struct {
    char id[GAMEID_ID_MAX];
    char profile[GAMEID_PROFILE_MAX];
    char name[GAMEID_NAME_MAX]; // what it's called (the console's own name for it, when it has one)
} gameid_game_t;

// A profile as gameID keeps it: a .rt4 or .rt6 under /profile, its path from there, no "..", no leading
// '/', no control characters.
bool gameid_profile_ok(const char *path);

// Whether it can be kept; *why says what's wrong otherwise.
bool gameid_console_ok(const gameid_console_t *c, const char **why);
bool gameid_game_ok(const gameid_game_t *g, const char **why);

// {"consoles": [{"name", "url", "other", "svs_input", "enabled"}, ...]} (as the API takes them and the
// file keeps them; "v", the format's version, is 1 or absent). How many, or -1 (*why says why).
int gameid_consoles_parse(const char *json, size_t len, gameid_console_t *out, int max, const char **why);

// The same written out, with "v": 1 when versioned. Its length, or 0 when it doesn't fit.
size_t gameid_consoles_json(const gameid_console_t *c, int n, bool versioned, char *out, size_t size);

// A game as the API takes it: {"id", "profile", "name"}.
bool gameid_game_parse(const char *json, size_t len, gameid_game_t *g, const char **why);

// A game as the file keeps it, a line each: ["id", "profile", "name"] (the first line: {"v": 1}).
bool gameid_game_from_line(const char *line, gameid_game_t *g);
size_t gameid_game_line(const gameid_game_t *g, char *out, size_t size); // its length, 0 when it doesn't fit

// A game as the API sends it: {"id": ..., "profile": ..., "name": ...}. Its length, 0 when it doesn't fit.
size_t gameid_game_json(const gameid_game_t *g, char *out, size_t size);

// --- asking a console (gameid_run.c) -------------------------------------------------------------------

#define GAMEID_HOST_MAX 64
#define GAMEID_MODE_MAX 16

// "http://host[:port]/path": the host, the port (80 when none) and the path, its query too. False for
// anything else (another scheme, user info, a host too long or with odd characters, no path).
bool gameid_split_url(const char *url, char *host, size_t host_size, uint16_t *port, const char **path);

// A console's whole reply (the status line, the headers, the body; a chunked body decoded in place): its
// status, *body and *body_len the body's. 0 when it isn't an HTTP/1.x reply (or a chunked body is cut).
int gameid_reply(char *buf, size_t len, const char **body, size_t *body_len);

// What a console says it runs.
typedef struct {
    char id[GAMEID_ID_MAX];     // "" when it runs none it can tell
    char name[GAMEID_NAME_MAX]; // its own name for the game, when it says one
    char mode[GAMEID_MODE_MAX]; // the console it is, when it says (a MemCard PRO: "PS1", "PS2", "GC")
} gameid_report_t;

// A reply's body read: JSON with "gameID" (and "gameName", "currentMode": a MemCard PRO's), or the ID as
// text (a PS1Digital's, an N64Digital's: one line of printable characters, trimmed). False when it's
// neither (an error page, JSON without "gameID").
bool gameid_read_report(const char *body, size_t len, gameid_report_t *out);

// The console it is, as the SVS Bridge names them ("ps1", "ps2", "n64", "gamecube"...): from what it
// reports, else from its name. "" when neither says.
const char *gameid_kind(const char *mode, const char *name);

// What's known of a console from asking it.
typedef struct {
    bool on;                    // it answered (with a game or none) the last time it was asked
    gameid_report_t game;       // what it runs, then
    char kind[GAMEID_MODE_MAX]; // gameid_kind's, kept from when it last said (a mode, or its name)
    uint32_t changed;           // when its game last changed, as a count that only grows (0: never)
} gameid_seen_t;

// Whose game is on screen: of the consoles enabled, on and running a game, the one whose game changed last;
// -1: none. With an SVS input reported (svs_input > 0), only those the RT4K shows: while it shows the SVS
// (on_svs 1), a console set to that input, or one on Auto that is the console the SVS Bridge says there
// (svs_device; when either isn't known, it counts); while it shows another input (on_svs 0), those not on
// the SVS and those on Auto; when that isn't known (-1), as on the SVS, those not on it taken as on Auto.
int gameid_pick(const gameid_console_t *c, const gameid_seen_t *seen, int n, int svs_input, const char *svs_device, int on_svs);

// The RT4K's active input, by its name, from its answer to a bare "input" ("input=0 HDMI ic=2 model=0"):
// false when it isn't one.
bool gameid_rt4k_input(const char *reply, char *name, size_t size);

// Whether the RT4K shows the SVS: 1, 0, or -1 when it can't be told. input: its active input's name
// ("HDMI", "HD15 YPbPr", "SCART RGBS", "RCA YPbPr"...); svs_out: the SVS's output to it, as the bridge says
// it ("vga", "scart", "component"; "" not said). An SVS is analog: never on HDMI.
int gameid_on_svs(const char *input, const char *svs_out);
