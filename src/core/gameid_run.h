// gameID at work (docs/GAMEID.md): a task that asks each console which game it runs, every 2 s, and loads
// the profile for the game on screen (its gameDB's, else its console's own) with "prof load": when that
// game changes, when its SVS input comes back on screen (after the RT4K's own S<n>), and when the RT4K
// comes on. A console that turns off gives its input's S<n> back. The consoles and the gameDB: gameid.h.

#pragma once

#include <stddef.h>
#include <stdint.h>

void gameid_run_start(void);

// What it knows and did, as JSON (GET /api/v1/gameid/state, docs/API.md): its length, 0 when it doesn't
// fit.
size_t gameid_state_json(char *out, size_t size);

// Changes whenever gameid_state_json() would say something else.
uint32_t gameid_state_seq(void);
