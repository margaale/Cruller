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

typedef struct {
    char name[GAMEID_NAME_MAX];
    char url[GAMEID_URL_MAX];      // what's asked: its reply is JSON with "gameID", or the ID as text
    char other[GAMEID_PROFILE_MAX]; // the profile for a game the gameDB hasn't ('' none)
    uint8_t svs_input;             // the SVS input it's on, 1-8; 0: worked out from the console it is
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
