// Host unit tests for what the RT4K says about itself (src/core/rt4k_info_core.c). Run with tests/run.sh.

#include <stdio.h>
#include <string.h>

#include "rt4k_info_core.h"

static int failures, checks;
static const char *current;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)

static uint32_t now;

static const char *poll(bool on) {
    now += 200; // the power task's tick
    return rt4k_info_core_poll(on, now);
}

static bool asks(const char *got, const char *want) {
    return got && !strcmp(got, want);
}

// The RT4K's answers to both questions.
static void answer_ver(void) {
    rt4k_info_core_line("[COM] RT4KPRO, FW Version: 1.89.0");
    rt4k_info_core_line("[COM] Build tag: b0928e");
}
static void answer_model(void) {
    rt4k_info_core_line("[COM] model=4 RT4K_Pro");
}

static void test_nothing_known(void) {
    now = 0;
    rt4k_info_core_init(NULL);
    rt4k_info_t i;
    CHECK(!rt4k_info_core_get(&i));
    CHECK(!i.version[0] && !i.model[0]);
    CHECK(!poll(false)); // asleep: no questions
    CHECK(!rt4k_info_core_take_changed(&i));
}

static void test_asks_when_it_comes_on(void) {
    now = 0;
    rt4k_info_core_init(NULL);
    poll(false);
    CHECK(asks(poll(true), "ver"));
    CHECK(!poll(true)); // one question at a time
    answer_ver();
    CHECK(asks(poll(true), "model")); // at once, not after the retry wait
    answer_model();
    CHECK(!poll(true));
    rt4k_info_t i;
    CHECK(rt4k_info_core_get(&i));
    CHECK(!strcmp(i.version, "1.89.0"));
    CHECK(!strcmp(i.model, "RT4K_Pro"));
}

static void test_the_power_probe_counts(void) {
    // The probe that noticed it's on was "ver": only the model is left to ask.
    now = 0;
    rt4k_info_core_init(NULL);
    poll(false);
    answer_ver();
    CHECK(asks(poll(true), "model"));
}

static void test_anyone_asking_counts(void) {
    // The page or Home Assistant asked; the replies count while it's on already.
    now = 0;
    rt4k_info_core_init(NULL);
    CHECK(asks(poll(true), "ver"));
    answer_ver();
    answer_model();
    CHECK(!poll(true));
    rt4k_info_t i;
    CHECK(rt4k_info_core_get(&i));
}

static void test_one_write_for_both(void) {
    now = 0;
    rt4k_info_core_init(NULL);
    rt4k_info_t k;
    poll(true);
    answer_ver();
    CHECK(!rt4k_info_core_take_changed(&k)); // the model is still to come
    poll(true);                               // asks "model"
    CHECK(!rt4k_info_core_take_changed(&k));
    answer_model();
    poll(true);                               // nothing left to ask
    CHECK(rt4k_info_core_take_changed(&k));
    CHECK(!strcmp(k.version, "1.89.0") && !strcmp(k.model, "RT4K_Pro"));
    CHECK(!rt4k_info_core_take_changed(&k)); // once
}

static void test_same_again_writes_nothing(void) {
    // A restart with the same firmware: what was kept is what it says.
    now = 0;
    rt4k_info_t saved = {"1.89.0", "RT4K_Pro"};
    rt4k_info_core_init(&saved);
    rt4k_info_t k;
    poll(true);
    answer_ver();
    poll(true);
    answer_model();
    poll(true);
    CHECK(!rt4k_info_core_take_changed(&k));
    // Off and on again: still nothing.
    poll(false);
    poll(true);
    answer_ver();
    poll(true);
    answer_model();
    poll(true);
    CHECK(!rt4k_info_core_take_changed(&k));
}

static void test_new_firmware_is_kept(void) {
    now = 0;
    rt4k_info_t saved = {"1.87.3", "RT4K_Pro"};
    rt4k_info_core_init(&saved);
    rt4k_info_t i, k;
    CHECK(!rt4k_info_core_get(&i));         // remembered, not fresh
    CHECK(!strcmp(i.version, "1.87.3"));
    poll(true);
    answer_ver();
    poll(true);
    answer_model();
    poll(true);
    CHECK(rt4k_info_core_take_changed(&k));
    CHECK(!strcmp(k.version, "1.89.0"));
}

