/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TEARSCAPE_PADSET_H
#define TEARSCAPE_PADSET_H

/* TEARSCAPE 0.2.17: every C6-admitted gamepad drives ONE logical player.
 *
 * WHY
 *   The framework's automated controls proof (nx-device-input-proof) creates
 *   device-faithful uinput clones of the real pad before the game's SDL_Init
 *   and injects on the clones. A glue that only listens to "the primary pad"
 *   never sees that input, and it cannot honour the rule that SELECT on one
 *   pad plus START on another must never end the game. Both are pad-set
 *   policy, so they live here once, testable on the host.
 *
 * WHAT (capability-based: no device name, VID/PID or CFW is consulted)
 *   - up to TEARS_PADSET_MAX pads, keyed by the engine joypad id; each pad
 *     keeps its own physical truth (control bitmask, six axes);
 *   - a symbolic control is DOWN when ANY admitted pad holds it (union);
 *   - an axis takes the largest-magnitude value across pads (a resting pad
 *     never cancels another);
 *   - the exit chord is SAME-PAD only: SELECT here + START there is a denial,
 *     reported exactly once per occurrence (not every frame);
 *   - removal compacts the set; a re-added pad is admitted again.
 *
 * The unit has no Godot or SDL dependency: control bits and axis indices are
 * the caller's numbering, so the host gate in tests/controls proves the
 * policy without a device. The GPTK decision semantics, the context policy
 * and the geometry code are untouched by this unit. */

#include <stdbool.h>
#include <stdint.h>

enum {
	TEARS_PADSET_MAX = 4,
	TEARS_PADSET_AXIS_LEFT_X = 0,
	TEARS_PADSET_AXIS_LEFT_Y = 1,
	TEARS_PADSET_AXIS_RIGHT_X = 2,
	TEARS_PADSET_AXIS_RIGHT_Y = 3,
	TEARS_PADSET_AXIS_LEFT_TRIGGER = 4,
	TEARS_PADSET_AXIS_RIGHT_TRIGGER = 5,
	TEARS_PADSET_AXES = 6,
};

enum TearsPadsetChord {
	/* No pad holds SELECT and START together. */
	TEARS_PADSET_CHORD_NONE = 0,
	/* ONE pad holds both: the sovereign chord (owned by the joypad driver). */
	TEARS_PADSET_CHORD_SAME_PAD = 1,
	/* SELECT and START are held, but only on DIFFERENT pads: denied. */
	TEARS_PADSET_CHORD_CROSS_PAD = 2,
};

struct TearsPadsetPad {
	int joy_id;
	int instance_id;
	uint32_t down_controls;
	float axes[TEARS_PADSET_AXES];
};

struct TearsPadset {
	TearsPadsetPad pads[TEARS_PADSET_MAX];
	unsigned count;
	uint32_t select_bit;
	uint32_t start_bit;
	/* A cross-pad occurrence is logged once, then re-armed only after the
	 * SELECT+START overlap dissolves. */
	bool cross_pad_logged;
	unsigned cross_pad_denials;
};

/* The chord bits are the caller's control bits for SELECT and START. */
void tears_padset_init(TearsPadset *p_set, uint32_t p_select_bit, uint32_t p_start_bit);

/* Slot of an admitted joypad id, or -1. */
int tears_padset_slot(const TearsPadset *p_set, int p_joy_id);

/* Admit a pad. Returns its slot; an already admitted id returns its existing
 * slot (state untouched); -1 when the set is full or the id is invalid.
 * A newly admitted pad starts with no control down and every axis at rest. */
int tears_padset_admit(TearsPadset *p_set, int p_joy_id, int p_instance_id);

/* Remove one pad and compact the remaining slots. Returns false if the id
 * was not admitted. */
bool tears_padset_remove(TearsPadset *p_set, int p_joy_id);

/* Replace one pad's complete physical truth (a full SDL snapshot). Returns
 * false when the id is not admitted. */
bool tears_padset_set_snapshot(TearsPadset *p_set, int p_joy_id,
		uint32_t p_down_controls, const float p_axes[TEARS_PADSET_AXES]);

/* One control transition on one pad. Returns false when the id is not
 * admitted (the caller leaves such a pad fully native). */
bool tears_padset_set_control(TearsPadset *p_set, int p_joy_id, uint32_t p_bit, bool p_down);

/* One axis sample on one pad. Same admission contract. */
bool tears_padset_set_axis(TearsPadset *p_set, int p_joy_id, int p_axis, float p_value);

/* Union of the admitted pads' control bits. */
uint32_t tears_padset_union_controls(const TearsPadset *p_set);

/* Largest-magnitude value of one axis across the admitted pads (0 when the
 * set is empty or the axis index is invalid). */
float tears_padset_axis(const TearsPadset *p_set, int p_axis);

/* Evaluate the SELECT+START relationship across the set. When the result is
 * CROSS_PAD and this occurrence has not been reported yet, *r_log_now is set
 * to true exactly once (the caller prints tears_padset_cross_pad_denial()).
 * The occurrence re-arms only when SELECT and START are no longer both held
 * somewhere in the set. */
TearsPadsetChord tears_padset_chord(TearsPadset *p_set, bool *r_log_now);

/* The exact denial text the framework proof looks for (no prefix). */
const char *tears_padset_cross_pad_denial();

#endif /* TEARSCAPE_PADSET_H */
