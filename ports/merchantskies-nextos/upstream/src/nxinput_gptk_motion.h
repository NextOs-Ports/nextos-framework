/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_MOTION_H
#define NXINPUT_GPTK_MOTION_H

/* V3 kinematics for the NEXTOSCONTROLLERS tuning: universal cursor motion
 * and camera axis shaping. Pure math, no SDL, no devices, NO static state:
 * every function reads only its arguments, so the same sample transformed
 * twice yields the same result and tuning is never double-applied.
 *
 * FPS-invariance guarantee: nxinput_gptk_cursor_step() is delta-time based
 * (target velocity + exponential smoothing + trapezoidal integration), so
 * integrating a constant deflection over the same wall-clock time at 30, 60
 * or 120 FPS produces the same displacement (within discretization noise,
 * well under 2%).
 */

#include "nxinput_gptk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cursor state carried between frames: position in pixels plus the smoothed
 * velocity in pixels/second. Owned by the caller. */
typedef struct nxinput_gptk_cursor_state {
  float x;     /* pixels, 0 .. drawable_w-1 */
  float y;     /* pixels, 0 .. drawable_h-1 */
  float vel_x; /* smoothed velocity, pixels/second */
  float vel_y;
} nxinput_gptk_cursor_state;

/* Place the cursor and zero all accumulated velocity/smoothing. MUST be
 * called on a context switch so no residual velocity leaks into the new
 * context. */
void nxinput_gptk_cursor_state_reset(nxinput_gptk_cursor_state *state,
                                     float x, float y);

/* Advance the cursor by one frame of dt_seconds with stick deflection
 * (axis_x, axis_y) in -1..1 each (radially clamped to magnitude 1).
 *
 * Pipeline: radial deadzone with rescaling -> response curve (pow on the
 * normalized magnitude) -> acceleration gain (1 + acceleration * curved)
 * -> pixels/second scaling (tuning->speed is the fraction of drawable
 * HEIGHT traveled per second at full deflection) -> exponential smoothing
 * with time constant smoothing_ms -> trapezoidal position integration ->
 * clamp into the drawable.
 *
 * Returns 0 and updates *state; returns -1 (state untouched) on NULL
 * arguments, dt_seconds <= 0, non-positive drawable, or tuning values
 * outside the documented bounds (fail closed). NaN axes count as 0. */
int nxinput_gptk_cursor_step(const nxinput_gptk_cursor_tuning *tuning,
                             float axis_x, float axis_y, float dt_seconds,
                             int drawable_w, int drawable_h,
                             nxinput_gptk_cursor_state *state);

/* Shape one camera axis sample. Pure function: call it exactly once per
 * sample; the result must feed the game INSTEAD of the raw axes, never in
 * addition to them (no double application of sensitivity).
 *
 * authority == NXINPUT_GPTK_AUTHORITY_NEXTOS: radial deadzone with
 * rescaling (full deflection still reaches magnitude 1 -- no resolution
 * loss above the deadzone), response curve, per-axis sensitivity and
 * inversion.
 *
 * authority == NXINPUT_GPTK_AUTHORITY_NATIVE: identity pass-through with
 * deadzone treated as 0 -- the game's NATIVE menu governs deadzone,
 * sensitivity and inversion, and NextOS applies nothing on top.
 *
 * Returns 0 with the result in out_x/out_y; returns -1 with both outputs
 * zeroed on NULL arguments or out-of-bounds tuning (fail closed). */
int nxinput_gptk_camera_transform(const nxinput_gptk_camera_tuning *tuning,
                                  float axis_x, float axis_y, float *out_x,
                                  float *out_y);

#ifdef __cplusplus
}
#endif

#endif