static void test_standby_makes_it_stale(void) {
    now = 0;
    rt4k_info_core_init(NULL);
    poll(true);
    answer_ver();
    poll(true);
    answer_model();
    poll(true);
    rt4k_info_t i;
    CHECK(rt4k_info_core_get(&i));
    const uint32_t seq = rt4k_info_core_seq();
    poll(false);
    CHECK(!rt4k_info_core_get(&i));         // from before now
    CHECK(!strcmp(i.version, "1.89.0"));    // but still known
    CHECK(rt4k_info_core_seq() != seq);     // pushed
    CHECK(asks(poll(true), "ver"));         // on again: asked again
}

static void test_gives_up(void) {
    // A model reply it can't read (another firmware) is asked a few times, then left alone.
    now = 0;
    rt4k_info_core_init(NULL);
    poll(true);
    answer_ver();
    int models = 0;
    for (int t = 0; t < 200; t++) {
        const char *a = poll(true);
        if (asks(a, "model")) models++;
    }
    CHECK(models == RT4K_INFO_TRIES);
    rt4k_info_t k;
    CHECK(rt4k_info_core_take_changed(&k)); // the version alone is kept then
    CHECK(!strcmp(k.version, "1.89.0") && !k.model[0]);
}

static void test_retries_after_a_while(void) {
    now = 0;
    rt4k_info_core_init(NULL);
    CHECK(asks(poll(true), "ver"));
    int quiet = 0;
    while (!poll(true)) quiet++;
    CHECK(quiet * 200 + 200 >= RT4K_INFO_RETRY_MS);
}

static void test_other_lines(void) {
    now = 0;
    rt4k_info_core_init(NULL);
    rt4k_info_core_line("FW Version: 9.9.9");                  // not a command reply
    rt4k_info_core_line("[COM] Serial Remote: pwr");
    rt4k_info_core_line("[COM] banner=model=x");                // "model=" not at the start
    rt4k_info_t i;
    rt4k_info_core_get(&i);
    CHECK(!i.version[0] && !i.model[0]);
    rt4k_info_core_line("[COM] model=RT4K_CE");                 // no number first: the rest
    rt4k_info_core_get(&i);
    CHECK(!strcmp(i.model, "RT4K_CE"));
    rt4k_info_core_line("[COM] RT4KPRO, FW Version: 1.2.3.4.5.6.7.8.9.10.11.12.13"); // cut, not overflowing
    rt4k_info_core_get(&i);
    CHECK(strlen(i.version) == RT4K_INFO_VERSION_MAX);
}

static void test_saved_garbage(void) {
    // A kept record without its terminators (it can't happen, but flash is flash).
    rt4k_info_t saved;
    memset(&saved, 'x', sizeof(saved));
    rt4k_info_core_init(&saved);
    rt4k_info_t i;
    rt4k_info_core_get(&i);
    CHECK(strlen(i.version) == RT4K_INFO_VERSION_MAX && strlen(i.model) == RT4K_INFO_MODEL_MAX);
}

int main(void) {
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"nothing_known", test_nothing_known},
        {"asks_when_it_comes_on", test_asks_when_it_comes_on},
        {"the_power_probe_counts", test_the_power_probe_counts},
        {"anyone_asking_counts", test_anyone_asking_counts},
        {"one_write_for_both", test_one_write_for_both},
        {"same_again_writes_nothing", test_same_again_writes_nothing},
        {"new_firmware_is_kept", test_new_firmware_is_kept},
        {"standby_makes_it_stale", test_standby_makes_it_stale},
        {"gives_up", test_gives_up},
        {"retries_after_a_while", test_retries_after_a_while},
        {"other_lines", test_other_lines},
        {"saved_garbage", test_saved_garbage},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        current = tests[i].name;
        tests[i].fn();
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
