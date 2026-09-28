// The SVS Bridge's report (POST /api/svs, docs/SVS.md) with no I/O: parsing it, and writing the switch
// it describes as JSON. svs.c keeps it; tests/test_svs.c checks these on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVS_NAME_MAX    32
#define SVS_KIND_MAX    11
#define SVS_FW_MAX      23
#define SVS_INPUTS_MAX  32 // the SVS takes up to 32 inputs
#define SVS_OUTPUTS_MAX 6

// An input's settings, as the bridge read them from the SVS's EEPROM (a bit each).
#define SVS_F_AUTO_PROFILE 0x01 // the SVS sends its profile (and IR codes) when this input is picked
#define SVS_F_RGSB         0x02 // RGBS converted to RGsB (V3 SCART and VGA modules)
#define SVS_F_SYNC_BYPASS  0x04 // sync bypass (V3 SCART)
#define SVS_F_RGB_TO_YPBPR 0x08 // through the RGB -> YPbPr transcoder
#define SVS_F_YPBPR_TO_RGB 0x10 // through the YPbPr -> RGB transcoder
#define SVS_F_V3           0x20 // a V3 module, which identifies itself
#define SVS_F_COUNT        6

// The flags' JSON names, in bit order.
extern const char *const svs_flag_names[SVS_F_COUNT];

typedef struct {
    char kind[SVS_KIND_MAX + 1]; // the module: "scart", "component", "vga", "svideo", "dterm", "bnc"; "" if not said
    char name[SVS_NAME_MAX + 1]; // as the user named it on the bridge ("Super Nintendo"; "" if not)
    uint8_t flags;               // SVS_F_*
    uint8_t known;               // which of the flags the bridge said (it only knows them after reading the SVS)
} svs_port_t;

// The switch as its bridge describes it: what the SVS can't report itself (the modules, the names),
// and the settings the bridge read from it.
typedef struct {
    char firmware[SVS_FW_MAX + 1]; // the SVS's firmware ("SVS_FW_1.21"; "" if not seen)
    int inputs_n, outputs_n;       // described so far (0: no layout set up on the bridge)
    svs_port_t inputs[SVS_INPUTS_MAX];
    svs_port_t outputs[SVS_OUTPUTS_MAX];
} svs_switch_t;

typedef struct {
    int input;                   // the switch's port (1-based)
    int total;                   // how many inputs the switch has (0: not said)
    char name[SVS_NAME_MAX + 1]; // the port's name ("name", "current_input_name", else the layout's)
    char id[SVS_NAME_MAX + 1];   // the bridge ("" if not said)
    bool has_switch;             // the report describes the switch ("firmware", "inputs", "outputs")
    svs_switch_t sw;             // then: that description (what it leaves out is empty)
} svs_msg_t;

// Parses a report: {"id", "current_input" (or "input"), "total_inputs", "name", "firmware",
// "inputs": [{"kind", "name", "auto_profile", ...}], "outputs": [{"kind", "name"}]}. Unknown keys are
// skipped. False, with *error, on anything that isn't a JSON object with a port number.
bool svs_parse(const char *json, svs_msg_t *out, const char **error);

// Writes sw as {"firmware","inputs":[...],"outputs":[...]} (the flags a port doesn't know left out).
// Returns the length, or 0 if it doesn't fit.
size_t svs_switch_json(const svs_switch_t *sw, char *out, size_t size);

// JSON-escapes in (quotes, backslashes, control characters) into out, always terminated.
void svs_json_escape(char *out, size_t size, const char *in);
