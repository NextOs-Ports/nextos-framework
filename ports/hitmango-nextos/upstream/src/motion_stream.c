#include <string.h>

#include "motion_stream.h"

enum {
    HGO_ACTION_DOWN = 0,
    HGO_ACTION_UP = 1,
    HGO_ACTION_MOVE = 2,
    HGO_ACTION_CANCEL = 3,
    HGO_SOURCE_TOUCHSCREEN = 0x00001002,
    HGO_SOURCE_JOYSTICK = 0x01000010,
};

void hgo_motion_stream_init(hgo_motion_stream_state *state)
{
    memset(state, 0, sizeof *state);
}

void hgo_motion_gamepad(hgo_motion_payload *event, int64_t now,
                        float lx, float ly, float rx, float ry,
                        float lt, float rt, float hat_x, float hat_y)
{
    memset(event, 0, sizeof *event);
    event->action = HGO_ACTION_MOVE;
    event->source = HGO_SOURCE_JOYSTICK;
    event->device_id = 1;
    event->event_time = now;
    event->down_time = now;
    event->axis[0] = lx;
    event->axis[1] = ly;
    event->axis[11] = rx;
    event->axis[14] = ry;
    event->axis[17] = lt;
    event->axis[18] = rt;
    event->axis[15] = hat_x;
    event->axis[16] = hat_y;
}

int hgo_motion_touch(hgo_motion_stream_state *state,
                     hgo_motion_payload *event, int64_t now,
                     int action, float x, float y)
{
    if (action < HGO_ACTION_DOWN || action > HGO_ACTION_CANCEL)
        return 0;
    if (action == HGO_ACTION_DOWN) {
        if (state->touch_active)
            return 0;
        state->touch_active = 1;
        state->touch_down_time = now;
    } else if (!state->touch_active) {
        return 0;
    }

    memset(event, 0, sizeof *event);
    event->action = action;
    event->source = HGO_SOURCE_TOUCHSCREEN;
    event->device_id = 0;
    event->event_time = now;
    event->down_time = state->touch_down_time;
    event->axis[0] = x;
    event->axis[1] = y;

    /* Preserve the terminal event's timestamp in its immutable snapshot, then
     * arm the next gesture for a fresh ACTION_DOWN clock. */
    if (action == HGO_ACTION_UP || action == HGO_ACTION_CANCEL) {
        state->touch_down_time = 0;
        state->touch_active = 0;
    }
    return 1;
}
