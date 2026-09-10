#ifndef HGO_MOTION_STREAM_H
#define HGO_MOTION_STREAM_H

#include <stdint.h>

typedef struct {
    int action;
    int source;
    int device_id;
    int meta_state;
    int button_state;
    int flags;
    int64_t event_time;
    int64_t down_time;
    float axis[48];
} hgo_motion_payload;

typedef struct {
    int64_t touch_down_time;
    int touch_active;
} hgo_motion_stream_state;

void hgo_motion_stream_init(hgo_motion_stream_state *state);
void hgo_motion_gamepad(hgo_motion_payload *event, int64_t now,
                        float lx, float ly, float rx, float ry,
                        float lt, float rt, float hat_x, float hat_y);
int hgo_motion_touch(hgo_motion_stream_state *state,
                     hgo_motion_payload *event, int64_t now,
                     int action, float x, float y);

#endif
