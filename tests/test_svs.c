// Host unit tests for the SVS Bridge's report (src/core/svs_proto.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "svs_proto.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)

static svs_msg_t m;
static const char *err;

static bool parse(const char *json) {
    err = NULL;
    return svs_parse(json, &m, &err);
}

// --- the input -------------------------------------------------------------------------------------

static void test_input_only(void) {
    CHECK(parse("{\"id\": \"svs-bridge-aabbccddeeff\", \"current_input\": 3, \"total_inputs\": 8, \"live\": true}"));
    CHECK(m.input == 3 && m.total == 8);
    CHECK(!strcmp(m.id, "svs-bridge-aabbccddeeff"));
    CHECK(!strcmp(m.name, ""));
    CHECK(!m.has_switch);
    CHECK(parse("{\"input\":1}") && m.input == 1 && m.total == 0 && !m.id[0]);
    // 0: no input active (and no name from the layout for it).
    CHECK(parse("{\"current_input\":0,\"inputs\":[{\"name\":\"A\"}]}") && m.input == 0 && !m.name[0]);
}

static void test_rejects(void) {
    CHECK(!parse("") && err);
    CHECK(!parse("[1]") && err);
    CHECK(!parse("{\"total_inputs\": 8}") && strstr(err, "current_input"));
    CHECK(!parse("{\"current_input\": null}"));
    CHECK(!parse("{\"current_input\": -1}"));
    CHECK(!parse("{\"current_input\": 3, \"inputs\": [{\"kind\": \"scart\"}"));  // cut short
    CHECK(!parse("{\"current_input\": 3, \"name\": \"PS2}"));
    CHECK(!parse("{\"current_input\": 3, \"x\": [[[[[[[[[[1]]]]]]]]]]}")); // too deep
}

static void test_skips_unknown(void) {
    CHECK(parse("{\"future\": {\"a\": [1, {\"b\": \"x\\\"y\"}], \"c\": null}, \"current_input\": 2, \"n\": -1.5e3, \"t\": false}"));
    CHECK(m.input == 2);
    // A "name" inside something else is not the port's name.
    CHECK(parse("{\"other\": {\"name\": \"no\"}, \"current_input\": 2}") && !m.name[0]);
}

// --- the switch ------------------------------------------------------------------------------------

static const char *full =
    "{\"id\":\"svs-bridge-aabbccddeeff\",\"current_input\":2,\"total_inputs\":4,\"live\":true,"
    "\"inputs\":[{\"kind\":\"scart\",\"name\":\"Super Nintendo\",\"extra\":[1,{\"a\":true}]},"
    "{\"kind\":\"component\",\"name\":\"PS2\"},"
    "{\"kind\":\"VGA\",\"name\":\"Dreamcast\"},"
    "{\"kind\":\"svideo\",\"name\":\"\"}],"
    "\"output\":{\"kind\":\"component\",\"name\":\"RetroTINK 4K\"}}";

static void test_switch(void) {
    CHECK(parse(full));
    CHECK(m.has_switch);
    CHECK(m.sw.inputs_n == 4);
    CHECK(!strcmp(m.sw.inputs[0].kind, "scart") && !strcmp(m.sw.inputs[0].name, "Super Nintendo"));
    CHECK(!strcmp(m.sw.inputs[2].kind, "vga"));   // lowercased
    CHECK(!strcmp(m.sw.inputs[3].name, ""));
    CHECK(m.sw.has_output && !strcmp(m.sw.output.kind, "component") && !strcmp(m.sw.output.name, "RetroTINK 4K"));
    // The active port's name comes from the layout when the report doesn't say it.
    CHECK(!strcmp(m.name, "PS2"));
    // Inputs without the output (a bridge that doesn't know which one goes to the RetroTINK).
    CHECK(parse("{\"current_input\":1,\"inputs\":[{\"kind\":\"scart\",\"name\":\"SNES\"}]}"));
    CHECK(m.has_switch && m.sw.inputs_n == 1 && !m.sw.has_output);
    CHECK(parse("{\"current_input\":1,\"output\":null}") && m.has_switch && !m.sw.has_output);
    CHECK(!parse("{\"current_input\":1,\"output\":[1]}"));
}

static void test_name_sources(void) {
    CHECK(parse("{\"current_input\":1,\"name\":\"A\",\"current_input_name\":\"B\",\"inputs\":[{\"name\":\"C\"}]}"));
    CHECK(!strcmp(m.name, "A"));
    CHECK(parse("{\"current_input\":1,\"current_input_name\":\"B\",\"inputs\":[{\"name\":\"C\"}]}"));
    CHECK(!strcmp(m.name, "B"));
    CHECK(parse("{\"current_input\":5,\"inputs\":[{\"name\":\"C\"}]}"));  // past the layout
    CHECK(!strcmp(m.name, ""));
}

