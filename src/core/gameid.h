// gameID's consoles and games (gameid_core.h), kept in cfgfs.h's files: the consoles in one small file,
// the gameDB a game a line (it can hold 1000: never all in RAM; read through, rewritten through). Any
// task may call these (the files' lock covers each).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gameid_core.h"

// The consoles saved (at most max): how many (0 when none were, or they can't be read).
int gameid_consoles_load(gameid_console_t *out, int max);

// The consoles as the API sends them ({"consoles": [...]}): its length, 0 when it doesn't fit.
size_t gameid_consoles_get_json(char *out, size_t size);

// Replaces them all from the API's JSON (gameid_consoles_parse); *why says what's wrong otherwise. One
// without a MAC keeps the one it had while its address stays.
bool gameid_consoles_put_json(const char *json, size_t len, const char **why);

// The console at url as gameID found it: its MAC (learned when it answered) or its new address (found by
// its MAC), either NULL or "" to leave as it is. Saved only when it changes; false when it couldn't be.
bool gameid_console_found(const char *url, const char *new_url, const char *mac);

// The game with this ID, if the gameDB has it (IDs compared as written).
bool gameid_game_find(const char *id, gameid_game_t *out);

// Adds the game at the end, or replaces the one with its ID (*replaced says which). False when it can't
// be written, or the gameDB is full.
bool gameid_game_put(const gameid_game_t *g, bool *replaced);

// *found: whether it was there.
bool gameid_game_delete(const char *id, bool *found);

// Each game to fn, in the gameDB's order, until fn returns false: how many were read (none saved: 0).
// The files are held meanwhile: fn mustn't wait on another task that uses them.
int gameid_games_each(bool (*fn)(const gameid_game_t *g, void *ctx), void *ctx);

// Changes when the consoles or the games do (for whoever shows or uses them).
uint32_t gameid_version(void);

// Both files gone (a factory reset).
bool gameid_wipe(void);
