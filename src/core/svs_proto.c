#include "svs_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- a small JSON reader: enough for the report, nested arrays and objects included ------------------

#define DEPTH_MAX 8

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// The string at p (which must be a '"') into out (NULL: just skip it), cut to fit without splitting a
// UTF-8 character. Returns the position after it, or NULL.
static const char *str(const char *p, char *out, size_t size) {
    if (*p != '"') return NULL;
    size_t n = 0;
    bool full = !out || !size; // nothing more goes in
    for (p++; *p != '"'; p++) {
        if (!*p) return NULL;
        unsigned c = (unsigned char)*p;
        bool raw = true; // a byte as it came (else a \u code point, to encode)
        if (c == '\\') {
            c = (unsigned char)*++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'u') {
                c = 0;
                for (int i = 0; i < 4; i++) {
                    const char h = *++p;
                    if (h >= '0' && h <= '9') c = c << 4 | (unsigned)(h - '0');
                    else if ((h | 0x20) >= 'a' && (h | 0x20) <= 'f') c = c << 4 | (unsigned)((h | 0x20) - 'a' + 10);
                    else return NULL;
                }
                if (c >= 0xD800 && c < 0xE000) c = '?'; // outside the BMP: not in a port name
                raw = false;
            } else if (!c) return NULL;
        }
        if (full) continue;
        char b[3];
        size_t len = 1;
        if (raw) {
            b[0] = (char)c;
            // A UTF-8 lead byte goes in only with room for its whole character (then its
            // continuation bytes fit too).
            const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (n + need >= size) { full = true; continue; }
        } else if (c < 0x80) {
            b[0] = (char)c;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); len = 2;
        } else {
            b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); len = 3;
        }
        if (n + len >= size) { full = true; continue; }
        memcpy(out + n, b, len);
        n += len;
    }
    if (out && size) out[n] = 0;
    return p + 1;
}

static const char *skip(const char *p, int depth);

// The next member of the object being read: its key into key, positioned at its value. False at the
// end (*pp after the '}') or on an error (*pp NULL).
static bool member(const char **pp, char *key, size_t size) {
    const char *p = ws(*pp);
    if (*p == ',') p = ws(p + 1);
    if (*p == '}') { *pp = p + 1; return false; }
    if (!(p = str(p, key, size)) || *(p = ws(p)) != ':') { *pp = NULL; return false; }
    *pp = ws(p + 1);
    return true;
}

// Likewise for the elements of an array.
static bool element(const char **pp) {
    const char *p = ws(*pp);
    if (*p == ',') p = ws(p + 1);
    if (*p == ']') { *pp = p + 1; return false; }
    if (!*p) { *pp = NULL; return false; }
    *pp = p;
    return true;
}

// Past the value at p (any kind). NULL on an error.
static const char *skip(const char *p, int depth) {
    p = ws(p);
    if (*p == '"') return str(p, NULL, 0);
    char key[4];
    if (*p == '{' || *p == '[') {
        if (depth >= DEPTH_MAX) return NULL;
        const bool obj = *p == '{';
        p++;
        while (obj ? member(&p, key, sizeof(key)) : element(&p)) {
            if (!(p = skip(p, depth + 1))) return NULL;
        }
        return p;
    }
    const char *start = p;
    while (*p && strchr("+-.0123456789eEtrufalsn", *p)) p++;
    return p > start ? p : NULL;
}

static const char *number(const char *p, long *v) {
    char *end;
    *v = strtol(p, &end, 10);
    return end == p ? NULL : skip(p, 0); // (past a fraction or exponent too)
}

// A string value, or NULL (as JSON null: left empty).
static const char *text(const char *p, char *out, size_t size) {
    out[0] = 0;
    if (!strncmp(p, "null", 4)) return p + 4;
    return str(p, out, size);
}

// Keeps a kind to lowercase letters and digits ("SCART" -> "scart"); anything else makes it "".
static void clean_kind(char *k) {
    for (char *c = k; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        else if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) { k[0] = 0; return; }
    }
}

// Control characters become spaces: a name is one line.
static void clean_name(char *n) {
    for (; *n; n++) if ((unsigned char)*n < 0x20 || *n == 0x7F) *n = ' ';
}

