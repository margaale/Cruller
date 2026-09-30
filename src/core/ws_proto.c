#include "ws_proto.h"

#include <string.h>

// --- SHA-1 (RFC 3174), only for the handshake ----------------------------------------------------

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        const uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    const size_t rest = len - i;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const size_t n = rest < 56 ? 64 : 128;
    const uint64_t bits = (uint64_t)len * 8;
    for (int b = 0; b < 8; b++) tail[n - 1 - b] = (uint8_t)(bits >> (8 * b));
    sha1_block(h, tail);
    if (n == 128) sha1_block(h, tail + 64);
    for (int j = 0; j < 5; j++) {
        out[4 * j] = (uint8_t)(h[j] >> 24);
        out[4 * j + 1] = (uint8_t)(h[j] >> 16);
        out[4 * j + 2] = (uint8_t)(h[j] >> 8);
        out[4 * j + 3] = (uint8_t)h[j];
    }
}

// --- base64 ----------------------------------------------------------------------------------------

size_t ws_base64(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

void ws_accept_key(const char *key, char out[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t buf[128];
    size_t n = strlen(key);
    if (n > sizeof(buf) - sizeof(guid)) n = sizeof(buf) - sizeof(guid);
    memcpy(buf, key, n);
    memcpy(buf + n, guid, sizeof(guid) - 1);
    uint8_t digest[20];
    ws_sha1(buf, n + sizeof(guid) - 1, digest);
    ws_base64(digest, sizeof(digest), out);
}

// --- frames ------------------------------------------------------------------------------------

size_t ws_frame_header(uint8_t *out, uint8_t opcode, uint64_t payload_len) {
    out[0] = (uint8_t)(0x80 | (opcode & 0x0f));
    if (payload_len < 126) {
        out[1] = (uint8_t)payload_len;
        return 2;
    }
    if (payload_len <= 0xffff) {
        out[1] = 126;
        out[2] = (uint8_t)(payload_len >> 8);
        out[3] = (uint8_t)payload_len;
        return 4;
    }
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)(payload_len >> (56 - 8 * i));
    return 10;
}

long ws_parse(uint8_t *buf, size_t len, size_t max_payload, ws_frame_t *f) {
    if (len < 2) return 0;
    const bool fin = buf[0] & 0x80;
    const uint8_t opcode = buf[0] & 0x0f;
    if (buf[0] & 0x70) return -1;             // no extensions negotiated
    if (!fin || opcode == 0) return -1;       // fragmented messages: not needed, not supported
    if (!(buf[1] & 0x80)) return -1;          // clients must mask
    uint64_t plen = buf[1] & 0x7f;
    size_t pos = 2;
    if (plen == 126) {
        if (len < 4) return 0;
        plen = (uint64_t)buf[2] << 8 | buf[3];
        pos = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = plen << 8 | buf[2 + i];
        pos = 10;
    }
    if (plen > max_payload) return -1;
    if (len < pos + 4 + plen) return 0;
    const uint8_t *mask = buf + pos;
    uint8_t *payload = buf + pos + 4;
    for (size_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
    f->opcode = opcode;
    f->payload = payload;
    f->len = (size_t)plen;
    return (long)(pos + 4 + plen);
}

// --- /api/v1/events' types -----------------------------------------------------------------------

static const char *const event_names[] = {"state"}; // bit i of a mask is event_names[i]
#define EVENT_TYPES (sizeof(event_names) / sizeof(event_names[0]))

uint32_t ws_event_types(const char *list) {
    uint32_t types = 0;
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *end = p;
        while (*end && *end != ',') end++;
        size_t n = (size_t)(end - p);
        while (n && p[n - 1] == ' ') n--;
        for (size_t i = 0; i < EVENT_TYPES; i++) {
            if (n && n == strlen(event_names[i]) && !strncmp(p, event_names[i], n)) types |= 1u << i;
        }
        p = end;
    }
    return types;
}

// Appends s to out (n bytes so far); false if it doesn't fit with the NUL.
static int put(char *out, size_t size, size_t *n, const char *s) {
    const size_t l = strlen(s);
    if (*n + l >= size) return 0;
    memcpy(out + *n, s, l + 1);
    *n += l;
    return 1;
}

size_t ws_event_names(uint32_t types, char *out, size_t size) {
    size_t n = 0;
    int ok = size > 0 && put(out, size, &n, "[");
    for (size_t i = 0, first = 1; ok && i < EVENT_TYPES; i++) {
        if (!(types & (1u << i))) continue;
        ok = (first || put(out, size, &n, ",")) && put(out, size, &n, "\"") && put(out, size, &n, event_names[i]) &&
            put(out, size, &n, "\"");
        first = 0;
    }
    ok = ok && put(out, size, &n, "]");
    if (!ok) {
        if (size) out[0] = 0;
        return 0;
    }
    return n;
}
