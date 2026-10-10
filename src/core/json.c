#include "json.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t len, at;
    json_tok_t *tok;
    int max, n;
} parser_t;

static void skip_space(parser_t *p) {
    while (p->at < p->len && (p->s[p->at] == ' ' || p->s[p->at] == '\t' || p->s[p->at] == '\n' || p->s[p->at] == '\r')) p->at++;
}

static int add(parser_t *p, json_type_t type, size_t start) {
    if (p->n >= p->max || p->n >= 0xffff) return -1;
    p->tok[p->n] = (json_tok_t){.type = (uint8_t)type, .start = (uint32_t)start};
    return p->n++;
}

static bool hex4(const char *s) {
    for (int k = 0; k < 4; k++) {
        const char c = s[k];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool string(parser_t *p) {
    const int i = add(p, JSON_STRING, ++p->at);
    if (i < 0) return false;
    while (p->at < p->len) {
        const unsigned char c = (unsigned char)p->s[p->at];
        if (c == '"') {
            p->tok[i].end = (uint32_t)p->at++;
            p->tok[i].next = (uint16_t)p->n;
            return true;
        }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (++p->at >= p->len) return false;
            const char e = p->s[p->at];
            if (e == 'u') {
                if (p->at + 4 >= p->len || !hex4(p->s + p->at + 1)) return false;
                p->at += 4;
            } else if (!strchr("\"\\/bfnrt", e)) {
                return false;
            }
        }
        p->at++;
    }
    return false;
}

static bool primitive(parser_t *p) {
    const size_t start = p->at;
    while (p->at < p->len && strchr("+-.0123456789eEtruefalsn", p->s[p->at])) p->at++;
    const size_t n = p->at - start;
    const char *v = p->s + start;
    const bool word = (n == 4 && (!memcmp(v, "true", 4) || !memcmp(v, "null", 4))) || (n == 5 && !memcmp(v, "false", 5));
    const char *d = n && v[0] == '-' ? v + 1 : v; // a number: a digit first, after any sign
    if (!n || (!word && !(d < v + n && *d >= '0' && *d <= '9'))) return false;
    const int i = add(p, JSON_PRIMITIVE, start);
    if (i < 0) return false;
    p->tok[i].end = (uint32_t)p->at;
    p->tok[i].next = (uint16_t)p->n;
    return true;
}

static bool value(parser_t *p, int depth);

static bool container(parser_t *p, int depth, bool object) {
    const int i = add(p, object ? JSON_OBJECT : JSON_ARRAY, p->at++);
    if (i < 0) return false;
    const char close = object ? '}' : ']';
    skip_space(p);
    if (p->at < p->len && p->s[p->at] == close) {
        p->tok[i].end = (uint32_t)++p->at;
        p->tok[i].next = (uint16_t)p->n;
        return true;
    }
    for (;;) {
        skip_space(p);
        if (object) {
            if (p->at >= p->len || p->s[p->at] != '"' || !string(p)) return false;
            skip_space(p);
            if (p->at >= p->len || p->s[p->at++] != ':') return false;
        }
        if (!value(p, depth + 1)) return false;
        p->tok[i].size++;
        skip_space(p);
        if (p->at >= p->len) return false;
        const char c = p->s[p->at++];
        if (c == close) break;
        if (c != ',') return false;
    }
    p->tok[i].end = (uint32_t)p->at;
    p->tok[i].next = (uint16_t)p->n;
    return true;
}

static bool value(parser_t *p, int depth) {
    if (depth > 16) return false;
    skip_space(p);
    if (p->at >= p->len) return false;
    const char c = p->s[p->at];
    if (c == '{' || c == '[') return container(p, depth, c == '{');
    if (c == '"') return string(p);
    return primitive(p);
}

int json_parse(const char *text, size_t len, json_tok_t *tok, int max) {
    parser_t p = {text, len, 0, tok, max, 0};
    if (!value(&p, 0)) return -1;
    skip_space(&p);
    return p.at == len ? p.n : -1;
}

static bool same(const char *text, const json_tok_t *t, const char *s) {
    const size_t n = strlen(s);
    return t->type == JSON_STRING && t->end - t->start == n && !memcmp(text + t->start, s, n); // (keys compared as written)
}

int json_get(const char *text, const json_tok_t *tok, int obj, const char *key) {
    if (obj < 0 || tok[obj].type != JSON_OBJECT) return -1;
    int k = obj + 1;
    for (int m = 0; m < tok[obj].size; m++) {
        const int v = k + 1;
        if (same(text, &tok[k], key)) return v;
        k = tok[v].next;
    }
    return -1;
}

int json_at(const json_tok_t *tok, int arr, int k) {
    if (arr < 0 || tok[arr].type != JSON_ARRAY || k < 0 || k >= tok[arr].size) return -1;
    int i = arr + 1;
    while (k--) i = tok[i].next;
    return i;
}

static int hexval(char c) {
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

bool json_str(const char *text, const json_tok_t *tok, int i, char *out, size_t size) {
    if (i < 0 || tok[i].type != JSON_STRING || !size) return false;
    size_t n = 0;
    const char *s = text + tok[i].start, *end = text + tok[i].end;
    while (s < end) {
        uint32_t cp = (unsigned char)*s++;
        if (cp >= 0x80) { // UTF-8 as written
            if (n + 1 >= size) return false;
            out[n++] = (char)cp;
            continue;
        }
        if (cp == '\\') {
            const char e = *s++;
            switch (e) {
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u':
                    cp = (uint32_t)(hexval(s[0]) << 12 | hexval(s[1]) << 8 | hexval(s[2]) << 4 | hexval(s[3]));
                    s += 4;
                    if (cp >= 0xd800 && cp < 0xdc00 && end - s >= 6 && s[0] == '\\' && s[1] == 'u' && hex4(s + 2)) {
                        const uint32_t lo = (uint32_t)(hexval(s[2]) << 12 | hexval(s[3]) << 8 | hexval(s[4]) << 4 | hexval(s[5]));
                        if (lo >= 0xdc00 && lo < 0xe000) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            s += 6;
                        }
                    }
                    break;
                default: cp = (unsigned char)e; break; // \" \\ \/
            }
        }
        if (!cp) return false;
        char u[4];
        size_t k;
        if (cp < 0x80) { u[0] = (char)cp; k = 1; }
        else if (cp < 0x800) { u[0] = (char)(0xc0 | cp >> 6); u[1] = (char)(0x80 | (cp & 0x3f)); k = 2; }
        else if (cp < 0x10000) { u[0] = (char)(0xe0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); u[2] = (char)(0x80 | (cp & 0x3f)); k = 3; }
        else { u[0] = (char)(0xf0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); u[3] = (char)(0x80 | (cp & 0x3f)); k = 4; }
        if (n + k >= size) return false;
        memcpy(out + n, u, k);
        n += k;
    }
    out[n] = 0;
    return true;
}

bool json_long(const char *text, const json_tok_t *tok, int i, long *out) {
    if (i < 0 || tok[i].type != JSON_PRIMITIVE) return false;
    const char *s = text + tok[i].start, *end = text + tok[i].end;
    const bool neg = *s == '-';
    if (neg) s++;
    if (s == end) return false;
    long v = 0;
    for (; s < end; s++) {
        if (*s < '0' || *s > '9') return false;
        if (v > (0x7fffffffL - (*s - '0')) / 10) return false; // (kept within 32 bits on every target)
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return true;
}

bool json_bool(const char *text, const json_tok_t *tok, int i, bool *out) {
    if (i < 0 || tok[i].type != JSON_PRIMITIVE) return false;
    const size_t n = tok[i].end - tok[i].start;
    if (n == 4 && !memcmp(text + tok[i].start, "true", 4)) { *out = true; return true; }
    if (n == 5 && !memcmp(text + tok[i].start, "false", 5)) { *out = false; return true; }
    return false;
}

int json_escape_str(char *out, size_t size, const char *s) {
    size_t n = 0;
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        char esc[8];
        size_t k;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; k = 2; }
        else if (c == '\n') { memcpy(esc, "\\n", 2); k = 2; }
        else if (c == '\r') { memcpy(esc, "\\r", 2); k = 2; }
        else if (c == '\t') { memcpy(esc, "\\t", 2); k = 2; }
        else if (c < 0x20) { k = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", c); }
        else { esc[0] = (char)c; k = 1; }
        if (n + k >= size) return -1;
        memcpy(out + n, esc, k);
        n += k;
    }
    if (n >= size) return -1;
    out[n] = 0;
    return (int)n;
}