// {"kind","name"} into port (other keys read past).
static const char *port(const char *p, svs_port_t *port) {
    memset(port, 0, sizeof(*port));
    if (*p != '{') return NULL;
    p++;
    char key[8];
    while (member(&p, key, sizeof(key))) {
        if (!strcmp(key, "kind")) p = text(p, port->kind, sizeof(port->kind));
        else if (!strcmp(key, "name")) p = text(p, port->name, sizeof(port->name));
        else p = skip(p, 2);
        if (!p) return NULL;
    }
    if (!p) return NULL;
    clean_kind(port->kind);
    clean_name(port->name);
    return p;
}

// [{"kind","name"}, ...] into ports (at most max; the rest are read past).
static const char *ports(const char *p, svs_port_t *ports, int max, int *count) {
    *count = 0;
    if (*p != '[') return NULL;
    p++;
    while (element(&p)) {
        svs_port_t one;
        if (!(p = port(p, &one))) return NULL;
        if (*count < max) ports[(*count)++] = one;
    }
    return p;
}

bool svs_parse(const char *json, svs_msg_t *out, const char **error) {
    memset(out, 0, sizeof(*out));
    out->input = -1;
    const char *p = ws(json);
    if (*p != '{') { *error = "send a JSON object"; return false; }
    p++;
    char key[24], name2[SVS_NAME_MAX + 1] = "";
    while (member(&p, key, sizeof(key))) {
        long v;
        if (!strcmp(key, "current_input") || !strcmp(key, "input")) {
            if (!strncmp(p, "null", 4)) p += 4;
            else if ((p = number(p, &v))) out->input = v >= 0 && v <= 255 ? (int)v : -1;
        } else if (!strcmp(key, "total_inputs")) {
            if (!strncmp(p, "null", 4)) p += 4;
            else if ((p = number(p, &v))) out->total = v > 0 && v <= 64 ? (int)v : 0;
        } else if (!strcmp(key, "name")) {
            p = text(p, out->name, sizeof(out->name));
        } else if (!strcmp(key, "current_input_name")) {
            p = text(p, name2, sizeof(name2));
        } else if (!strcmp(key, "id")) {
            p = text(p, out->id, sizeof(out->id));
        } else if (!strcmp(key, "inputs")) {
            p = ports(p, out->sw.inputs, SVS_INPUTS_MAX, &out->sw.inputs_n);
            out->has_switch = true;
        } else if (!strcmp(key, "output")) {
            if (!strncmp(p, "null", 4)) p += 4;
            else if ((p = port(p, &out->sw.output))) out->sw.has_output = true;
            out->has_switch = true;
        } else {
            p = skip(p, 1);
        }
        if (!p) break;
    }
    if (!p) { *error = "not valid JSON"; return false; }
    if (out->input < 0) { *error = "need \"current_input\": <port number>"; return false; }
    clean_name(out->id);
    // The active port's name: as said, else as the layout has it.
    if (!out->name[0]) snprintf(out->name, sizeof(out->name), "%s", name2);
    if (!out->name[0] && out->input >= 1 && out->input <= out->sw.inputs_n) {
        memcpy(out->name, out->sw.inputs[out->input - 1].name, sizeof(out->name));
    }
    clean_name(out->name);
    return true;
}

// --- writing --------------------------------------------------------------------------------------

void svs_json_escape(char *out, size_t size, const char *in) {
    size_t n = 0;
    if (!size) return;
    for (; *in && n + 7 < size; in++) {
        if (*in == '"' || *in == '\\') { out[n++] = '\\'; out[n++] = *in; }
        else if ((unsigned char)*in < 0x20) n += (size_t)snprintf(out + n, size - n, "\\u%04x", *in);
        else out[n++] = *in;
    }
    out[n] = 0;
}

#define ADD(...) do { o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__); } while (0)

static size_t port_json(const svs_port_t *port, char *out, size_t size) {
    size_t o = 0;
    char kind[2 * SVS_KIND_MAX + 8], name[2 * SVS_NAME_MAX + 8];
    svs_json_escape(kind, sizeof(kind), port->kind);
    svs_json_escape(name, sizeof(name), port->name);
    ADD("{\"kind\":\"%s\",\"name\":\"%s\"}", kind, name);
    return o;
}

size_t svs_switch_json(const svs_switch_t *sw, char *out, size_t size) {
    size_t o = 0;
    ADD("{\"inputs\":[");
    for (int i = 0; i < sw->inputs_n; i++) {
        if (i) ADD(",");
        if (o < size) o += port_json(&sw->inputs[i], out + o, size - o);
    }
    ADD("],\"output\":");
    if (!sw->has_output) ADD("null");
    else if (o < size) o += port_json(&sw->output, out + o, size - o);
    ADD("}");
    return o < size ? o : 0;
}
