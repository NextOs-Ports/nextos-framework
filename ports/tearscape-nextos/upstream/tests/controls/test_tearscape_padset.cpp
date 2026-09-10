/* SPDX-License-Identifier: GPL-3.0-only */
/* Host gate for the logical-player pad set (tearscape_padset.{h,cpp}):
 * union of controls, max-magnitude axes, same-pad chord, cross-pad denial
 * logged once per occurrence, removal compaction, cap at 4 pads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tearscape_padset.h"

static void expect(bool p_condition, const char *p_message) {
	if (!p_condition) {
		fprintf(stderr, "TEARSCAPE PADSET: FAIL: %s\n", p_message);
		exit(1);
	}
}

/* The glue's control numbering (nxinput_gptk_control): bit = 1u << control. */
enum {
	BIT_A = 1u << 0,
	BIT_B = 1u << 1,
	BIT_X = 1u << 2,
	BIT_SELECT = 1u << 8,
	BIT_START = 1u << 9,
	BIT_L2 = 1u << 6,
};

int main() {
	TearsPadset set;
	tears_padset_init(&set, BIT_SELECT, BIT_START);
	expect(set.count == 0 && tears_padset_union_controls(&set) == 0u &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_X) == 0.0f,
			"an empty set has nothing down and every axis at rest");
	expect(strcmp(tears_padset_cross_pad_denial(),
					"chord denied: SELECT and START on different pads (cross-pad)") == 0,
			"the denial text is the exact substring the framework proof parses");

	/* --- admission, identity, cap -------------------------------------- */
	expect(tears_padset_admit(&set, -1, 10) == -1, "a negative joypad id is refused");
	expect(tears_padset_admit(&set, 0, 100) == 0, "the first pad takes slot 0");
	expect(tears_padset_admit(&set, 0, 100) == 0 && set.count == 1,
			"re-admitting an admitted id returns its slot without duplicating it");
	expect(tears_padset_admit(&set, 1, 101) == 1, "the second pad takes slot 1");
	expect(tears_padset_admit(&set, 2, 102) == 2, "the third pad takes slot 2");
	expect(tears_padset_admit(&set, 3, 103) == 3, "the fourth pad takes slot 3");
	expect(tears_padset_admit(&set, 4, 104) == -1 && set.count == 4,
			"a fifth pad is refused: the set is capped at 4");
	expect(tears_padset_slot(&set, 4) == -1 && !tears_padset_set_control(&set, 4, BIT_A, true) &&
					!tears_padset_set_axis(&set, 4, TEARS_PADSET_AXIS_LEFT_X, 1.0f),
			"a refused pad cannot feed the set");
	expect(set.pads[2].instance_id == 102, "a slot remembers its SDL instance id");

	/* --- union of controls --------------------------------------------- */
	expect(tears_padset_set_control(&set, 0, BIT_A, true) &&
					tears_padset_union_controls(&set) == BIT_A,
			"single-pad press is the union");
	expect(tears_padset_set_control(&set, 1, BIT_A, true) &&
					tears_padset_union_controls(&set) == BIT_A,
			"a second pad holding the same control does not change the union");
	expect(tears_padset_set_control(&set, 0, BIT_A, false) &&
					tears_padset_union_controls(&set) == BIT_A,
			"releasing on one pad keeps the control down while another holds it");
	expect(tears_padset_set_control(&set, 1, BIT_A, false) &&
					tears_padset_union_controls(&set) == 0u,
			"the union releases only when every holder released");
	expect(tears_padset_set_control(&set, 2, BIT_B, true) &&
					tears_padset_set_control(&set, 3, BIT_X, true) &&
					tears_padset_union_controls(&set) == (BIT_B | BIT_X),
			"different controls on different pads are OR-ed together");
	tears_padset_set_control(&set, 2, BIT_B, false);
	tears_padset_set_control(&set, 3, BIT_X, false);

	/* --- max-magnitude axes -------------------------------------------- */
	expect(tears_padset_set_axis(&set, 0, TEARS_PADSET_AXIS_LEFT_X, 0.30f) &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_X) == 0.30f,
			"single-pad axis is the aggregate");
	expect(tears_padset_set_axis(&set, 1, TEARS_PADSET_AXIS_LEFT_X, -0.90f) &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_X) == -0.90f,
			"the largest magnitude wins and keeps its sign");
	expect(tears_padset_set_axis(&set, 2, TEARS_PADSET_AXIS_LEFT_X, 0.0f) &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_X) == -0.90f,
			"a resting pad never cancels another pad's deflection");
	expect(tears_padset_set_axis(&set, 1, TEARS_PADSET_AXIS_LEFT_X, 0.0f) &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_X) == 0.30f,
			"when the strongest pad rests the next strongest takes over");
	expect(tears_padset_set_axis(&set, 3, TEARS_PADSET_AXIS_RIGHT_TRIGGER, 0.75f) &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_RIGHT_TRIGGER) == 0.75f &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_TRIGGER) == 0.0f,
			"triggers aggregate per axis independently");
	expect(!tears_padset_set_axis(&set, 0, TEARS_PADSET_AXES, 1.0f) &&
					!tears_padset_set_axis(&set, 0, -1, 1.0f) &&
					tears_padset_axis(&set, TEARS_PADSET_AXES) == 0.0f,
			"an axis index outside the six is refused");
	tears_padset_set_axis(&set, 0, TEARS_PADSET_AXIS_LEFT_X, 0.0f);
	tears_padset_set_axis(&set, 3, TEARS_PADSET_AXIS_RIGHT_TRIGGER, 0.0f);

	/* --- snapshot replaces one pad's truth ------------------------------ */
	{
		const float axes[TEARS_PADSET_AXES] = { 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
		expect(tears_padset_set_snapshot(&set, 1, BIT_L2, axes) &&
						tears_padset_union_controls(&set) == BIT_L2 &&
						tears_padset_axis(&set, TEARS_PADSET_AXIS_LEFT_X) == 0.5f &&
						tears_padset_axis(&set, TEARS_PADSET_AXIS_RIGHT_TRIGGER) == 1.0f,
				"a snapshot installs a pad's held controls and axes at once");
		expect(!tears_padset_set_snapshot(&set, 9, BIT_L2, axes),
				"a snapshot for an unknown pad is refused");
		expect(tears_padset_set_snapshot(&set, 1, 0u, nullptr) &&
						tears_padset_union_controls(&set) == 0u &&
						tears_padset_axis(&set, TEARS_PADSET_AXIS_RIGHT_TRIGGER) == 0.0f,
				"a null axis snapshot rests every axis of that pad");
	}

	/* --- chord: same pad vs cross pad ---------------------------------- */
	bool log_now = true;
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_NONE && !log_now,
			"nothing held is no chord and no denial");
	tears_padset_set_control(&set, 0, BIT_SELECT, true);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_NONE && !log_now,
			"a lone SELECT is a plain button");
	tears_padset_set_control(&set, 0, BIT_START, true);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_SAME_PAD && !log_now,
			"SELECT+START on the SAME pad is the sovereign chord, never a denial");
	tears_padset_set_control(&set, 0, BIT_START, false);
	tears_padset_set_control(&set, 0, BIT_SELECT, false);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_NONE && !log_now,
			"releasing the same-pad chord returns to none");

	tears_padset_set_control(&set, 0, BIT_SELECT, true);
	tears_padset_set_control(&set, 1, BIT_START, true);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_CROSS_PAD && log_now &&
					set.cross_pad_denials == 1,
			"SELECT on pad 0 + START on pad 1 is denied and logged");
	expect(tears_padset_union_controls(&set) == (BIT_SELECT | BIT_START),
			"the denied chord's plain buttons still reach the union");
	for (int frame = 0; frame < 5; frame++) {
		expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_CROSS_PAD && !log_now &&
						set.cross_pad_denials == 1,
				"the same cross-pad occurrence is not logged again every frame");
	}
	tears_padset_set_control(&set, 2, BIT_START, true);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_CROSS_PAD && !log_now &&
					set.cross_pad_denials == 1,
			"a third pad joining the overlap is still the same occurrence");
	tears_padset_set_control(&set, 0, BIT_START, true);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_SAME_PAD && !log_now,
			"one pad completing the chord itself wins over the cross-pad overlap");
	tears_padset_set_control(&set, 0, BIT_START, false);
	tears_padset_set_control(&set, 1, BIT_START, false);
	tears_padset_set_control(&set, 2, BIT_START, false);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_NONE && !log_now,
			"START released everywhere dissolves the overlap");
	tears_padset_set_control(&set, 1, BIT_START, true);
	expect(tears_padset_chord(&set, &log_now) == TEARS_PADSET_CHORD_CROSS_PAD && log_now &&
					set.cross_pad_denials == 2,
			"a new cross-pad occurrence after a gap is logged once more");
	tears_padset_set_control(&set, 0, BIT_SELECT, false);
	tears_padset_set_control(&set, 1, BIT_START, false);
	expect(tears_padset_chord(&set, nullptr) == TEARS_PADSET_CHORD_NONE,
			"a null log pointer is accepted");

	/* --- removal compaction and re-admission ---------------------------- */
	tears_padset_set_control(&set, 3, BIT_X, true);
	tears_padset_set_axis(&set, 3, TEARS_PADSET_AXIS_RIGHT_Y, -0.6f);
	expect(!tears_padset_remove(&set, 7), "removing an unknown id is a no-op");
	expect(tears_padset_remove(&set, 1) && set.count == 3 &&
					tears_padset_slot(&set, 2) == 1 && tears_padset_slot(&set, 3) == 2 &&
					set.pads[1].instance_id == 102 && set.pads[2].instance_id == 103 &&
					set.pads[3].joy_id == 0 && set.pads[3].instance_id == 0,
			"removing slot 1 compacts the higher slots and clears the tail");
	expect(tears_padset_union_controls(&set) == BIT_X &&
					tears_padset_axis(&set, TEARS_PADSET_AXIS_RIGHT_Y) == -0.6f,
			"the remaining pads keep their held controls and axes across compaction");
	tears_padset_set_control(&set, 3, BIT_X, false);
	tears_padset_set_axis(&set, 3, TEARS_PADSET_AXIS_RIGHT_Y, 0.0f);
	expect(tears_padset_admit(&set, 1, 110) == 3 && set.count == 4 &&
					set.pads[3].down_controls == 0u &&
					set.pads[3].axes[TEARS_PADSET_AXIS_RIGHT_Y] == 0.0f,
			"a re-added pad is admitted again into the freed slot, at rest");
	expect(tears_padset_remove(&set, 1) && tears_padset_admit(&set, 4, 104) == 3,
			"after a removal the previously refused fifth pad fits");
	expect(tears_padset_remove(&set, 0) && tears_padset_remove(&set, 2) &&
					tears_padset_remove(&set, 3) && tears_padset_remove(&set, 4) &&
					set.count == 0 && tears_padset_union_controls(&set) == 0u,
			"removing every pad empties the set");

	puts("TEARSCAPE PADSET: PASS admission=10 union=6 axes=7 snapshot=3 chord=13 removal=6");
	return 0;
}
