#include "rt4k_info_core.h"

#include <string.h>

static struct {
    rt4k_info_t info;       // what's known
    rt4k_info_t kept;       // what was last handed to be kept
    bool fresh_version;     // said since the RT4K last came on
    bool fresh_model;
    int tries_version;      // questions asked since it came on
    int tries_model;
    bool on;                // as the last poll saw it
    const bool *waiting;    // the last question's answer (its fresh flag), NULL: none out
    uint32_t asked_ms;      // when it went out
    bool settled;           // on, and nothing left to ask: what it said can be kept (one write)
    uint32_t seq;
} s;

static void copy(char *out, size_t size, const char *from, size_t len) {
    if (len >= size) len = size - 1;
    memcpy(out, from, len);
    out[len] = 0;
}

static size_t bounded_len(const char *p, size_t size) {
    const char *end = memchr(p, 0, size);
    return end ? (size_t)(end - p) : size;
}

void rt4k_info_core_init(const rt4k_info_t *saved) {
    memset(&s, 0, sizeof(s));
    if (saved) {
        copy(s.info.version, sizeof(s.info.version), saved->version, bounded_len(saved->version, sizeof(saved->version)));
        copy(s.info.model, sizeof(s.info.model), saved->model, bounded_len(saved->model, sizeof(saved->model)));
    }
    s.kept = s.info;
}

static void learn(char *field, size_t size, bool *fresh, const char *value, size_t len) {
    char v[RT4K_INFO_MODEL_MAX + 1];
    copy(v, size < sizeof(v) ? size : sizeof(v), value, len);
    if (!v[0]) return;
    if (strcmp(field, v)) {
        strcpy(field, v);
        s.seq++;
    }
    if (!*fresh) {
        *fresh = true;
        s.seq++;
    }
}

void rt4k_info_core_line(const char *line) {
    if (strncmp(line, "[COM] ", 6)) return; // only command replies
    const char *l = line + 6;
    // "ver": "[COM] RT4KPRO, FW Version: 1.89.0" (then "[COM] Build tag: ...").
    const char *v = strstr(l, "FW Version:");
    if (v) {
        v += strlen("FW Version:");
        while (*v == ' ') v++;
        learn(s.info.version, sizeof(s.info.version), &s.fresh_version, v, strcspn(v, " ,"));
        return;
    }
    // "model": "[COM] model=<n> <name>"; the name is what follows the first word (as fw.js read it).
    if (!strncmp(l, "model=", 6)) {
        const char *sp = strchr(l, ' ');
        const char *m = sp ? sp : l + 6;
        while (*m == ' ') m++;
        size_t len = strlen(m);
        while (len && m[len - 1] == ' ') len--;
        learn(s.info.model, sizeof(s.info.model), &s.fresh_model, m, len);
    }
}

const char *rt4k_info_core_poll(bool on, uint32_t now_ms) {
    if (!on) {
        if (s.on && (s.fresh_version || s.fresh_model)) s.seq++; // what's known is from before now
        if (s.on) s.fresh_version = s.fresh_model = false;
        s.on = s.settled = false;
        return NULL;
    }
    if (!s.on) {
        s.on = true;
        s.waiting = NULL;
        s.tries_version = s.tries_model = 0;
    }
    // One question at a time: the next goes once this one is answered, or after a while.
    if (s.waiting && !*s.waiting && now_ms - s.asked_ms < RT4K_INFO_RETRY_MS) return NULL;
    s.waiting = NULL;
    s.settled = false;
    const char *ask = NULL;
    if (!s.fresh_version && s.tries_version < RT4K_INFO_TRIES) {
        s.tries_version++;
        s.waiting = &s.fresh_version;
        ask = "ver";
    } else if (!s.fresh_model && s.tries_model < RT4K_INFO_TRIES) {
        s.tries_model++;
        s.waiting = &s.fresh_model;
        ask = "model";
    }
    if (ask) s.asked_ms = now_ms;
    else s.settled = true;
    return ask;
}

bool rt4k_info_core_get(rt4k_info_t *out) {
    *out = s.info;
    return s.fresh_version && s.fresh_model;
}

bool rt4k_info_core_take_changed(rt4k_info_t *out) {
    // Kept only once the questions are over (a single flash write for both answers), and only when
    // it differs from what's kept: a restart, or the same firmware again, writes nothing.
    if (!s.settled) return false;
    if (!strcmp(s.info.version, s.kept.version) && !strcmp(s.info.model, s.kept.model)) return false;
    s.kept = s.info;
    *out = s.info;
    return true;
}

uint32_t rt4k_info_core_seq(void) {
    return s.seq;
}
