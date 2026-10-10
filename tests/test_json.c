// Host unit tests for the JSON reader (src/core/json.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "json.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)
#define RUN(t) do { current = #t; t(); } while (0)

static json_tok_t tok[64];
static const char *text;
static int parse(const char *s) {
    text = s;
    return json_parse(s, strlen(s), tok, 64);
}

static void test_values(void) {
    CHECK(parse("{\"a\": 1, \"b\": \"x\", \"c\": true, \"d\": null, \"e\": [1, 2, {\"f\": false}]}") > 0);
    long v = 0;
    bool b = true;
    char s[16];
    CHECK(json_long(text, tok, json_get(text, tok, 0, "a"), &v) && v == 1);
    CHECK(json_str(text, tok, json_get(text, tok, 0, "b"), s, sizeof(s)) && !strcmp(s, "x"));
    CHECK(json_bool(text, tok, json_get(text, tok, 0, "c"), &b) && b);
    const int d = json_get(text, tok, 0, "d");
    CHECK(d > 0 && !json_bool(text, tok, d, &b) && !json_long(text, tok, d, &v));
    const int e = json_get(text, tok, 0, "e");
    CHECK(tok[e].type == JSON_ARRAY && tok[e].size == 3);
    CHECK(json_long(text, tok, json_at(tok, e, 1), &v) && v == 2);
    const int f = json_at(tok, e, 2);
    CHECK(tok[f].type == JSON_OBJECT && json_bool(text, tok, json_get(text, tok, f, "f"), &b) && !b);
    CHECK(json_get(text, tok, 0, "z") < 0 && json_at(tok, e, 3) < 0 && json_at(tok, 0, 0) < 0);
    // a key after a nested value is still found (next skips the subtree)
    CHECK(parse("{\"x\": {\"y\": [1, [2, 3]]}, \"z\": -42}") > 0 && json_long(text, tok, json_get(text, tok, 0, "z"), &v) && v == -42);
    CHECK(parse("  [ ]  ") == 1 && tok[0].type == JSON_ARRAY && tok[0].size == 0);
    CHECK(parse("{}") == 1);
}

static void test_strings(void) {
    char s[32];
    CHECK(parse("\"a\\\"b\\\\c\\/d\\n\"") == 1 && json_str(text, tok, 0, s, sizeof(s)) && !strcmp(s, "a\"b\\c/d\n"));
    CHECK(parse("\"\\u00e9\\u20ac\"") == 1 && json_str(text, tok, 0, s, sizeof(s)) && !strcmp(s, "\xc3\xa9\xe2\x82\xac")); // é €
    CHECK(parse("\"\\ud83c\\udfae\"") == 1 && json_str(text, tok, 0, s, sizeof(s)) && !strcmp(s, "\xf0\x9f\x8e\xae")); // 🎮
    CHECK(parse("\"HDMI\xc2\xae\"") == 1 && json_str(text, tok, 0, s, sizeof(s)) && !strcmp(s, "HDMI\xc2\xae")); // UTF-8 as is
    CHECK(parse("\"\\u0000\"") == 1 && !json_str(text, tok, 0, s, sizeof(s)));                                 // no 0 inside
    CHECK(parse("\"abcdef\"") == 1 && !json_str(text, tok, 0, s, 6) && json_str(text, tok, 0, s, 7));          // fits or not
    char e[32];
    CHECK(json_escape_str(e, sizeof(e), "a\"b\\\n\x01") == 14 &&!strcmp(e, "a\\\"b\\\\\\n\\u0001"));
    CHECK(json_escape_str(e, 4, "abcd") < 0 && json_escape_str(e, 5, "abcd") == 4);
}

static void test_numbers(void) {
    long v;
    CHECK(parse("[0, -7, 2147483647, 2147483648, 1.5, 1e3]") > 0);
    CHECK(json_long(text, tok, json_at(tok, 0, 0), &v) && v == 0);
    CHECK(json_long(text, tok, json_at(tok, 0, 1), &v) && v == -7);
    CHECK(json_long(text, tok, json_at(tok, 0, 2), &v) && v == 2147483647L);
    CHECK(!json_long(text, tok, json_at(tok, 0, 3), &v)); // past 32 bits
    CHECK(!json_long(text, tok, json_at(tok, 0, 4), &v) && !json_long(text, tok, json_at(tok, 0, 5), &v)); // not whole numbers as written
}

static void test_rejects(void) {
    const char *bad[] = {"", "{", "[1,]", "{\"a\" 1}", "{\"a\":1,}", "{a:1}", "\"x", "\"\\x\"", "\"\\u12\"", "tru", "[1] 2",
        "\"a\nb\"", "{\"a\":}", "[,1]", "-", "nope"};
    for (size_t k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
        if (parse(bad[k]) >= 0) printf("  accepted: %s\n", bad[k]);
        CHECK(parse(bad[k]) < 0);
    }
    // too many tokens for the room given
    text = "[1,2,3,4]";
    CHECK(json_parse(text, strlen(text), tok, 4) < 0 && json_parse(text, strlen(text), tok, 5) == 5);
    // too deep
    char deep[80];
    memset(deep, '[', 20);
    memset(deep + 20, ']', 20);
    deep[40] = 0;
    CHECK(parse(deep) < 0);
}

int main(void) {
    RUN(test_values);
    RUN(test_strings);
    RUN(test_numbers);
    RUN(test_rejects);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
