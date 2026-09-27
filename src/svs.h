// The Scalable Video Switch's active input, as its board reports it (POST /api/svs; see docs/SVS.md).
// gameID uses it to know which console is on screen.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SVS_NAME_MAX 32

typedef struct {
    int input;                   // the switch's port (1-based)
    char name[SVS_NAME_MAX + 1]; // the port's name, as the board calls it ("" if not given)
    char id[SVS_NAME_MAX + 1];   // the board that said so
    uint32_t at_ms;              // when it last said so (repeats included)
    uint32_t changed_ms;         // when the input last changed
} svs_state_t;

// Records a report; true if the input changed. Repeats (the board resends every 60 s) only refresh at_ms.
bool svs_report(int input, const char *name, const char *id);

// The last report; false if none since boot.
bool svs_get(svs_state_t *out);
