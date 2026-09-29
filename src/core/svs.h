// The Scalable Video Switch's active input, and the switch itself, as its bridge reports them (POST
// /api/svs; see docs/SVS.md). gameID uses them to know which console is on screen; the page's SVS tab
// shows them.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "svs_proto.h"

#define SVS_HISTORY  10

typedef struct {
    int input;                   // the switch's port (1-based; 0: none active)
    int total;                   // how many inputs the switch has (0: not said)
    char name[SVS_NAME_MAX + 1]; // the port's name, as the bridge calls it ("" if not given)
    char id[SVS_NAME_MAX + 1];   // the bridge that said so
    uint32_t at_ms;              // when it last said so (repeats included)
    uint32_t changed_ms;         // when the input last changed
    uint32_t switch_seq;         // changes when the switch's description does (0: none yet)
    int history_n;               // the last input changes, newest first
    struct { int input; uint32_t at_ms; } history[SVS_HISTORY];
} svs_state_t;

// Records a report; true if the input changed. Repeats (the bridge resends every 60 s) only refresh
// at_ms. A report that describes the switch (m->has_switch) replaces the description kept.
bool svs_report(const svs_msg_t *m);

// The last report; false if none since boot.
bool svs_get(svs_state_t *out);

// The switch as last described; its switch_seq (0, and *out left alone, if none since boot).
uint32_t svs_get_switch(svs_switch_t *out);

// Changes with every input change and every new description (the WebSocket pushes the status then).
uint32_t svs_version(void);
