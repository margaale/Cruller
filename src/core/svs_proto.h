// The SVS Bridge's report (POST /api/svs, docs/SVS.md) with no I/O: parsing it, and writing the switch
// it describes as JSON. svs.c keeps it; tests/test_svs.c checks these on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVS_NAME_MAX   32
#define SVS_KIND_MAX   11
#define SVS_DEVICE_MAX 15
#define SVS_INPUTS_MAX 32 // the SVS takes up to 32 inputs

typedef struct {
    char kind[SVS_KIND_MAX + 1]; // the module: "scart", "component", "vga", "svideo", "dterm", "bnc"; "" if not said
    char name[SVS_NAME_MAX + 1]; // what is connected to it, by name ("Super Nintendo / Super Famicom"; "" if not said)
    char device[SVS_DEVICE_MAX + 1]; // ... and its id in the bridge's list ("snes", "rt4k"; "" if none picked)
} svs_port_t;

// The switch as its bridge describes it (what the SVS can't report itself): each input's module and
// what is connected to it, and the output that goes to the RetroTINK.
typedef struct {
    int inputs_n; // described so far (0: no layout set up on the bridge)
    svs_port_t inputs[SVS_INPUTS_MAX];
    bool has_output; // the bridge said which output goes to the RetroTINK
    svs_port_t output;
} svs_switch_t;

typedef struct {
    int input;                   // the switch's port (1-based)
    int total;                   // how many inputs the switch has (0: not said)
    char name[SVS_NAME_MAX + 1]; // the port's name ("name", "current_input_name", else the layout's)
    char id[SVS_NAME_MAX + 1];   // the bridge ("" if not said)
    bool has_switch;             // the report describes the switch ("inputs", "output")
    svs_switch_t sw;             // then: that description (what it leaves out is empty)
} svs_msg_t;

// Parses a report: {"id", "current_input" (or "input"), "total_inputs", "name",
// "inputs": [{"kind", "name", "device"}, ...], "output": {"kind", "name", "device"}}. Unknown keys are skipped. False,
// with *error, on anything that isn't a JSON object with a port number.
bool svs_parse(const char *json, svs_msg_t *out, const char **error);

// Writes sw as {"inputs":[{"kind","name","device"}...],"output":{"kind","name","device"}|null}. Returns the length, or
// 0 if it doesn't fit.
size_t svs_switch_json(const svs_switch_t *sw, char *out, size_t size);

// JSON-escapes in (quotes, backslashes, control characters) into out, always terminated.
void svs_json_escape(char *out, size_t size, const char *in);
