#include <assert.h>
#include <stdio.h>

#include "motion_stream.h"

int main(void)
{
    hgo_motion_stream_state state;
    hgo_motion_payload down, pad, move, up, next_down, cancel;
    hgo_motion_stream_init(&state);

    assert(!hgo_motion_touch(&state, &move, 90, 2, 1.0f, 1.0f));
    assert(hgo_motion_touch(&state, &down, 100, 0, 12.0f, 34.0f));
    assert(!hgo_motion_touch(&state, &move, 105, 0, 1.0f, 1.0f));
    hgo_motion_gamepad(&pad, 110, 0.1f, 0.2f, 0.3f, 0.4f,
                       0.0f, 0.0f, 0.0f, 0.0f);
    assert(hgo_motion_touch(&state, &move, 120, 2, 56.0f, 78.0f));
    assert(hgo_motion_touch(&state, &up, 130, 1, 90.0f, 12.0f));
    assert(!hgo_motion_touch(&state, &move, 140, 2, 1.0f, 1.0f));
    assert(!hgo_motion_touch(&state, &up, 150, 1, 1.0f, 1.0f));
    assert(hgo_motion_touch(&state, &next_down, 200, 0, 1.0f, 2.0f));
    assert(hgo_motion_touch(&state, &cancel, 210, 3, 3.0f, 4.0f));
    assert(!hgo_motion_touch(&state, &cancel, 220, 3, 3.0f, 4.0f));

    assert(down.source == 0x00001002 && down.device_id == 0);
    assert(down.down_time == 100 && down.event_time == 100);
    assert(pad.source == 0x01000010 && pad.device_id == 1);
    assert(pad.down_time == 110 && pad.axis[11] == 0.3f);
    assert(move.down_time == 100 && move.event_time == 120);
    assert(move.axis[0] == 56.0f && move.axis[1] == 78.0f);
    assert(up.down_time == 100 && up.event_time == 130);
    assert(next_down.down_time == 200);
    assert(cancel.down_time == 200 && state.touch_down_time == 0);
    assert(state.touch_active == 0);

    /* Repeated select -> hold -> drag -> release -> reselect cycles are the
     * community failure mode.  Interleave a gamepad frame on every cycle and
     * prove it cannot alter the Android finger clock or grammar. */
    for (int cycle = 0; cycle < 1000; cycle++) {
        int64_t start = 1000 + (int64_t)cycle * 10;
        assert(hgo_motion_touch(&state, &down, start, 0, 10.0f, 20.0f));
        assert(!hgo_motion_touch(&state, &next_down, start + 1, 0,
                                 11.0f, 21.0f));
        hgo_motion_gamepad(&pad, start + 2, 0.0f, 0.0f, 1.0f, -1.0f,
                           0.0f, 0.0f, 0.0f, 0.0f);
        assert(hgo_motion_touch(&state, &move, start + 3, 2,
                                30.0f, 40.0f));
        assert(hgo_motion_touch(&state, &up, start + 4, 1,
                                50.0f, 60.0f));
        assert(down.down_time == start);
        assert(move.down_time == start);
        assert(up.down_time == start);
        assert(pad.down_time == start + 2);
        assert(!hgo_motion_touch(&state, &move, start + 5, 2,
                                 70.0f, 80.0f));
        assert(!hgo_motion_touch(&state, &up, start + 6, 1,
                                 70.0f, 80.0f));
        assert(!state.touch_active && state.touch_down_time == 0);
    }

    assert(!hgo_motion_touch(&state, &move, 20000, -1, 0.0f, 0.0f));
    assert(!hgo_motion_touch(&state, &move, 20001, 4, 0.0f, 0.0f));

    puts("motion stream test: OK");
    return 0;
}
