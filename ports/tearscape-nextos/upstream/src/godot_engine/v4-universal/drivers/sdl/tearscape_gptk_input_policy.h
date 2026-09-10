/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TEARSCAPE_GPTK_INPUT_POLICY_H
#define TEARSCAPE_GPTK_INPUT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* TEARSCAPE 0.2.17: which pads drive NEXTOSCONTROLLERS is no longer a
 * "primary pad" policy. Every C6-admitted gamepad forms ONE logical player
 * (union of controls, largest-magnitude axes, same-pad exit chord); that
 * policy lives in tearscape_padset.{h,cpp} with its own host gate. Co-op
 * (the game's own attack2 InputMap) still keeps every pad native. */

/* Device disconnect destroys this physical-state cache. Scene/source/co-op
 * transitions deliberately preserve it so the authority handoff can wait for
 * release/neutral instead of swallowing or replaying half a gesture. */
static inline void tears_gptk_clear_axis_cache(
		float *p_left_x, float *p_left_y, float *p_right_x, float *p_right_y) {
	if (p_left_x) {
		*p_left_x = 0.0f;
	}
	if (p_left_y) {
		*p_left_y = 0.0f;
	}
	if (p_right_x) {
		*p_right_x = 0.0f;
	}
	if (p_right_y) {
		*p_right_y = 0.0f;
	}
}

/* A stick mapped to the game's button-like zoom_map action emits one press per
 * radial gesture. Movement/jitter while held cannot retrigger it. */
static inline bool tears_gptk_edge_next(
		bool p_down, float p_x, float p_y) {
	const float magnitude_squared = p_x * p_x + p_y * p_y;
	if (p_down) {
		return magnitude_squared > 0.30f * 0.30f;
	}
	return magnitude_squared >= 0.55f * 0.55f;
}

#endif /* TEARSCAPE_GPTK_INPUT_POLICY_H */
