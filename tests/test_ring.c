// Host unit tests for the byte ring the log and the web terminal keep (src/core/ring.h). Run with
// tests/run.sh.

#include <string.h>

#include "check.h"
#include "ring.h"

#define SIZE 16u

static char ring[SIZE];
static uint32_t head;
static char out[64];

static void reset(void) {
    memset(ring, 0, sizeof(ring));
    head = 0;
}

static void put(const char *s) {
    ring_put(ring, SIZE, &head, s, strlen(s));
}

static size_t read_from(uint32_t *pos, size_t max) {
    return ring_read(ring, SIZE, head, pos, out, max);
}

static void test_what_came_after(void) {
    reset();
    put("hello");
    uint32_t pos = 0;
    CHECK(read_from(&pos, sizeof(out)) == 5 && !memcmp(out, "hello", 5) && pos == 5);
    CHECK(read_from(&pos, sizeof(out)) == 0); // nothing new
    put(" world");
    CHECK(read_from(&pos, sizeof(out)) == 6 && !memcmp(out, " world", 6) && pos == 11);
}

static void test_in_pieces(void) {
    reset();
    put("abcdefgh");
    uint32_t pos = 0;
    CHECK(read_from(&pos, 3) == 3 && !memcmp(out, "abc", 3));
    CHECK(read_from(&pos, 3) == 3 && !memcmp(out, "def", 3));
    CHECK(read_from(&pos, 3) == 2 && !memcmp(out, "gh", 2) && pos == 8);
}

static void test_across_the_end(void) {
    reset();
    put("0123456789abc");
    uint32_t pos = head;
    put("defgh"); // bytes 13 to 17: the ring's end at 16
    CHECK(read_from(&pos, sizeof(out)) == 5 && !memcmp(out, "defgh", 5) && pos == 18);
}

static void test_overwritten_reader(void) {
    reset();
    uint32_t pos = 0;
    put("0123456789abcdef");
    put("XYZ"); // 19 written, the last 16 kept
    CHECK(read_from(&pos, sizeof(out)) == 16 && !memcmp(out, "3456789abcdefXYZ", 16) && pos == 19);
}

static void test_reader_from_before_a_reset(void) {
    reset();
    put("abc");
    uint32_t pos = 500; // further along than anything written since
    CHECK(read_from(&pos, sizeof(out)) == 3 && !memcmp(out, "abc", 3) && pos == 3);
}

int main(void) {
    RUN(test_what_came_after);
    RUN(test_in_pieces);
    RUN(test_across_the_end);
    RUN(test_overwritten_reader);
    RUN(test_reader_from_before_a_reset);
    return tests_done();
}
