// The Scalable Video Switch's active input, as its bridge reports it (POST /api/svs; see docs/SVS.md).
// gameID uses it to know which console is on screen; the page's SVS tab shows it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SVS_NAME_MAX 32
#define SVS_HISTORY  10

typedef struct {
    int input;                   // the switch's port (1-based)
    int total;                   // how many inputs the switch has (0: not said)
    char name[SVS_NAME_MAX + 1]; // the port's name, as the bridge calls it ("" if not given)
    char id[SVS_NAME_MAX + 1];   // the bridge that said so
    uint32_t at_ms;              // when it last said so (repeats included)
    uint32_t changed_ms;         // when the input last changed
    int history_n;               // the last input changes, newest first
    struct { int input; uint32_t at_ms; } history[SVS_HISTORY];
} svs_state_t;

// Records a report (total 0: not given); true if the input changed. Repeats (the bridge resends every
// 60 s) only refresh at_ms.
bool svs_report(int input, int total, const char *name, const char *id);

// The last report; false if none since boot.
bool svs_get(svs_state_t *out);

// Changes with every input change (the WebSocket pushes the status then).
uint32_t svs_version(void);
