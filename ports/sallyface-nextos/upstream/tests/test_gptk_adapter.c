#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gptk_adapter.h"

typedef struct {
    int calls;
    int keycode[8];
    int pressed[8];
    int physical[8];
} delivery_log;

static void record_delivery(void *user, int keycode, int pressed,
                            int physical)
{
    delivery_log *log = user;
    assert(log->calls < 8);
    log->keycode[log->calls] = keycode;
    log->pressed[log->calls] = pressed;
    log->physical[log->calls] = physical;
    log->calls++;
}

static const char baseline[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "port = sallyface\n"
    "[menu]\n"
    "A = sf.interact\n"
    "B = sf.back\n"
    "LEFT_STICK = sf.move\n"
    "[gameplay]\n"
    "A = sf.interact\n"
    "B = sf.back\n"
    "LEFT_STICK = sf.move\n";

static const char swapped[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "port = sallyface\n"
    "[menu]\n"
    "A = sf.back\n"
    "B = sf.interact\n"
    "LEFT_STICK = sf.move\n"
    "[gameplay]\n"
    "A = sf.back\n"
    "B = sf.interact\n"
    "LEFT_STICK = sf.move\n";

static void baseline_reaches_android_sink_once(void)
{
    delivery_log log = { 0 };
    assert(sf_gptk_init_text(baseline, strlen(baseline), record_delivery,
                             &log, "test") == 0);
    assert(sf_gptk_control_owned(NXINPUT_GPTK_A));
    assert(sf_gptk_control_owned(NXINPUT_GPTK_LEFT_STICK));
    sf_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
    sf_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
    sf_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
    assert(log.calls == 2);
    assert(log.keycode[0] == 96 && log.pressed[0] == 1);
    assert(log.keycode[1] == 96 && log.pressed[1] == 0);
    assert(log.physical[0] == NXINPUT_GPTK_A);
    assert(sf_gptk_raw_duplicate_count() == 0);
    sf_gptk_close();
}

static void ab_swap_changes_the_real_sink(void)
{
    delivery_log log = { 0 };
    assert(sf_gptk_init_text(swapped, strlen(swapped), record_delivery,
                             &log, "test") == 0);
    sf_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
    sf_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
    sf_gptk_feed_button(NXINPUT_GPTK_B, 1, 1.0f);
    sf_gptk_feed_button(NXINPUT_GPTK_B, 0, 0.0f);
    assert(log.calls == 4);
    assert(log.keycode[0] == 97 && log.keycode[1] == 97);
    assert(log.keycode[2] == 96 && log.keycode[3] == 96);
    assert(log.physical[0] == NXINPUT_GPTK_A);
    assert(log.physical[2] == NXINPUT_GPTK_B);
    sf_gptk_close();
}

static void stick_is_semantic_and_duplicate_guard_is_measurable(void)
{
    delivery_log log = { 0 };
    float x = 0.0f, y = 0.0f;
    assert(sf_gptk_init_text(baseline, strlen(baseline), record_delivery,
                             &log, "test") == 0);
    assert(sf_gptk_route_stick(NXINPUT_GPTK_LEFT_STICK, 0.75f, -0.25f,
                               &x, &y) == 1);
    assert(x == 0.75f && y == -0.25f);
    assert(sf_gptk_route_stick(NXINPUT_GPTK_RIGHT_STICK, 1.0f, 1.0f,
                               &x, &y) == 0);
    assert(sf_gptk_raw_duplicate_count() == 0);
    sf_gptk_note_native_button_delivery(NXINPUT_GPTK_A);
    assert(sf_gptk_raw_duplicate_count() == 1);
    sf_gptk_close();
}

static void invalid_semantic_binding_fails_closed(void)
{
    static const char bad[] =
        "format = NEXTOS_CONTROLLERS/1\n"
        "port = sallyface\n"
        "[menu]\nA = sf.move\n"
        "[gameplay]\nA = sf.move\n";
    delivery_log log = { 0 };
    assert(sf_gptk_init_text(bad, strlen(bad), record_delivery, &log,
                             "test") == -1);
    assert(!sf_gptk_control_owned(NXINPUT_GPTK_A));
}

int main(void)
{
    baseline_reaches_android_sink_once();
    ab_swap_changes_the_real_sink();
    stick_is_semantic_and_duplicate_guard_is_measurable();
    invalid_semantic_binding_fails_closed();
    puts("gptk adapter tests: OK");
    return 0;
}
