#ifndef DEADTRIGGER_GAMEPAD_DT_H
#define DEADTRIGGER_GAMEPAD_DT_H

#include <stdint.h>

/*
 * Dead Trigger consumes semantic actions through PlayerControlsGamepad.
 * The UI thread publishes one normalized SDL sample here; UnityMain freezes
 * it once per nativeRender so ButtonDown/ButtonUp keep Android/Unity frame
 * semantics even though the two loops run independently.
 */
void dt_gamepad_publish(float left_x, float left_y,
                        float right_x, float right_y,
                        uint32_t semantic_level, int enabled);
void dt_gamepad_frame_begin(void);
void dt_gamepad_try_install(void);
int dt_gamepad_is_installed(void);
int dt_gamepad_gameplay_active(void);

#endif
