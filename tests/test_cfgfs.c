// Host unit tests for cfgfs (src/core/cfgfs.c) over a flash of RAM (tests/cfgfs_ram.c). Run with
// tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "cfgfs.h"
#include "cfgfs_ram.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)
#define RUN(t) do { current = #t; t(); } while (0)

static char buf[8192];

static void test_mount(void) {
    cfgfs_ram_present(false);
    CHECK(!cfgfs_mount() && cfgfs_read("a", buf, sizeof(buf)) < 0 && !cfgfs_write("a", "x", 1)); // no flash: nothing works
    cfgfs_ram_present(true);
    cfgfs_ram_fill(0x5a); // whatever was there before (DonutShop's old blocks): formatted
    CHECK(cfgfs_mount());
    CHECK(cfgfs_read("a", buf, sizeof(buf)) < 0);
    CHECK(cfgfs_write("a", "hello", 5) && cfgfs_read("a", buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5));
    CHECK(cfgfs_mount() && cfgfs_read("a", buf, sizeof(buf)) == 5); // mounted again: still there
    CHECK(cfgfs_read("a", buf, 4) < 0);                              // bigger than the room given
}

static void test_replace_remove(void) {
    CHECK(cfgfs_write("b", "one", 3) && cfgfs_write("b", "second", 6) && cfgfs_read("b", buf, sizeof(buf)) == 6 && !memcmp(buf, "second", 6));
    CHECK(cfgfs_write("b", "", 0) && cfgfs_read("b", buf, sizeof(buf)) == 0);
    CHECK(cfgfs_remove("b") && cfgfs_read("b", buf, sizeof(buf)) < 0 && cfgfs_remove("b")); // gone, and again
    CHECK(cfgfs_format() && cfgfs_read("a", buf, sizeof(buf)) < 0);
}

typedef struct { int n; char seen[8][32]; int stop_at; } lines_t;
static bool collect(const char *line, void *ctx) {
    lines_t *l = ctx;
    if (l->n < 8) snprintf(l->seen[l->n], sizeof(l->seen[0]), "%s", line);
    return ++l->n != l->stop_at;
}

static void test_lines(void) {
    char line[16];
    lines_t l = {0};
    CHECK(!cfgfs_lines("none", line, sizeof(line), collect, &l));
    const char *text = "first\n\nthis one is far too long for the line\nlast without newline";
    CHECK(cfgfs_write("t", text, strlen(text)));
    l = (lines_t){0};
    char big[32];
    CHECK(cfgfs_lines("t", big, sizeof(big), collect, &l) && l.n == 3 && !strcmp(l.seen[0], "first") && !strcmp(l.seen[1], "") &&
        !strcmp(l.seen[2], "last without newline")); // the 37-byte line skipped whole, an empty one kept
    l = (lines_t){0};
    CHECK(cfgfs_lines("t", line, sizeof(line), collect, &l) && l.n == 2 && !strcmp(l.seen[0], "first")); // 16: the last one skipped too
    l = (lines_t){.stop_at = 1};
    CHECK(cfgfs_lines("t", big, sizeof(big), collect, &l) && l.n == 1); // fn says stop
}

// A file rewritten line by line from the old one, as gameID's games are: the old one read while the new
// one is written.
typedef struct { const char *drop; } edit_t;
static bool keep(const char *line, void *ctx) {
    const edit_t *e = ctx;
    if (strcmp(line, e->drop)) {
        cfgfs_put(line, strlen(line));
        cfgfs_put("\n", 1);
    }
    return true;
}

static void test_rewrite(void) {
    char text[4000] = "", line[64];
    for (int k = 0; k < 200; k++) snprintf(text + strlen(text), sizeof(text) - strlen(text), "game %03d\n", k);
    CHECK(cfgfs_write("g", text, strlen(text)));
    edit_t e = {"game 100"};
    CHECK(cfgfs_create("g") && cfgfs_lines("g", line, sizeof(line), keep, &e) && cfgfs_commit());
    const long n = cfgfs_read("g", buf, sizeof(buf));
    CHECK(n == (long)strlen(text) - 9 && !strstr(buf, "game 100") && strstr(buf, "game 101"));
    // dropped instead: the old one stays
    CHECK(cfgfs_create("g") && cfgfs_put("x", 1));
    cfgfs_abort();
    CHECK(cfgfs_read("g", buf, sizeof(buf)) == n);
    CHECK(cfgfs_create("g") && !cfgfs_create("h")); // one written at a time
    cfgfs_abort();
}

// The power cut at every point of a replace: the file is the old one or the new one, never anything else.
static void test_power_cut(void) {
    char old[3000], new_[3000];
    memset(old, 'o', sizeof(old));
    memset(new_, 'n', sizeof(new_));
    int seen_old = 0, seen_new = 0, bad = 0;
    for (int cut = 0; cut < 40; cut++) {
        cfgfs_ram_cut_after(-1);
        CHECK(cfgfs_format() && cfgfs_write("p", old, sizeof(old)));
        cfgfs_ram_cut_after(cut);
        cfgfs_write("p", new_, sizeof(new_));
        cfgfs_ram_cut_after(-1);
        CHECK(cfgfs_mount()); // after the restart
        const long n = cfgfs_read("p", buf, sizeof(buf));
        if (n == (long)sizeof(old) && !memcmp(buf, old, sizeof(old))) seen_old++;
        else if (n == (long)sizeof(new_) && !memcmp(buf, new_, sizeof(new_))) seen_new++;
        else bad++;
    }
    CHECK(!bad && seen_old && seen_new);
    printf("  power cut at 40 points: old %d times, new %d times, anything else %d\n", seen_old, seen_new, bad);
}

int main(void) {
    RUN(test_mount);
    RUN(test_replace_remove);
    RUN(test_lines);
    RUN(test_rewrite);
    RUN(test_power_cut);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
