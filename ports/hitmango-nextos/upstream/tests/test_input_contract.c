#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "input_layout.h"
#include "input_lifecycle.h"
#include "motion_stream.h"
#include "touch_arbiter.h"

static void cursor_gesture(hgo_touch_arbiter_state *arbiter,
                           hgo_motion_stream_state *motion,
                           int64_t start)
{
    hgo_motion_payload down, move, up;
    assert(hgo_touch_arbiter_begin(arbiter, TOUCH_OWNER_CURSOR,
                                   10.0f, 20.0f));
    assert(hgo_motion_touch(motion, &down, start, 0, 10.0f, 20.0f));
    assert(!hgo_touch_arbiter_begin(arbiter, TOUCH_OWNER_SWIPE,
                                    100.0f, 100.0f));
    assert(hgo_touch_arbiter_move(arbiter, TOUCH_OWNER_CURSOR,
                                  30.0f, 40.0f));
    assert(hgo_motion_touch(motion, &move, start + 1, 2, 30.0f, 40.0f));
    assert(hgo_touch_arbiter_end(arbiter, TOUCH_OWNER_CURSOR,
                                 50.0f, 60.0f));
    assert(hgo_motion_touch(motion, &up, start + 2, 1, 50.0f, 60.0f));
    assert(down.down_time == start);
    assert(move.down_time == start);
    assert(up.down_time == start);
    assert(hgo_touch_arbiter_owner(arbiter) == TOUCH_OWNER_NONE);
}

int main(void)
{
    hgo_input_layout layout = HGO_INPUT_LAYOUT_V120_INITIALIZER;
    hgo_touch_arbiter_state arbiter;
    hgo_motion_stream_state motion;
    hgo_input_lifecycle_state lifecycle;
    hgo_motion_payload down, cancel;

    unsetenv("HGO_SWAP_STICKS");
    unsetenv("HGO_CLICK_A");
    hgo_input_layout_from_environment(&layout);
    assert(layout.cursor_on_left);
    assert(layout.cursor_uses_a);
    assert(hgo_input_cursor_button_held(&layout, 0, 1, 0));
    assert(!hgo_input_cursor_button_held(&layout, 0, 0, 1));
    assert(!hgo_input_cursor_button_held(&layout, 1, 1, 0));

    hgo_touch_arbiter_init(&arbiter);
    hgo_motion_stream_init(&motion);
    for (int cycle = 0; cycle < 1000; cycle++)
        cursor_gesture(&arbiter, &motion, 1000 + (int64_t)cycle * 10);

    /* Focus loss cancels the one Android finger and the next A press can own a
     * fresh gesture after focus returns. */
    hgo_input_lifecycle_init(&lifecycle);
    assert(hgo_touch_arbiter_begin(&arbiter, TOUCH_OWNER_CURSOR, 1.0f, 2.0f));
    assert(hgo_motion_touch(&motion, &down, 20000, 0, 1.0f, 2.0f));
    hgo_input_lifecycle_event(&lifecycle, HGO_INPUT_FOCUS_LOST);
    assert(hgo_input_lifecycle_suspended(&lifecycle));
    assert(hgo_touch_arbiter_cancel(&arbiter) == TOUCH_OWNER_CURSOR);
    assert(hgo_motion_touch(&motion, &cancel, 20001, 3, 1.0f, 2.0f));
    hgo_input_lifecycle_event(&lifecycle, HGO_INPUT_FOCUS_GAINED);
    assert(!hgo_input_lifecycle_suspended(&lifecycle));
    cursor_gesture(&arbiter, &motion, 20010);

    /* Hot-unplug follows the same single-cancel boundary and cannot leave A
     * stuck or block the replacement controller's first gesture. */
    assert(hgo_touch_arbiter_begin(&arbiter, TOUCH_OWNER_CURSOR, 3.0f, 4.0f));
    assert(hgo_motion_touch(&motion, &down, 20100, 0, 3.0f, 4.0f));
    assert(hgo_touch_arbiter_cancel(&arbiter) == TOUCH_OWNER_CURSOR);
    assert(hgo_motion_touch(&motion, &cancel, 20101, 3, 3.0f, 4.0f));
    assert(hgo_touch_arbiter_cancel(&arbiter) == TOUCH_OWNER_NONE);
    cursor_gesture(&arbiter, &motion, 20110);

    /* A cursor DOWN and a board swipe can never represent two concurrent
     * fingers; ownership transfers only after the first gesture ends. */
    assert(hgo_touch_arbiter_begin(&arbiter, TOUCH_OWNER_SWIPE, 5.0f, 6.0f));
    assert(!hgo_touch_arbiter_begin(&arbiter, TOUCH_OWNER_CURSOR,
                                    7.0f, 8.0f));
    assert(hgo_touch_arbiter_end(&arbiter, TOUCH_OWNER_SWIPE, 9.0f, 10.0f));
    cursor_gesture(&arbiter, &motion, 20200);

    assert(setenv("HGO_SWAP_STICKS", "0", 1) == 0);
    assert(setenv("HGO_CLICK_A", "0", 1) == 0);
    hgo_input_layout_from_environment(&layout);
    assert(!layout.cursor_on_left);
    assert(!layout.cursor_uses_a);
    assert(!hgo_input_cursor_button_held(&layout, 0, 1, 0));
    assert(hgo_input_cursor_button_held(&layout, 0, 0, 1));

    puts("input v1.2.0 contract test: OK");
    return 0;
}
