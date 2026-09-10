/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxinput_godot_runtime.h"
#include "tearscape_gptk_context_policy.h"
#include "tearscape_gptk_input_policy.h"

static void expect(int condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "TEARSCAPE GPTK CONTEXT: FAIL: %s\n", message);
		exit(1);
	}
}

enum {
	TEST_CONTROL_A = 0,
	TEST_CONTROL_X = 2,
	TEST_CONTROL_R2 = 7,
	TEST_CONTROL_LEFT_STICK = 16,
	TEST_CONTROL_RIGHT_STICK = 17,
};

int main(void) {
	nxinput_godot_action_latch attack_latch = { 0 };
	nxinput_godot_neutral_handoff handoff = { 0 };
	nxinput_godot_neutral_handoff native_handoff = { 0 };
	nxinput_godot_neutral_handoff suppressed_handoff = { 0 };
	nxinput_godot_vector_alias vector_alias = { 0 };
	nxinput_godot_lifecycle lifecycle = { 0 };
	float left_x = 0.8f;
	float left_y = -0.4f;
	float right_x = 0.7f;
	float right_y = 0.3f;
	float strengths[4] = {};
	float aggregate[4] = {};
	expect(tears_gptk_context_for_scene("res://scenes/ui/screens/main_menu.tscn", false) ==
			TEARS_GPTK_CONTEXT_MENU, "main menu without GUI focus must stay menu");
	expect(tears_gptk_context_for_scene("res://scenes/map.tscn", false) ==
			TEARS_GPTK_CONTEXT_GAMEPLAY, "map root must be gameplay");
	expect(tears_gptk_context_for_scene("res://scenes/level/world/world_1.tscn", false) ==
			TEARS_GPTK_CONTEXT_GAMEPLAY, "level root must be gameplay");
	expect(tears_gptk_context_for_scene("res://scenes/map.tscn", true) ==
			TEARS_GPTK_CONTEXT_MENU, "paused UiManager overlay over gameplay must be menu");
	expect(tears_gptk_context_for_scene("res://scenes/level/world/world_1.tscn", true) ==
			TEARS_GPTK_CONTEXT_MENU, "paused UiManager overlay over a level must be menu");
	expect(tears_gptk_context_for_scene("res://scenes/ui/screens/main_menu.tscn", true) ==
			TEARS_GPTK_CONTEXT_MENU, "UI scene remains menu when the tree is paused");
	expect(tears_gptk_context_for_scene("res://unknown/custom.tscn", false) ==
			TEARS_GPTK_CONTEXT_UNPROVEN, "unknown scene must fail safe to native");
	expect(tears_gptk_context_for_scene("res://unknown/custom.tscn", true) ==
			TEARS_GPTK_CONTEXT_UNPROVEN, "paused unknown scene must still fail safe to native");
	expect(tears_gptk_context_for_scene(NULL, false) ==
			TEARS_GPTK_CONTEXT_UNPROVEN, "absent scene must fail safe to native");
	expect(tears_gptk_scene_requires_native_start_coop(
			"res://scenes/ui/screens/start_game_screen.tscn"),
			"start screen keeps physical X/WEST native for P1/P2 identity");
	expect(!tears_gptk_scene_requires_native_start_coop(
			"res://scenes/ui/screens/main_menu.tscn"),
			"other menus remain governed by the editable mapping");
	expect(strcmp(tears_gptk_context_source_for_scene(
			"res://scenes/ui/screens/main_menu.tscn",
			TEARS_GPTK_CONTEXT_MENU, false), "scene:ui") == 0,
			"ordinary UI keeps the generic menu authority epoch");
	expect(strcmp(tears_gptk_context_source_for_scene(
			"res://scenes/ui/screens/start_game_screen.tscn",
			TEARS_GPTK_CONTEXT_MENU, false), "scene:ui:start-coop") == 0,
			"start-coop has a distinct physical-X authority epoch");
	expect(strcmp(tears_gptk_context_source_for_scene(
			"res://scenes/ui/screens/main_menu.tscn",
			TEARS_GPTK_CONTEXT_MENU, false),
			tears_gptk_context_source_for_scene(
					"res://scenes/ui/screens/start_game_screen.tscn",
					TEARS_GPTK_CONTEXT_MENU, false)) != 0,
			"entering start-coop must trigger the existing context handoff");

	/* X pressed under an editable owner remains suppressed through the release
	 * when start-coop takes native ownership; no orphan native release leaks. */
	nxinput_godot_handoff_partition(&native_handoff, &suppressed_handoff,
			nxinput_godot_control_bit(TEST_CONTROL_X),
			nxinput_godot_control_bit(TEST_CONTROL_X),
			TEST_CONTROL_LEFT_STICK, 0.0f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.0f, 0.0f);
	expect((native_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_X)) == 0u &&
			(suppressed_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_X)) != 0u &&
			(native_handoff.controls & suppressed_handoff.controls) == 0u,
			"governed X keeps exactly the suppressed release owner on entry");
	expect(nxinput_godot_handoff_button(
			&suppressed_handoff, TEST_CONTROL_X, false) &&
			!nxinput_godot_handoff_button(&native_handoff, TEST_CONTROL_X, false) &&
			native_handoff.controls == 0u && suppressed_handoff.controls == 0u,
			"governed X release is consumed once and both masks return neutral");

	/* X born native inside start-coop keeps that owner if the scene changes
	 * again before release, even when the next mapping would govern X. */
	native_handoff.controls = nxinput_godot_control_bit(TEST_CONTROL_X);
	nxinput_godot_handoff_partition(&native_handoff, &suppressed_handoff,
			nxinput_godot_control_bit(TEST_CONTROL_X),
			nxinput_godot_control_bit(TEST_CONTROL_X),
			TEST_CONTROL_LEFT_STICK, 0.0f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.0f, 0.0f);
	expect((native_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_X)) != 0u &&
			(suppressed_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_X)) == 0u &&
			(native_handoff.controls & suppressed_handoff.controls) == 0u,
			"native start-coop X remains native across a second transition");
	expect(nxinput_godot_handoff_button(&native_handoff, TEST_CONTROL_X, false) &&
			!nxinput_godot_handoff_button(
					&suppressed_handoff, TEST_CONTROL_X, false) &&
			native_handoff.controls == 0u && suppressed_handoff.controls == 0u,
			"native start-coop X release reaches Godot once and clears both masks");

	/* TEARSCAPE 0.2.17: the pad policy moved to tearscape_padset.{h,cpp}
	 * (every admitted pad is one logical player); its gate is
	 * test_tearscape_padset.cpp. */

	/* Device disconnect is the only boundary that destroys physical axis
	 * truth. Co-op transitions preserve it for the neutral-handoff test below. */
	tears_gptk_clear_axis_cache(&left_x, &left_y, &right_x, &right_y);
	expect(left_x == 0.0f && left_y == 0.0f && right_x == 0.0f && right_y == 0.0f,
			"primary disconnect clears every physical vector cache");

	nxinput_godot_split_vector(0.20f, -0.20f, strengths);
	expect(strengths[0] == 0.20f && strengths[1] == 0.0f &&
			strengths[2] == 0.0f && strengths[3] == 0.20f,
			"adapter preserves sub-0.25 analog components for Godot's own deadzone");
	nxinput_godot_split_vector(-1.0f, 1.0f, strengths);
	expect(strengths[0] == 0.0f && strengths[1] == 1.0f &&
			strengths[2] == 1.0f && strengths[3] == 0.0f,
			"adapter preserves full directional strengths without rescaling");
	expect(nxinput_godot_vector_alias_update(
			&vector_alias, 0, 0.8f, 0.0f, aggregate) == 0 &&
			aggregate[3] == 0.8f,
			"left stick establishes the shared player.move strength");
	expect(nxinput_godot_vector_alias_update(
			&vector_alias, 1, 0.0f, 0.0f, aggregate) == 0 &&
			aggregate[3] == 0.8f,
			"neutral right-stick alias cannot release left-stick movement");
	expect(nxinput_godot_vector_alias_update(
			&vector_alias, 1, 0.4f, -0.6f, aggregate) == 0 &&
			aggregate[0] == 0.6f && aggregate[3] == 0.8f,
			"two stick aliases aggregate independently by direction");
	expect(nxinput_godot_vector_alias_update(
			&vector_alias, 0, 0.0f, 0.0f, aggregate) == 0 &&
			aggregate[0] == 0.6f && aggregate[3] == 0.4f,
			"centering one alias preserves the other held stick");
	nxinput_godot_vector_alias_clear(&vector_alias);

	expect(nxinput_godot_action_preview(&attack_latch, true) ==
			NXINPUT_GODOT_ACTION_DELIVER,
			"first alias press reaches the real action sink");
	expect(nxinput_godot_action_commit(&attack_latch, true) == 0,
			"first alias press commits after ACK");
	expect(nxinput_godot_action_preview(&attack_latch, true) ==
			NXINPUT_GODOT_ACTION_ACK_ONLY,
			"second attack alias joins the semantic OR without a duplicate press");
	expect(nxinput_godot_action_commit(&attack_latch, true) == 0,
			"second alias press increments the semantic OR");
	expect(nxinput_godot_action_preview(&attack_latch, false) ==
			NXINPUT_GODOT_ACTION_ACK_ONLY,
			"releasing B while R2 is held cannot release player.attack");
	expect(nxinput_godot_action_commit(&attack_latch, false) == 0,
			"first alias release keeps the action held");
	expect(nxinput_godot_action_preview(&attack_latch, false) ==
			NXINPUT_GODOT_ACTION_DELIVER,
			"last alias release reaches the real action sink");
	expect(nxinput_godot_action_commit(&attack_latch, false) == 0 &&
			attack_latch.held_count == 0,
			"semantic OR returns to neutral only after the final release");
	expect(nxinput_godot_action_preview(&attack_latch, false) ==
			NXINPUT_GODOT_ACTION_INVALID,
			"unmatched release fails closed");

	nxinput_godot_handoff_snapshot(&handoff,
			nxinput_godot_control_bit(TEST_CONTROL_A) |
					nxinput_godot_control_bit(TEST_CONTROL_R2),
			TEST_CONTROL_LEFT_STICK, 0.70f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.0f, 0.0f);
	expect(nxinput_godot_handoff_button(&handoff, TEST_CONTROL_A, true),
			"held native button remains passthrough after context proof");
	expect(nxinput_godot_handoff_button(&handoff, TEST_CONTROL_A, false),
			"matching button release remains native and clears its barrier");
	expect(!nxinput_godot_handoff_button(&handoff, TEST_CONTROL_A, true),
			"next button press may be governed after neutral handoff");
	expect(nxinput_godot_handoff_vector(&handoff, TEST_CONTROL_LEFT_STICK, 0.25f, 0.0f),
			"held native stick remains passthrough while nonneutral");
	expect(nxinput_godot_handoff_vector(&handoff, TEST_CONTROL_LEFT_STICK, 0.05f, 0.02f),
			"ordinary released-stick drift remains native and clears the barrier");
	expect(!nxinput_godot_handoff_vector(&handoff, TEST_CONTROL_LEFT_STICK, 0.50f, 0.0f),
			"next stick gesture may be governed after center");
	expect(nxinput_godot_handoff_button(&handoff, TEST_CONTROL_R2, false),
			"held native trigger release also crosses the barrier natively");

	/* Menu A/left stick are governed, while X/right stick are native. A scene
	 * transition must keep those distinct until release/center; a second scene
	 * transition before neutral must not steal either gesture. */
	nxinput_godot_handoff_partition(&native_handoff, &suppressed_handoff,
			nxinput_godot_control_bit(TEST_CONTROL_A) |
					nxinput_godot_control_bit(TEST_CONTROL_X),
			nxinput_godot_control_bit(TEST_CONTROL_A) |
					nxinput_godot_control_bit(TEST_CONTROL_LEFT_STICK),
			TEST_CONTROL_LEFT_STICK, 0.7f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.6f, 0.0f);
	expect((native_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_X)) != 0u &&
			(native_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_RIGHT_STICK)) != 0u,
			"menu-native X/right stick keep their native releases");
	expect((suppressed_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_A)) != 0u &&
			(suppressed_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_LEFT_STICK)) != 0u,
			"menu-governed A/left stick cannot enter gameplay half-held");
	expect((native_handoff.controls & suppressed_handoff.controls) == 0u,
			"held controls have exactly one transition owner");
	nxinput_godot_handoff_partition(&native_handoff, &suppressed_handoff,
			nxinput_godot_control_bit(TEST_CONTROL_A) |
					nxinput_godot_control_bit(TEST_CONTROL_X),
			UINT32_MAX,
			TEST_CONTROL_LEFT_STICK, 0.7f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.6f, 0.0f);
	expect((native_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_X)) != 0u &&
			(suppressed_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_A)) != 0u,
			"second transition preserves the first gesture authority");
	expect(nxinput_godot_handoff_button(&native_handoff, TEST_CONTROL_X, false),
			"native X release reaches Godot and clears its barrier");
	expect(nxinput_godot_handoff_button(&suppressed_handoff, TEST_CONTROL_A, false),
			"governed A release is consumed and clears its barrier");
	expect(nxinput_godot_handoff_vector(
			&native_handoff, TEST_CONTROL_RIGHT_STICK, 0.0f, 0.0f),
			"native right-stick center reaches Godot");
	expect(nxinput_godot_handoff_vector(
			&suppressed_handoff, TEST_CONTROL_LEFT_STICK, 0.0f, 0.0f),
			"governed left-stick center is consumed");
	expect(native_handoff.controls == 0u && suppressed_handoff.controls == 0u,
			"both transition masks return to neutral");

	/* Hotplug promotion starts from a complete snapshot: input that began on
	 * the native P2 path must finish there after that pad becomes P1. */
	nxinput_godot_neutral_handoff promotion_handoff = { 0 };
	nxinput_godot_neutral_handoff promotion_suppressed = { 0 };
	const uint32_t promotion_down = nxinput_godot_control_bit(TEST_CONTROL_A);
	nxinput_godot_handoff_snapshot(&promotion_handoff, promotion_down,
			TEST_CONTROL_LEFT_STICK, 0.70f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.0f, 0.0f);
	nxinput_godot_handoff_partition(&promotion_handoff, &promotion_suppressed,
			promotion_down, UINT32_MAX,
			TEST_CONTROL_LEFT_STICK, 0.70f, 0.0f,
			TEST_CONTROL_RIGHT_STICK, 0.0f, 0.0f);
	expect((promotion_handoff.controls & promotion_down) != 0u &&
			(promotion_handoff.controls & nxinput_godot_control_bit(TEST_CONTROL_LEFT_STICK)) != 0u &&
			(promotion_handoff.controls & promotion_suppressed.controls) == 0u,
			"promoted held button/stick remain exclusively native");
	expect(nxinput_godot_handoff_button(
			&promotion_handoff, TEST_CONTROL_A, false),
			"promoted held button release reaches native path");
	expect(!nxinput_godot_handoff_button(
			&promotion_handoff, TEST_CONTROL_A, true),
			"new button gesture may be governed after promoted release");
	expect(nxinput_godot_handoff_vector(
			&promotion_handoff, TEST_CONTROL_LEFT_STICK, 0.40f, 0.0f),
			"promoted stick stays native while nonneutral");
	expect(nxinput_godot_handoff_vector(
			&promotion_handoff, TEST_CONTROL_LEFT_STICK, 0.0f, 0.0f),
			"promoted stick center reaches native path");
	expect(!nxinput_godot_handoff_vector(
			&promotion_handoff, TEST_CONTROL_LEFT_STICK, 0.50f, 0.0f) &&
			promotion_handoff.controls == 0u,
			"new stick gesture may be governed after promoted center");

	expect(nxinput_godot_lifecycle_health_allowed(&lifecycle),
			"healthy adapter may publish runtime health");
	nxinput_godot_lifecycle_fail(&lifecycle);
	expect(nxinput_godot_lifecycle_consume_close(&lifecycle) &&
			!nxinput_godot_lifecycle_consume_close(&lifecycle),
			"fatal close request is consumed exactly once");
	expect(!nxinput_godot_lifecycle_health_allowed(&lifecycle) &&
			nxinput_godot_lifecycle_exit_status(&lifecycle) != 0,
			"fatal remains health-blocking and nonzero after close consumption");
	expect(strcmp(nxinput_godot_runtime_marker(), "nxinput-godot-runtime/1") == 0,
			"Godot runtime marker matches the pinned framework core");

	expect(!tears_gptk_edge_next(false, 0.54f, 0.0f), "zoom edge stays up below press threshold");
	expect(tears_gptk_edge_next(false, 0.56f, 0.0f), "zoom edge presses once above threshold");
	expect(tears_gptk_edge_next(true, 0.90f, 0.0f), "held zoom gesture does not retrigger");
	expect(tears_gptk_edge_next(true, 0.31f, 0.0f), "zoom hysteresis ignores held jitter");
	expect(!tears_gptk_edge_next(true, 0.29f, 0.0f), "zoom edge releases below threshold");
	expect(tears_gptk_edge_next(false, 0.40f, 0.40f), "zoom edge uses radial magnitude");

	puts("TEARSCAPE GPTK POLICY: PASS context=9 start-coop=9 pads=see-padset-gate disconnect-cache=1 analog=2 vector-alias=4 aliases=10 handoff=7 partitions=17 lifecycle=4 zoom-edge=6");
	return 0;
}