static void test_empty_layout(void) {
    // A bridge with no layout set up says so: an empty list replaces what Cruller had.
    CHECK(parse("{\"current_input\":1,\"inputs\":[]}"));
    CHECK(m.has_switch && m.sw.inputs_n == 0 && !m.sw.has_output);
}

static void test_limits(void) {
    char json[4096];
    size_t o = (size_t)snprintf(json, sizeof(json), "{\"current_input\":1,\"inputs\":[");
    for (int i = 0; i < 40; i++) o += (size_t)snprintf(json + o, sizeof(json) - o, "%s{\"kind\":\"scart\"}", i ? "," : "");
    snprintf(json + o, sizeof(json) - o, "],\"total_inputs\":32}");
    CHECK(parse(json));
    CHECK(m.sw.inputs_n == SVS_INPUTS_MAX && m.total == 32);

    // Names are cut to fit, never in the middle of a character.
    CHECK(parse("{\"current_input\":1,\"name\":\"0123456789012345678901234567890\xc3\xa9zz\"}"));
    CHECK(strlen(m.name) == 31);
    CHECK(parse("{\"current_input\":1,\"name\":\"012345678901234567890123456789\xc3\xa9zz\"}"));
    CHECK(strlen(m.name) == 32 && !strcmp(m.name + 30, "\xc3\xa9"));
    // Kinds that aren't a word are dropped; control characters in names become spaces.
    CHECK(parse("{\"current_input\":1,\"inputs\":[{\"kind\":\"sc art\",\"name\":\"a\\nb\"}]}"));
    CHECK(!strcmp(m.sw.inputs[0].kind, "") && !strcmp(m.sw.inputs[0].name, "a b"));
}

static void test_escapes(void) {
    CHECK(parse("{\"current_input\":1,\"name\":\"Mega \\u00e9 \\\"Drive\\\" \\/ \\u20ac\"}"));
    CHECK(!strcmp(m.name, "Mega \xc3\xa9 \"Drive\" / \xe2\x82\xac"));
    CHECK(!parse("{\"current_input\":1,\"name\":\"\\u12\"}"));
    CHECK(!parse("{\"current_input\":1,\"name\":\"\\u+12a\"}"));
    CHECK(parse("{\"current_input\":1,\"name\":\"\\u00C9\"}") && !strcmp(m.name, "\xc3\x89"));
}

// --- writing ---------------------------------------------------------------------------------------

static void test_json_out(void) {
    CHECK(parse(full));
    char out[2048];
    const size_t n = svs_switch_json(&m.sw, out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(!strcmp(out, "{\"inputs\":[{\"kind\":\"scart\",\"name\":\"Super Nintendo\"},{\"kind\":\"component\",\"name\":\"PS2\"},"
        "{\"kind\":\"vga\",\"name\":\"Dreamcast\"},{\"kind\":\"svideo\",\"name\":\"\"}],"
        "\"output\":{\"kind\":\"component\",\"name\":\"RetroTINK 4K\"}}"));

    // What Cruller writes, it reads back the same.
    svs_switch_t before = m.sw;
    char again[2100];
    snprintf(again, sizeof(again), "{\"current_input\":1,%s", out + 1);
    CHECK(parse(again));
    CHECK(!memcmp(&before, &m.sw, sizeof(before)));

    // Too small: 0, not half a JSON.
    CHECK(svs_switch_json(&m.sw, out, 40) == 0);

    svs_switch_t none;
    memset(&none, 0, sizeof(none));
    CHECK(svs_switch_json(&none, out, sizeof(out)) && !strcmp(out, "{\"inputs\":[],\"output\":null}"));

    snprintf(none.inputs[0].name, sizeof(none.inputs[0].name), "a\"b\\c");
    none.inputs_n = 1;
    CHECK(svs_switch_json(&none, out, sizeof(out)) && strstr(out, "\"name\":\"a\\\"b\\\\c\""));
}

#define RUN(t) do { current = #t; t(); } while (0)

int main(void) {
    RUN(test_input_only);
    RUN(test_rejects);
    RUN(test_skips_unknown);
    RUN(test_switch);
    RUN(test_name_sources);
    RUN(test_empty_layout);
    RUN(test_limits);
    RUN(test_escapes);
    RUN(test_json_out);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
