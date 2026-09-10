/* SPDX-License-Identifier: GPL-3.0-only */
/* TEARSCAPE 0.2.17: the logical-player pad set. See tearscape_padset.h.
 * No Godot or SDL header: this file compiles and is gated anywhere. */

#include "tearscape_padset.h"

#include <string.h>

static const char tears_padset_denial_text[] =
		"chord denied: SELECT and START on different pads (cross-pad)";

const char *tears_padset_cross_pad_denial() {
	return tears_padset_denial_text;
}

void tears_padset_init(TearsPadset *p_set, uint32_t p_select_bit, uint32_t p_start_bit) {
	if (!p_set) {
		return;
	}
	memset(p_set, 0, sizeof(*p_set));
	p_set->select_bit = p_select_bit;
	p_set->start_bit = p_start_bit;
}

int tears_padset_slot(const TearsPadset *p_set, int p_joy_id) {
	if (!p_set || p_joy_id < 0) {
		return -1;
	}
	for (unsigned slot = 0; slot < p_set->count; slot++) {
		if (p_set->pads[slot].joy_id == p_joy_id) {
			return (int)slot;
		}
	}
	return -1;
}

int tears_padset_admit(TearsPadset *p_set, int p_joy_id, int p_instance_id) {
	if (!p_set || p_joy_id < 0) {
		return -1;
	}
	const int existing = tears_padset_slot(p_set, p_joy_id);
	if (existing >= 0) {
		return existing;
	}
	if (p_set->count >= (unsigned)TEARS_PADSET_MAX) {
		return -1;
	}
	TearsPadsetPad &pad = p_set->pads[p_set->count];
	memset(&pad, 0, sizeof(pad));
	pad.joy_id = p_joy_id;
	pad.instance_id = p_instance_id;
	p_set->count++;
	return (int)p_set->count - 1;
}

bool tears_padset_remove(TearsPadset *p_set, int p_joy_id) {
	const int slot = tears_padset_slot(p_set, p_joy_id);
	if (slot < 0) {
		return false;
	}
	for (unsigned next = (unsigned)slot; next + 1u < p_set->count; next++) {
		p_set->pads[next] = p_set->pads[next + 1u];
	}
	p_set->count--;
	memset(&p_set->pads[p_set->count], 0, sizeof(p_set->pads[0]));
	return true;
}

bool tears_padset_set_snapshot(TearsPadset *p_set, int p_joy_id,
		uint32_t p_down_controls, const float p_axes[TEARS_PADSET_AXES]) {
	const int slot = tears_padset_slot(p_set, p_joy_id);
	if (slot < 0) {
		return false;
	}
	TearsPadsetPad &pad = p_set->pads[slot];
	pad.down_controls = p_down_controls;
	for (int axis = 0; axis < TEARS_PADSET_AXES; axis++) {
		pad.axes[axis] = p_axes ? p_axes[axis] : 0.0f;
	}
	return true;
}

bool tears_padset_set_control(TearsPadset *p_set, int p_joy_id, uint32_t p_bit, bool p_down) {
	const int slot = tears_padset_slot(p_set, p_joy_id);
	if (slot < 0) {
		return false;
	}
	if (p_down) {
		p_set->pads[slot].down_controls |= p_bit;
	} else {
		p_set->pads[slot].down_controls &= ~p_bit;
	}
	return true;
}

bool tears_padset_set_axis(TearsPadset *p_set, int p_joy_id, int p_axis, float p_value) {
	const int slot = tears_padset_slot(p_set, p_joy_id);
	if (slot < 0 || p_axis < 0 || p_axis >= TEARS_PADSET_AXES) {
		return false;
	}
	p_set->pads[slot].axes[p_axis] = p_value;
	return true;
}

uint32_t tears_padset_union_controls(const TearsPadset *p_set) {
	uint32_t down = 0;
	if (!p_set) {
		return 0;
	}
	for (unsigned slot = 0; slot < p_set->count; slot++) {
		down |= p_set->pads[slot].down_controls;
	}
	return down;
}

float tears_padset_axis(const TearsPadset *p_set, int p_axis) {
	float best = 0.0f;
	if (!p_set || p_axis < 0 || p_axis >= TEARS_PADSET_AXES) {
		return 0.0f;
	}
	for (unsigned slot = 0; slot < p_set->count; slot++) {
		const float value = p_set->pads[slot].axes[p_axis];
		const float magnitude = value < 0.0f ? -value : value;
		const float best_magnitude = best < 0.0f ? -best : best;
		if (magnitude > best_magnitude) {
			best = value;
		}
	}
	return best;
}

TearsPadsetChord tears_padset_chord(TearsPadset *p_set, bool *r_log_now) {
	if (r_log_now) {
		*r_log_now = false;
	}
	if (!p_set) {
		return TEARS_PADSET_CHORD_NONE;
	}
	bool same_pad = false;
	bool any_select = false;
	bool any_start = false;
	for (unsigned slot = 0; slot < p_set->count; slot++) {
		const uint32_t down = p_set->pads[slot].down_controls;
		const bool select = (down & p_set->select_bit) != 0u;
		const bool start = (down & p_set->start_bit) != 0u;
		if (select && start) {
			same_pad = true;
		}
		any_select = any_select || select;
		any_start = any_start || start;
	}
	if (same_pad) {
		/* The sovereign chord is a same-pad fact; a simultaneous cross-pad
		 * overlap adds nothing and is not a denial. */
		return TEARS_PADSET_CHORD_SAME_PAD;
	}
	if (any_select && any_start) {
		if (!p_set->cross_pad_logged) {
			p_set->cross_pad_logged = true;
			p_set->cross_pad_denials++;
			if (r_log_now) {
				*r_log_now = true;
			}
		}
		return TEARS_PADSET_CHORD_CROSS_PAD;
	}
	p_set->cross_pad_logged = false;
	return TEARS_PADSET_CHORD_NONE;
}
