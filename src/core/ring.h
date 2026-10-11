// A byte ring that readers follow each at their own pace: the log (log_ring.c) and the web terminal's text
// (console.c). head counts every byte ever written; size is a power of two. No lock of its own: the
// caller holds its own around each call.

#pragma once

#include <stddef.h>
#include <stdint.h>

static inline void ring_put(char *ring, uint32_t size, uint32_t *head, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) ring[(*head + i) & (size - 1)] = s[i];
    *head += (uint32_t)n;
}

// Copies the bytes written after *pos into out (up to max) and advances *pos. A reader whose bytes were
// overwritten, or ahead of head (one from before a reset), starts again at the oldest byte kept.
static inline size_t ring_read(const char *ring, uint32_t size, uint32_t head, uint32_t *pos, char *out, size_t max) {
    uint32_t from = *pos;
    if (from > head || head - from > size) from = head > size ? head - size : 0;
    size_t n = head - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = ring[(from + i) & (size - 1)];
    *pos = from + (uint32_t)n;
    return n;
}
