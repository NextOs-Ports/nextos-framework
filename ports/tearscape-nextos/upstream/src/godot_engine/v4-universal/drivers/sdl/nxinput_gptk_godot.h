/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_GODOT_H
#define NXINPUT_GPTK_GODOT_H

/* TEARSCAPE-CONTROLS-LIVE: the NEXTOSCONTROLLERS.gptk runtime for Godot.
 *
 * Chain: physical pad -> nxinput-normalized SDL gamepad event -> GPTK
 * live decision (action / null / native, per control and per proved context)
 * -> ACK-capable real Godot InputMap sinks (InputEventAction through
 * Input::parse_input_event on the main thread).
 *
 * A control governed by the GPTK (action or null) is suppressed before the
 * native JoyButton/JoyAxis path only after complete sink coverage and scene
 * context are proved, so one physical press can never fire twice and an
 * incomplete integration always remains native-safe.
 * The SELECT+START lifecycle chord stays sovereign and out-of-band: the
 * joypad driver updates it BEFORE asking this governor anything, and this
 * module never reads or remaps it.
 *
 * Everything here runs on the main thread: process_joypads() and the
 * pre-event context tick are both main-loop work, so no locking is needed.
 */

#include <stdbool.h>

typedef struct SDL_Gamepad SDL_Gamepad;

#ifdef __cplusplus
extern "C" {
#endif

/* Port-owned ACK sinks and context resolver. These are exported from the
 * final ELF and named by the external nxrelease input-proof receipt. */
int tearscape_gptk_inputmap_sink(
		void *p_user, const char *p_action, int p_pressed, float p_value);
int tearscape_gptk_inputmap_vector_sink(
		void *p_user, const char *p_action, float p_x, float p_y);
int tearscape_gptk_quit_sink(
		void *p_user, const char *p_action, int p_pressed, float p_value);
int tearscape_gptk_resolve_context(const char *p_scene_path, int p_tree_paused);

#ifdef __cplusplus
}
#endif

/* TEARSCAPE 0.2.10: the narrow pre-init boundary. Reads the owner/default
 * NEXTOSCONTROLLERS.gptk EXACTLY once (nxinput_gptk_preinit_load) and
 * returns the selected FACE_LAYOUT (0 auto, 1 modern, 2 retro) for
 * nxc6_declare_port_bundle_for_layout(). Must run at the very top of
 * JoypadSDL::initialize(), before bundle declare, staging and SDL_Init. */
#ifdef __cplusplus
extern "C" int nxgptk_godot_preinit();
#else
int nxgptk_godot_preinit(void);
#endif

/* Consumes the single pre-init read (owner copy in the game directory,
 * immutable default under defaults/) and prints the bounded load receipt to
 * the log. Safe to call every frame. */
void nxgptk_godot_init_once();

/* True only when a valid mapping (owner or default) is live. When false the
 * whole input path stays byte-for-byte native. */
bool nxgptk_godot_active();

/* Main-thread tick: proves the live context from Tearscape's current scene,
 * releasing every latched action on a context switch, and feeds cached stick
 * vectors. An unknown scene keeps every control on Godot's native path. */
void nxgptk_godot_tick(double p_delta);

/* TEARSCAPE 0.2.17: every C6-admitted gamepad (up to the pad-set cap) drives
 * ONE logical player: a control is down when any admitted pad holds it, an
 * axis is the largest deflection across pads, and the exit chord is judged
 * per pad by the joypad driver (SELECT here + START there is denied and
 * logged once). Admission logs `nx/input: controller: ... mapping=...` and
 * `nx/input: pad slot=...` per pad; removal logs `controller-removed`,
 * releases every latched action and keeps the remaining pads working.
 * Calling connected() for a pad that is already a member refreshes its truth
 * from the complete SDL snapshot and returns true. Co-op (attack2 present)
 * keeps every pad on Godot's native device-aware path as before. */
bool nxgptk_godot_gamepad_connected(int p_joy_id, SDL_Gamepad *p_gamepad);
bool nxgptk_godot_gamepad_disconnected(int p_joy_id);

/* Gamepad button event (SDL_GamepadButton). Returns true when the control is
 * governed (action delivered or explicitly null-suppressed) and the native
 * path must NOT run; false hands the event to the native path untouched. */
bool nxgptk_godot_gamepad_button(int p_joy_id, int p_sdl_button, bool p_pressed);

/* Gamepad axis event (SDL_GamepadAxis, value in -1..1 for sticks and 0..1
 * for triggers). Same contract as the button hook. */
/* Signed SDL stick axis -> [-1, 1] with an EXACT zero at rest. The symmetric
 * (v+32768)/65535 form maps a centered stick to +1.5e-5, which every strict
 * "deflected" test (vector gesture evidence, handoff) reads as motion: the
 * gesture opens on admission and never closes. One normalizer for the
 * snapshot and the event path, so both agree byte-for-byte. */
static inline float nxgptk_godot_stick_axis_value(int p_raw) {
	if (p_raw >= 0) {
		return p_raw >= 32767 ? 1.0f : (float)p_raw / 32767.0f;
	}
	return p_raw <= -32768 ? -1.0f : (float)p_raw / 32768.0f;
}

bool nxgptk_godot_gamepad_axis(int p_joy_id, int p_sdl_axis, float p_value);

/* One-shot consume of a mapped system.quit action (never the sovereign
 * chord, which has its own path). */
bool nxgptk_godot_consume_quit_request();

/* A sink ACK/release failure is terminal and distinct from a clean mapped
 * quit.  The display loop consumes this signal before publishing health,
 * requests close and sets a nonzero process exit status. */
bool nxgptk_godot_consume_fatal_request();

#endif /* NXINPUT_GPTK_GODOT_H */
