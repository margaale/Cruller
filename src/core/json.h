// A small JSON reader: the text split into tokens (objects, arrays, strings, the rest), then values
// looked up by key or position and decoded. For the request bodies of the API and the replies of the
// consoles gameID asks (src/core/gameid*.c); it doesn't allocate and doesn't change the text.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE } json_type_t;

typedef struct {
    uint8_t type;   // json_type_t
    uint32_t start; // a string's: inside its quotes; the rest: its first byte
    uint32_t end;   // one past its last byte (a string's: its closing quote)
    uint16_t size;  // an object's keys, an array's values
    uint16_t next;  // the token after it and everything in it
} json_tok_t;

// Splits text[0..len) (one value, spaces around it allowed) into at most max tokens, in order: a
// container, then its members (an object's alternate key, value). The count, or -1 when the text
// isn't JSON or needs more tokens.
int json_parse(const char *text, size_t len, json_tok_t *tok, int max);

// An object's value for key (its token's index), or -1.
int json_get(const char *text, const json_tok_t *tok, int obj, const char *key);

// An array's k-th value (from 0), or -1.
int json_at(const json_tok_t *tok, int arr, int k);

// A string token decoded (escapes, \u as UTF-8) into out, 0-terminated. False when it isn't a string,
// doesn't fit, or holds a 0.
bool json_str(const char *text, const json_tok_t *tok, int i, char *out, size_t size);

// A whole number (true, false and null aren't), within long's range.
bool json_long(const char *text, const json_tok_t *tok, int i, long *out);

bool json_bool(const char *text, const json_tok_t *tok, int i, bool *out);

// s written as a JSON string's inside (quotes, backslashes and control characters escaped) into out,
// 0-terminated; the length, or -1 when it doesn't fit.
int json_escape_str(char *out, size_t size, const char *s);
