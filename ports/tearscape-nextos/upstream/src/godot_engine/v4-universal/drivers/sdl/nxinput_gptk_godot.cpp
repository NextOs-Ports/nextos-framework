/* SPDX-License-Identifier: GPL-3.0-only */
/* TEARSCAPE-CONTROLS-LIVE: NEXTOSCONTROLLERS.gptk live runtime for Godot.
 * See nxinput_gptk_godot.h for the contract. */

#include "nxinput_gptk_godot.h"
#include "tearscape_gptk_context_policy.h"
#include "tearscape_gptk_input_policy.h"
#include "tearscape_gptk_receipt.h"
#include "tearscape_padset.h"

#ifdef FBDEV_ENABLED

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/input/input_map.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include <SDL3/SDL_gamepad.h>

#include <cstdio>

#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "joystick/linux/nxinput_godot_runtime.h"
#include "joystick/linux/nxinput_gptk.h"
#include "joystick/linux/nxinput_gptk_live.h"
#include "joystick/linux/nxinput_gptk_preinit.h"
#include "joystick/linux/nxinput_gptk_loader.h"
#include "joystick/linux/nxinput_gptk4.h"
#include "joystick/linux/nxinput_gptk4_bridge.h"
#include "joystick/linux/nxinput_gptk4_preinit.h"
}

/* The EXACT adapter-contract action allowlist. test_runtime_contract.py
 * keeps this table equal to the actions in nxproject.json / the generated
 * adapter-contract.json; an action added on one side without the other
 * fails the source gate before any build. */
static const char *const nxgptk_allowed_actions[] = {
	"menu.accept",
	"menu.back",
	"menu.navigate",
	"player.attack",
	"player.heal",
	"player.move",
	"player.open_map",
	"player.roll",
	"player.select_next",
	"player.select_previous",
	"player.switch_tool",
	"player.use_shield",
	"player.use_tool",
	"player.zoom_map",
	"system.pause",
	"system.quit",
};

/* Semantic action -> the game's REAL InputMap action (single-action sinks).
 * menu.navigate and player.move resolve in the vector path below. zoom_map is
 * natively button-like; a vector registration remains only to interpret an
 * owner file inherited from the old analog default as one edge per gesture. */
struct NxGptkActionSink {
	const char *action;
	const char *input_map_action;
	/* A physical button in Tearscape's own InputMap frequently fires MORE than
	 * one action: `project.binary` binds JoypadButton 0 to `roll` AND to
	 * `ui_accept`. Governing that button natively means suppressing the
	 * physical event and synthesizing the semantic action, so delivering a
	 * single InputMap action silently drops the game's second binding. That is
	 * what made in-world dialogue unclosable: DialogueManager._Input advances
	 * on `ui_accept` (Names.Input.Accept) and never pauses the tree, so the
	 * context stays GAMEPLAY, A is governed to `roll` alone, and no press can
	 * dismiss a sign or an NPC line. The companion restores the button's real
	 * identity; it is not editable and never widens the map. */
	const char *companion_input_map_action;
};
static const NxGptkActionSink nxgptk_button_sinks[] = {
	{ "menu.accept", "ui_accept", nullptr },
	{ "menu.back", "ui_cancel", nullptr },
	{ "player.attack", "attack", nullptr },
	/* JoypadButton 3 (Y, Xbox layout) = heal in the game's own InputMap. The
	 * frozen engine had no heal sink, so no button could drink the flask. */
	{ "player.heal", "heal", nullptr },
	{ "player.open_map", "open_map", nullptr },
	/* JoypadButton 0 = roll + ui_accept in the game's own InputMap. */
	{ "player.roll", "roll", "ui_accept" },
	{ "player.select_next", "select_next", nullptr },
	{ "player.select_previous", "select_prev", nullptr },
	{ "player.switch_tool", "switch_tool", nullptr },
	{ "player.use_shield", "use_shield", nullptr },
	{ "player.use_tool", "use_tool", nullptr },
	{ "player.zoom_map", "zoom_map", nullptr },
	/* UiManager opens/closes every pause overlay from Names.Input.Menu. The
	 * unrelated "pause" project action has no runtime UI consumer. */
	{ "system.pause", "menu", nullptr },
};

/* Vector action -> four directional InputMap actions (up, down, left,
 * right). Analog is preserved for navigation/movement. The edge_action form
 * is a compatibility reader for old owner mappings, not the generated
 * default. */
struct NxGptkVectorSink {
	const char *action;
	const char *up;
	const char *down;
	const char *left;
	const char *right;
	bool edge_action;
};
static const NxGptkVectorSink nxgptk_vector_sinks[] = {
	{ "menu.navigate", "ui_up", "ui_down", "ui_left", "ui_right", false },
	{ "player.move", "move_up", "move_down", "move_left", "move_right", false },
	/* MinimapScreen reads zoom_map as IsActionPressed; one radial stick gesture
	 * therefore becomes one press/release edge, never strength re-delivery. */
	{ "player.zoom_map", "zoom_map", nullptr, nullptr, nullptr, true },
};

struct NxGptkState {
	bool init_done = false;
	bool active = false;
	bool context_proven = false;
	bool quit_requested = false;
	bool native_coop = false;
	bool native_authority = true;
	bool force_start_coop_native = false;
	bool unproven_receipt_emitted = false;
	bool fatal_receipt_emitted = false;
	nxinput_gptk map;
	nxinput_gptk_live live;
	nxinput_gptk_context context = NXINPUT_GPTK_CONTEXT_MENU;
	bool physical_trigger_down[2] = { false, false };
	uint32_t physical_down_controls = 0;
	/* Cached stick vectors, fed on the main-thread tick. */
	float left_x = 0.0f, left_y = 0.0f;
	float right_x = 0.0f, right_y = 0.0f;
	nxinput_godot_neutral_handoff handoff = { 0 };
	nxinput_godot_neutral_handoff suppressed_handoff = { 0 };
	nxinput_godot_action_latch button_latches[
			sizeof(nxgptk_button_sinks) / sizeof(nxgptk_button_sinks[0])] = {};
	/* Directional latches for the vector path: strength currently applied
	 * per vector sink and per direction (0 = released). */
	float vector_strength[sizeof(nxgptk_vector_sinks) / sizeof(nxgptk_vector_sinks[0])][4];
	nxinput_godot_vector_alias vector_alias[
			sizeof(nxgptk_vector_sinks) / sizeof(nxgptk_vector_sinks[0])] = {};
	bool vector_edge_down[
			sizeof(nxgptk_vector_sinks) / sizeof(nxgptk_vector_sinks[0])]
			[NXINPUT_GODOT_VECTOR_ALIAS_SOURCES] = {};
	int vector_feed_source = -1;
	nxinput_godot_lifecycle lifecycle = { 0 };
	/* TEARSCAPE 0.2.17: every C6-admitted gamepad is ONE logical player
	 * (union of controls, max-magnitude axes, same-pad exit chord). */
	TearsPadset pads = {};
	bool pads_ready = false;
	uint32_t button_sink_receipts = 0;
	uint32_t vector_sink_receipts = 0;
	uint32_t suppression_receipts = 0;
	/* TEARSCAPE 0.2.17: the per-run JSON-lines evidence receipt
	 * (NXGPTK_RECEIPT, truncated per launch by port-env.sh) read back by the
	 * framework's automated on-device proof and the release lock builder. The
	 * shapes and the edge/attribution rules live in tearscape_gptk_receipt.*. */
	TearsGptkReceipt receipt = { nullptr, false };
	/* The control whose feed is executing (-1 outside a feed): a press is
	 * attributed to it, a release to the control that pressed. */
	int current_control = -1;
	TearsGptkPressOwner button_owner[
			sizeof(nxgptk_button_sinks) / sizeof(nxgptk_button_sinks[0])] = {};
	TearsGptkPressOwner quit_owner = { -1 };
	/* One open/closed gesture per stick (vector alias source). */
	TearsGptkVectorGesture vector_gesture[NXINPUT_GODOT_VECTOR_ALIAS_SOURCES] = {};
	const NxGptkVectorSink *vector_gesture_sink[NXINPUT_GODOT_VECTOR_ALIAS_SOURCES] = {};
};
static NxGptkState nxgptk;

static void nxgptk_receipt_line(const char *p_line) {
	if (!nxgptk.receipt.tried) {
		(void)tears_gptk_receipt_open(&nxgptk.receipt, getenv("NXGPTK_RECEIPT"));
	}
	tears_gptk_receipt_write(&nxgptk.receipt, p_line);
}

/* The adapter sink id (nxproject controls.actions[].sinks[0]) of a semantic
 * action: the `sink` field of every evidence line, stdout and receipt alike. */
static const char *nxgptk_adapter_sink(const char *p_action) {
	const char *sink = tears_gptk_adapter_sink_id(p_action);
	return sink ? sink : "unknown";
}

static bool nxgptk_vector_release_all();
static int nxgptk_control_for_button(int p_sdl_button);

static const char *nxgptk_context_name() {
	return nxgptk.context == NXINPUT_GPTK_CONTEXT_MENU ? "menu" : "gameplay";
}

static const char *nxgptk_context_source() {
	const char *source = nxinput_gptk_live_context_source(&nxgptk.live);
	return source ? source : "unproven";
}

static int nxgptk_vector_control_for_source(int p_source) {
	if (p_source == 0) {
		return NXINPUT_GPTK_LEFT_STICK;
	}
	if (p_source == 1) {
		return NXINPUT_GPTK_RIGHT_STICK;
	}
	return -1;
}

/* One delivery line per real edge to the game (press AND release; a vector
 * gesture's start and return to neutral). */
static void nxgptk_receipt_delivery(int p_control, const char *p_event,
		const char *p_action, bool p_pressed) {
	char line[512];
	if (tears_gptk_receipt_delivery_line(line, sizeof(line),
			nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
			nxgptk_context_source(),
			p_control >= 0 ? nxinput_gptk_control_name(p_control) : "",
			p_event, p_action, nxgptk_adapter_sink(p_action), p_pressed) == 0) {
		nxgptk_receipt_line(line);
	}
}

/* Closes the stick gesture of one alias source on the evidence (return to
 * neutral, release-all or context change). */
static void nxgptk_vector_gesture_close(int p_source) {
	if (p_source < 0 || p_source >= (int)NXINPUT_GODOT_VECTOR_ALIAS_SOURCES) {
		return;
	}
	if (tears_gptk_vector_gesture_close(&nxgptk.vector_gesture[p_source])) {
		const NxGptkVectorSink *sink = nxgptk.vector_gesture_sink[p_source];
		nxgptk_receipt_delivery(nxgptk_vector_control_for_source(p_source),
				"motion", sink ? sink->action : "", false);
	}
	nxgptk.vector_gesture_sink[p_source] = nullptr;
}

/* TEARSCAPE 0.2.17: the logical player. The pad set keeps each admitted
 * pad's physical truth; the runtime below only ever sees the aggregate. */
static void nxgptk_pads_ensure() {
	if (!nxgptk.pads_ready) {
		nxgptk.pads_ready = true;
		tears_padset_init(&nxgptk.pads,
				nxinput_godot_control_bit(NXINPUT_GPTK_SELECT),
				nxinput_godot_control_bit(NXINPUT_GPTK_START));
	}
}

/* Trigger hysteresis (press at 0.55, release at 0.30) on the aggregate. */
static bool nxgptk_trigger_next(bool p_down, float p_value) {
	if (!p_down && p_value >= 0.55f) {
		return true;
	}
	if (p_down && p_value <= 0.30f) {
		return false;
	}
	return p_down;
}

/* Rebuild the logical player's physical truth from the pad set: union of the
 * button controls, largest-magnitude sticks, hysteresis on the largest
 * trigger. A resting pad never cancels another pad's input. */
static void nxgptk_refresh_from_pads() {
	uint32_t down = tears_padset_union_controls(&nxgptk.pads);
	nxgptk.left_x = tears_padset_axis(&nxgptk.pads, TEARS_PADSET_AXIS_LEFT_X);
	nxgptk.left_y = tears_padset_axis(&nxgptk.pads, TEARS_PADSET_AXIS_LEFT_Y);
	nxgptk.right_x = tears_padset_axis(&nxgptk.pads, TEARS_PADSET_AXIS_RIGHT_X);
	nxgptk.right_y = tears_padset_axis(&nxgptk.pads, TEARS_PADSET_AXIS_RIGHT_Y);
	for (int trigger = 0; trigger < 2; trigger++) {
		const float value = tears_padset_axis(&nxgptk.pads, trigger == 0
						? TEARS_PADSET_AXIS_LEFT_TRIGGER
						: TEARS_PADSET_AXIS_RIGHT_TRIGGER);
		const bool now = nxgptk_trigger_next(nxgptk.physical_trigger_down[trigger], value);
		nxgptk.physical_trigger_down[trigger] = now;
		const uint32_t bit = nxinput_godot_control_bit(
				trigger == 0 ? NXINPUT_GPTK_L2 : NXINPUT_GPTK_R2);
		if (now) {
			down |= bit;
		} else {
			down &= ~bit;
		}
	}
	nxgptk.physical_down_controls = down;
}

/* The sovereign chord itself is judged per pad by the joypad driver. This
 * only names the denied cross-pad shape (SELECT here + START there), once
 * per occurrence, in the exact words the framework proof looks for. */
static void nxgptk_observe_chord() {
	bool log_now = false;
	(void)tears_padset_chord(&nxgptk.pads, &log_now);
	if (log_now) {
		print_line(vformat("nx/input: %s", tears_padset_cross_pad_denial()));
	}
}

static bool nxgptk_deliver(const char *p_input_map_action, bool p_pressed, float p_strength) {
	Input *input = Input::get_singleton();
	if (!input || !p_input_map_action || !p_input_map_action[0]) {
		return false;
	}
	Ref<InputEventAction> ev;
	ev.instantiate();
	if (ev.is_null()) {
		return false;
	}
	ev->set_action(StringName(p_input_map_action));
	ev->set_pressed(p_pressed);
	ev->set_strength(p_pressed ? p_strength : 0.0f);
	input->parse_input_event(ev);
	return true;
}

static void nxgptk_mark_fatal(const char *p_reason, int p_control) {
	nxinput_godot_lifecycle_fail(&nxgptk.lifecycle);
	nxgptk.context_proven = false;
	if (!nxgptk.fatal_receipt_emitted) {
		nxgptk.fatal_receipt_emitted = true;
		const char *control_name = p_control >= 0
				? nxinput_gptk_control_name(p_control)
				: "internal";
		print_line(vformat("nx/gptk: %s decision=FATAL native_replay=false control=%s reason=%s",
				nxinput_gptk_event_evidence_schema(), control_name,
				p_reason ? p_reason : "unknown"));
	}
}

static bool nxgptk_clear_live_context(const char *p_reason) {
	const bool vectors_ok = nxgptk_vector_release_all();
	const bool scalars_ok =
			nxinput_gptk_live_clear_context_checked(&nxgptk.live) == 0;
	nxgptk.context_proven = false;
	if (!vectors_ok || !scalars_ok || nxinput_gptk_live_is_fatal(&nxgptk.live)) {
		nxgptk_mark_fatal(p_reason, -1);
		return false;
	}
	return true;
}

static uint32_t nxgptk_governed_controls(
		bool p_context_proven, nxinput_gptk_context p_context) {
	uint32_t governed = 0;
	if (!p_context_proven) {
		return governed;
	}
	for (int control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
		const nxinput_gptk_decision decision =
				nxinput_gptk_decide(&nxgptk.map, p_context, control, nullptr);
		if (decision == NXINPUT_GPTK_DECIDE_ACTION ||
				decision == NXINPUT_GPTK_DECIDE_SUPPRESS) {
			governed |= nxinput_godot_control_bit(control);
		}
	}
	return governed;
}

static void nxgptk_partition_handoff(
		bool p_context_proven, nxinput_gptk_context p_context) {
	/* Classify against the OLD authority. ACTION/null releases are consumed;
	 * NONE/native releases stay native. Existing barriers win across a second
	 * transition before neutral, so ownership can never flip mid-gesture. */
	nxinput_godot_handoff_partition(&nxgptk.handoff,
			&nxgptk.suppressed_handoff, nxgptk.physical_down_controls,
			nxgptk_governed_controls(p_context_proven, p_context),
			NXINPUT_GPTK_LEFT_STICK, nxgptk.left_x, nxgptk.left_y,
			NXINPUT_GPTK_RIGHT_STICK, nxgptk.right_x, nxgptk.right_y);
}

static int nxgptk_commit_action_edge(const NxGptkActionSink *sink,
		const char *p_action, bool p_pressed, float p_value) {
	if (!sink || !p_action) {
		return -1;
	}
	const size_t index = (size_t)(sink - nxgptk_button_sinks);
	if (index >= sizeof(nxgptk_button_sinks) / sizeof(nxgptk_button_sinks[0]) ||
			strcmp(sink->action, p_action) != 0) {
		return -1;
	}
	nxinput_godot_action_latch &latch = nxgptk.button_latches[index];
	const nxinput_godot_action_effect effect =
			nxinput_godot_action_preview(&latch, p_pressed);
	if (effect == NXINPUT_GODOT_ACTION_INVALID) {
		return -1;
	}
	if (effect == NXINPUT_GODOT_ACTION_DELIVER) {
		const float strength = p_value > 0.0f ? p_value : 1.0f;
		if (!nxgptk_deliver(sink->input_map_action, p_pressed, strength)) {
			return -1;
		}
		/* Both halves of the physical button travel on the same edge and the
		 * same latch: a companion that cannot be delivered is a partial
		 * delivery, never a silent success. */
		if (sink->companion_input_map_action &&
				!nxgptk_deliver(sink->companion_input_map_action, p_pressed,
						strength)) {
			return -1;
		}
	}
	if (nxinput_godot_action_commit(&latch, p_pressed) != 0) {
		return -1;
	}
	if (effect == NXINPUT_GODOT_ACTION_DELIVER) {
		/* The release may be born from a context change, outside any feed: it
		 * belongs to the control that pressed. */
		const int control = tears_gptk_press_owner_edge(
				&nxgptk.button_owner[index], p_pressed, nxgptk.current_control);
		nxgptk_receipt_delivery(control, "press", p_action, p_pressed);
	}
	if (p_pressed) {
		if (index < 32 && (nxgptk.button_sink_receipts & (UINT32_C(1) << index)) == 0) {
			nxgptk.button_sink_receipts |= UINT32_C(1) << index;
			print_line(vformat("nx/gptk: %s decision=ACTION context=%s context_source=%s action=%s sink=%s delivery=1 suppressed=true",
					nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
					nxgptk_context_source(), p_action, nxgptk_adapter_sink(p_action)));
			if (sink->companion_input_map_action) {
				print_line(vformat("nx/gptk: %s decision=COMPANION action=%s sink=%s reason=same-physical-button",
						nxinput_gptk_event_evidence_schema(), p_action,
						sink->companion_input_map_action));
			}
		}
	}
	return 0;
}

static const NxGptkActionSink *nxgptk_action_sink_for(
		const char *p_action) {
	for (const NxGptkActionSink &sink : nxgptk_button_sinks) {
		if (p_action && strcmp(sink.action, p_action) == 0) {
			return &sink;
		}
	}
	return nullptr;
}

/* ACK sink: one semantic action edge -> the real InputMap action. Multiple
 * physical aliases share one refcount, including button+stick edge aliases. */
extern "C" int tearscape_gptk_inputmap_sink(
		void *p_user, const char *p_action, int p_pressed, float p_value) {
	return nxgptk_commit_action_edge(
			(const NxGptkActionSink *)p_user, p_action, p_pressed != 0, p_value);
}

extern "C" int tearscape_gptk_quit_sink(
		void *p_user, const char *p_action, int p_pressed, float p_value) {
	(void)p_user;
	(void)p_value;
	if (!p_action || strcmp(p_action, "system.quit") != 0) {
		return -1;
	}
	if (p_pressed) {
		nxgptk.quit_requested = true;
	}
	nxgptk_receipt_delivery(tears_gptk_press_owner_edge(&nxgptk.quit_owner,
			p_pressed != 0, nxgptk.current_control), "press", p_action, p_pressed != 0);
	return 0;
}

bool nxgptk_godot_consume_quit_request() {
	const bool requested = nxgptk.quit_requested;
	nxgptk.quit_requested = false;
	return requested;
}

bool nxgptk_godot_consume_fatal_request() {
	return nxinput_godot_lifecycle_consume_close(&nxgptk.lifecycle) != 0;
}

static bool nxgptk_validate_input_map_sinks() {
	InputMap *input_map = InputMap::get_singleton();
	if (!input_map) {
		print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=inputmap-unavailable",
				nxinput_gptk_event_evidence_schema()));
		return false;
	}
	for (const NxGptkActionSink &sink : nxgptk_button_sinks) {
		if (!input_map->has_action(StringName(sink.input_map_action))) {
			print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=missing-sink sink=%s",
					nxinput_gptk_event_evidence_schema(), sink.input_map_action));
			return false;
		}
		if (sink.companion_input_map_action &&
				!input_map->has_action(
						StringName(sink.companion_input_map_action))) {
			print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=missing-companion-sink sink=%s",
					nxinput_gptk_event_evidence_schema(),
					sink.companion_input_map_action));
			return false;
		}
	}
	for (const NxGptkVectorSink &sink : nxgptk_vector_sinks) {
		if (sink.edge_action && !nxgptk_action_sink_for(sink.action)) {
			print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=missing-edge-alias-sink action=%s",
					nxinput_gptk_event_evidence_schema(), sink.action));
			return false;
		}
		const char *directions[] = { sink.up, sink.down, sink.left, sink.right };
		for (const char *direction : directions) {
			if (direction && !input_map->has_action(StringName(direction))) {
				print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=missing-vector-sink sink=%s",
						nxinput_gptk_event_evidence_schema(), direction));
				return false;
			}
		}
	}
	print_line("nx/gptk: InputMap sink contract proven");
	return true;
}

/* TEARSCAPE 0.2.10: the narrow pre-init boundary (mission 5.3). The GPTK
 * owner/default map -- and with it the V3 FACE_LAYOUT -- is read EXACTLY
 * once, through nxinput_gptk4_load_project_at(), BEFORE the port declares its
 * bundle, stages the live mapping and calls SDL_Init. The same in-memory
 * map/receipt then feeds the live runtime; nothing re-reads the file later,
 * so a TOCTOU swap can never decide something different. */
/* V5 (nxinput 0.11.1): the sixty lines that opened the directories, called
 * the loader, formatted the receipt and projected the schema-4 selection onto
 * the V3 result used to live HERE and, identically, in three other ports.
 * They are now nxinput_gptk4_preinit_load(); what stays in this file is the
 * only part that is really the port's: WHICH actions this game declares. */
static nxinput_gptk4_preinit_result nxgptk4_preinit;
static bool nxgptk_preinit_done = false;

/* V5 (nxinput 0.11.0): the owner file is NEXTOS_CONTROLLERS/4, loaded
 * (default then owner, bytes never rewritten) and projected onto the live
 * V3 map by nxinput_gptk4_bridge. Actions carry their value kinds; the only
 * sparse override context this port declares is `menu`. */
static const nxinput_gptk4_action_decl nxgptk4_actions[] = {
	{ "menu.accept", NXINPUT_GPTK4_V_DIGITAL },
	{ "menu.back", NXINPUT_GPTK4_V_DIGITAL },
	{ "menu.navigate", NXINPUT_GPTK4_V_VECTOR2 },
	{ "player.attack", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.heal", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.move", NXINPUT_GPTK4_V_VECTOR2 },
	{ "player.open_map", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.roll", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.select_next", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.select_previous", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.switch_tool", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.use_shield", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.use_tool", NXINPUT_GPTK4_V_DIGITAL },
	{ "player.zoom_map", NXINPUT_GPTK4_V_DIGITAL },
	{ "system.pause", NXINPUT_GPTK4_V_DIGITAL },
	{ "system.quit", NXINPUT_GPTK4_V_DIGITAL },
};
static const char *const nxgptk4_contexts[] = { "menu" };

int nxgptk_godot_preinit() {
	if (nxgptk_preinit_done) {
		return (int)nxgptk4_preinit.v3.face_layout;
	}
	nxgptk_preinit_done = true;
	nxinput_gptk4_contract contract;
	memset(&contract, 0, sizeof(contract));
	contract.actions = nxgptk4_actions;
	contract.count = sizeof(nxgptk4_actions) / sizeof(nxgptk4_actions[0]);
	contract.contexts = nxgptk4_contexts;
	contract.context_count = sizeof(nxgptk4_contexts) / sizeof(nxgptk4_contexts[0]);
	nxinput_gptk4_preinit_load(getenv("NXCOMPAT_GAME_DIR"), &contract, &nxgptk4_preinit);
	static bool stdout_line_buffered = false;
	if (!stdout_line_buffered) {
		stdout_line_buffered = true;
		setvbuf(stdout, nullptr, _IOLBF, 0);
	}
	if (nxgptk4_preinit.receipt_json[0]) {
		print_line(vformat("nx/gptk: preinit load %s", String::utf8(nxgptk4_preinit.receipt_json)));
	}
	print_line(vformat("nx/gptk: preinit face_layout=%s loaded=%d",
			nxinput_gptk_face_layout_name((int)nxgptk4_preinit.v3.face_layout),
			nxgptk4_preinit.v3.loaded));
	if (nxgptk4_preinit.log_line[0]) {
		print_line(vformat("nx/gptk: %s", String::utf8(nxgptk4_preinit.log_line)));
	}
	return (int)nxgptk4_preinit.v3.face_layout;
}

void nxgptk_godot_init_once() {
	if (nxgptk.init_done) {
		return;
	}
	nxgptk.init_done = true;
	nxgptk.active = false;

	/* Consume the single pre-init read. Calling preinit here is only the
	 * fail-safe for a build that skipped the boundary; the Tearscape driver
	 * runs it before SDL_Init and the source-order gate proves that. */
	(void)nxgptk_godot_preinit();
	if (!nxgptk4_preinit.v3.loaded) {
		print_line(vformat("nx/gptk: no valid mapping (NXI%04d); controls stay native",
				nxgptk4_preinit.v3.rc));
		return;
	}
	nxgptk.map = nxgptk4_preinit.v3.map;

	if (!nxgptk_validate_input_map_sinks()) {
		return;
	}
	nxinput_gptk_live_init(&nxgptk.live, &nxgptk.map);
	for (const NxGptkActionSink &sink : nxgptk_button_sinks) {
		if (nxinput_gptk_live_register(&nxgptk.live, sink.action,
					tearscape_gptk_inputmap_sink, (void *)&sink) != 0) {
			print_line("nx/gptk: scalar sink registration failed; controls stay native");
			return;
		}
	}
	if (nxinput_gptk_live_register(&nxgptk.live, "system.quit",
			tearscape_gptk_quit_sink, nullptr) != 0) {
		print_line("nx/gptk: quit sink registration failed; controls stay native");
		return;
	}
	for (const NxGptkVectorSink &sink : nxgptk_vector_sinks) {
		if (nxinput_gptk_live_register_vector(&nxgptk.live, sink.action,
					tearscape_gptk_inputmap_vector_sink, (void *)&sink) != 0) {
			print_line("nx/gptk: vector sink registration failed; controls stay native");
			return;
		}
	}
	char seal_error[160] = {};
	if (nxinput_gptk_live_seal(&nxgptk.live, seal_error, sizeof(seal_error)) != 0) {
		print_line(vformat("nx/gptk: %s; controls stay native", seal_error));
		return;
	}
	memset(nxgptk.vector_strength, 0, sizeof(nxgptk.vector_strength));
	memset(nxgptk.button_latches, 0, sizeof(nxgptk.button_latches));
	for (TearsGptkPressOwner &owner : nxgptk.button_owner) {
		tears_gptk_press_owner_init(&owner);
	}
	tears_gptk_press_owner_init(&nxgptk.quit_owner);
	for (unsigned int source = 0; source < NXINPUT_GODOT_VECTOR_ALIAS_SOURCES; source++) {
		tears_gptk_vector_gesture_init(&nxgptk.vector_gesture[source]);
		nxgptk.vector_gesture_sink[source] = nullptr;
	}
	nxgptk.current_control = -1;
	nxgptk.context_proven = false;
	nxgptk.native_authority = true;
	nxgptk.active = true;
	const char *load_source = nxinput_gptk_load_source_name(
			(nxinput_gptk_load_source)nxgptk4_preinit.v3.receipt.source);
	print_line(vformat("nx/gptk: runtime live marker=%s godot_runtime=%s evidence=%s source=%s schema=%d",
			NXINPUT_GPTK4_RUNTIME_MARKER, nxinput_godot_runtime_marker(),
			nxinput_gptk_event_evidence_schema(), load_source,
			(int)nxgptk4_preinit.v3.receipt.selected_gptk_schema));
	/* The runtime seal line of the receipt: every sink registered above
	 * (button sinks + the quit sink + the vector sinks). */
	const size_t sinks = sizeof(nxgptk_button_sinks) / sizeof(nxgptk_button_sinks[0]) +
			1u + sizeof(nxgptk_vector_sinks) / sizeof(nxgptk_vector_sinks[0]);
	char runtime_line[640];
	if (tears_gptk_receipt_runtime_line(runtime_line, sizeof(runtime_line),
			nxinput_gptk_event_evidence_schema(), NXINPUT_GPTK4_RUNTIME_MARKER,
			nxgptk4_preinit.v3.receipt.selected_sha256, load_source,
			(unsigned int)nxgptk4_preinit.v3.map.schema_version,
			nxinput_gptk_face_layout_name((int)nxgptk4_preinit.v3.face_layout),
			sinks) == 0) {
		nxgptk_receipt_line(runtime_line);
	}
}

bool nxgptk_godot_active() {
	return nxgptk.active;
}

static float nxgptk_snapshot_stick_axis(SDL_Gamepad *p_gamepad, SDL_GamepadAxis p_axis) {
	return nxgptk_godot_stick_axis_value(SDL_GetGamepadAxis(p_gamepad, p_axis));
}

static float nxgptk_snapshot_trigger_axis(SDL_Gamepad *p_gamepad, SDL_GamepadAxis p_axis) {
	float value = SDL_GetGamepadAxis(p_gamepad, p_axis) /
			(float)SDL_JOYSTICK_AXIS_MAX;
	if (value < 0.0f) {
		value = 0.0f;
	} else if (value > 1.0f) {
		value = 1.0f;
	}
	return value;
}

bool nxgptk_godot_gamepad_connected(int p_joy_id, SDL_Gamepad *p_gamepad) {
	if (!p_gamepad || p_joy_id < 0) {
		return false;
	}
	nxgptk_pads_ensure();

	/* Admission is an authority boundary. Snapshot the already-open SDL pad
	 * BEFORE it joins the logical player so presses/motion that began on the
	 * native path can only finish on that same path. A later neutral/press is
	 * the first gesture eligible for GPTK. The same snapshot refreshes a pad
	 * that is already a member (hotplug of a sibling pad). */
	uint32_t physical_down = 0;
	for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++) {
		const int control = nxgptk_control_for_button(button);
		if (control >= 0 && SDL_GetGamepadButton(
					p_gamepad, (SDL_GamepadButton)button)) {
			physical_down |= nxinput_godot_control_bit(control);
		}
	}
	const float left_x = nxgptk_snapshot_stick_axis(
			p_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
	const float left_y = nxgptk_snapshot_stick_axis(
			p_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
	const float right_x = nxgptk_snapshot_stick_axis(
			p_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
	const float right_y = nxgptk_snapshot_stick_axis(
			p_gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
	const float left_trigger = nxgptk_snapshot_trigger_axis(
			p_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
	const float right_trigger = nxgptk_snapshot_trigger_axis(
			p_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
	const float axes[TEARS_PADSET_AXES] = {
		left_x, left_y, right_x, right_y, left_trigger, right_trigger
	};

	/* Capability-based admission: any gamepad the C6 seam let SDL open joins
	 * the logical player, up to the set's cap. Nothing here reads a device
	 * name, VID/PID or firmware identity. */
	const int instance = (int)SDL_GetGamepadID(p_gamepad);
	const bool refresh = tears_padset_slot(&nxgptk.pads, p_joy_id) >= 0;
	const int slot = tears_padset_admit(&nxgptk.pads, p_joy_id, instance);
	if (slot < 0) {
		print_line(vformat("nx/input: pad refused joy=%d instance=%d: logical player set is full (cap=%d); pad stays native",
				p_joy_id, instance, (int)TEARS_PADSET_MAX));
		return false;
	}

	/* What the logical player already held, with its authority already
	 * decided, before this pad's truth is merged. */
	nxinput_godot_neutral_handoff previous = { 0 };
	nxinput_godot_handoff_snapshot(&previous, nxgptk.physical_down_controls,
			NXINPUT_GPTK_LEFT_STICK, nxgptk.left_x, nxgptk.left_y,
			NXINPUT_GPTK_RIGHT_STICK, nxgptk.right_x, nxgptk.right_y);
	const bool first = !refresh && nxgptk.pads.count == 1;
	tears_padset_set_snapshot(&nxgptk.pads, p_joy_id, physical_down, axes);
	nxgptk_refresh_from_pads();
	nxinput_godot_neutral_handoff merged = { 0 };
	nxinput_godot_handoff_snapshot(&merged, nxgptk.physical_down_controls,
			NXINPUT_GPTK_LEFT_STICK, nxgptk.left_x, nxgptk.left_y,
			NXINPUT_GPTK_RIGHT_STICK, nxgptk.right_x, nxgptk.right_y);
	if (first) {
		/* The single-pad adoption: nothing else is held, both masks restart. */
		nxgptk.handoff.controls = 0;
		nxgptk.suppressed_handoff.controls = 0;
	}
	/* Controls active only because of this pad were never judged by any
	 * authority: they finish natively. Controls another admitted pad already
	 * held keep the ownership they already have. */
	nxgptk.handoff.controls |= merged.controls & ~previous.controls &
			~nxgptk.suppressed_handoff.controls;
	nxgptk_observe_chord();
	if (refresh) {
		return true;
	}

	/* Admission receipt in the shape the framework's automated on-device
	 * controls proof parses: the REAL SDL mapping of the opened gamepad (never
	 * synthesized) and one slot line per admitted pad. SDL3 has no device
	 * index, so the slot line repeats the instance id in that field. */
	const char *gamepad_name = SDL_GetGamepadName(p_gamepad);
	const String name = gamepad_name ? String::utf8(gamepad_name) : String("unknown");
	const int vendor = (int)SDL_GetGamepadVendor(p_gamepad);
	const int product = (int)SDL_GetGamepadProduct(p_gamepad);
	char *mapping = SDL_GetGamepadMapping(p_gamepad);
	if (mapping) {
		print_line(vformat("nx/input: controller: %s (%04x:%04x) mapping=%s",
				name, vendor, product, String::utf8(mapping)));
		SDL_free(mapping);
	} else {
		print_line(vformat("nx/input: controller: %s (%04x:%04x) mapping unavailable: %s",
				name, vendor, product, String::utf8(SDL_GetError())));
	}
	print_line(vformat("nx/input: pad slot=%d instance=%d sdl_index=%d",
			slot, instance, instance));
	print_line(vformat("nx/gptk: pad joy=%d instance=%d admitted to the logical player from complete SDL snapshot; pads=%d",
			p_joy_id, instance, (int)nxgptk.pads.count));
	return true;
}

bool nxgptk_godot_gamepad_disconnected(int p_joy_id) {
	nxgptk_pads_ensure();
	const int slot = tears_padset_slot(&nxgptk.pads, p_joy_id);
	if (slot < 0) {
		return false;
	}
	const int instance = nxgptk.pads.pads[slot].instance_id;
	tears_padset_remove(&nxgptk.pads, p_joy_id);
	/* Every latched action is released now; the next tick re-proves the
	 * context. The remaining pads keep the logical player alive. */
	(void)nxgptk_clear_live_context("pad-disconnect-release-failed");
	nxgptk.native_authority = true;
	nxgptk.handoff.controls = 0;
	nxgptk.suppressed_handoff.controls = 0;
	nxgptk.physical_down_controls = 0;
	nxgptk.physical_trigger_down[0] = false;
	nxgptk.physical_trigger_down[1] = false;
	tears_gptk_clear_axis_cache(
			&nxgptk.left_x, &nxgptk.left_y, &nxgptk.right_x, &nxgptk.right_y);
	if (nxgptk.pads.count > 0) {
		/* Whatever the remaining pads still hold was just released from every
		 * GPTK latch: it finishes natively, exactly like an adoption snapshot.
		 * The joypad driver then refreshes each remaining pad from SDL truth. */
		nxgptk_refresh_from_pads();
		nxinput_godot_handoff_snapshot(&nxgptk.handoff, nxgptk.physical_down_controls,
				NXINPUT_GPTK_LEFT_STICK, nxgptk.left_x, nxgptk.left_y,
				NXINPUT_GPTK_RIGHT_STICK, nxgptk.right_x, nxgptk.right_y);
	}
	print_line(vformat("nx/input: controller-removed instance=%d joy=%d slot=%d remaining=%d",
			instance, p_joy_id, slot, (int)nxgptk.pads.count));
	return true;
}

static bool nxgptk_result_consumes(
		nxinput_gptk_live_result p_result, int p_control, bool p_pressed) {
	if (p_result == NXINPUT_GPTK_LIVE_PASSTHROUGH) {
		return false;
	}
	if (p_result == NXINPUT_GPTK_LIVE_SUPPRESSED && p_pressed && p_control >= 0) {
		/* A proven `null`: the physical press existed and NOTHING was delivered
		 * -- evidence as important as a delivery, one receipt line per press. */
		char line[320];
		if (tears_gptk_receipt_suppressed_line(line, sizeof(line),
				nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
				nxgptk_context_source(), nxinput_gptk_control_name(p_control)) == 0) {
			nxgptk_receipt_line(line);
		}
		if (p_control < 32 &&
				(nxgptk.suppression_receipts & (UINT32_C(1) << p_control)) == 0) {
			nxgptk.suppression_receipts |= UINT32_C(1) << p_control;
			print_line(vformat("nx/gptk: %s decision=SUPPRESS context=%s context_source=%s control=%s delivery=0 suppressed=true",
					nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
					nxgptk_context_source(), nxinput_gptk_control_name(p_control)));
		}
	}
	if (p_result == NXINPUT_GPTK_LIVE_FATAL) {
		nxgptk_mark_fatal("sink-ack-failed", p_control);
	}
	/* DELIVERED/SUPPRESSED own the event. FATAL happened after a sink was
	 * invoked and must never be replayed through the native path. */
	return true;
}

/* SDL gamepad button -> symbolic GPTK control. The C6 seam already made the
 * SDL logical layout sovereign (PortMaster mapping), so SOUTH/EAST/... are
 * the semantic A/B/... of this device. */
static int nxgptk_control_for_button(int p_sdl_button) {
	switch (p_sdl_button) {
		case SDL_GAMEPAD_BUTTON_SOUTH:
			return NXINPUT_GPTK_A;
		case SDL_GAMEPAD_BUTTON_EAST:
			return NXINPUT_GPTK_B;
		case SDL_GAMEPAD_BUTTON_WEST:
			return NXINPUT_GPTK_X;
		case SDL_GAMEPAD_BUTTON_NORTH:
			return NXINPUT_GPTK_Y;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
			return NXINPUT_GPTK_L1;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
			return NXINPUT_GPTK_R1;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK:
			return NXINPUT_GPTK_L3;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
			return NXINPUT_GPTK_R3;
		case SDL_GAMEPAD_BUTTON_START:
			return NXINPUT_GPTK_START;
		case SDL_GAMEPAD_BUTTON_BACK:
			return NXINPUT_GPTK_SELECT;
		case SDL_GAMEPAD_BUTTON_DPAD_UP:
			return NXINPUT_GPTK_UP;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
			return NXINPUT_GPTK_DOWN;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
			return NXINPUT_GPTK_LEFT;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
			return NXINPUT_GPTK_RIGHT;
		default:
			return -1;
	}
}

/* Ownership of an event that produced NO edge on the logical player (another
 * admitted pad already holds, or still holds, the same control): the event
 * is owned exactly as the live control is -- consumed while governed, native
 * otherwise -- without touching any latch or handoff mask. */
static bool nxgptk_control_consumed_without_edge(int p_control) {
	if (!nxgptk.active) {
		return false;
	}
	if (!nxinput_godot_lifecycle_health_allowed(&nxgptk.lifecycle)) {
		return true;
	}
	const uint32_t bit = nxinput_godot_control_bit(p_control);
	if ((nxgptk.suppressed_handoff.controls & bit) != 0u) {
		return true;
	}
	if ((nxgptk.handoff.controls & bit) != 0u) {
		return false;
	}
	if (nxgptk.native_coop) {
		return false;
	}
	if (p_control == NXINPUT_GPTK_X && nxgptk.force_start_coop_native) {
		return false;
	}
	return nxinput_gptk_live_should_consume(&nxgptk.live, p_control) != 0;
}

bool nxgptk_godot_gamepad_button(int p_joy_id, int p_sdl_button, bool p_pressed) {
	nxgptk_pads_ensure();
	if (tears_padset_slot(&nxgptk.pads, p_joy_id) < 0) {
		return false;
	}
	const int control = nxgptk_control_for_button(p_sdl_button);
	if (control < 0) {
		return false;
	}
	const uint32_t bit = nxinput_godot_control_bit(control);
	/* Physical truth per pad first, then the logical player's union. */
	const uint32_t previous_down = nxgptk.physical_down_controls;
	tears_padset_set_control(&nxgptk.pads, p_joy_id, bit, p_pressed);
	nxgptk_refresh_from_pads();
	nxgptk_observe_chord();
	if ((nxgptk.physical_down_controls & bit) == (previous_down & bit)) {
		return nxgptk_control_consumed_without_edge(control);
	}
	/* SDL may deliver the first native press before the main-thread tick has
	 * loaded GPTK. Keep physical truth anyway so activation can hand that
	 * already-native control back only after its matching release. */
	if (!nxgptk.active) {
		return false;
	}
	if (!nxinput_godot_lifecycle_health_allowed(&nxgptk.lifecycle)) {
		return true;
	}
	if (nxinput_godot_handoff_button(
			&nxgptk.suppressed_handoff, control, p_pressed)) {
		return true;
	}
	if (nxinput_godot_handoff_button(&nxgptk.handoff, control, p_pressed)) {
		return false;
	}
	if (nxgptk.native_coop) {
		return false;
	}
	if (control == NXINPUT_GPTK_X && nxgptk.force_start_coop_native) {
		/* The press may change scenes before release. Latch native ownership so
		 * the release cannot be consumed as a new GPTK action in the next scene. */
		if (p_pressed) {
			nxgptk.handoff.controls |= bit;
		}
		return false;
	}
	nxgptk.current_control = control;
	const nxinput_gptk_live_result result = nxinput_gptk_live_feed(
			&nxgptk.live, control, p_pressed ? 1 : 0, p_pressed ? 1.0f : 0.0f);
	nxgptk.current_control = -1;
	return nxgptk_result_consumes(result, control, p_pressed);
}

bool nxgptk_godot_gamepad_axis(int p_joy_id, int p_sdl_axis, float p_value) {
	nxgptk_pads_ensure();
	if (tears_padset_slot(&nxgptk.pads, p_joy_id) < 0) {
		return false;
	}
	int control = -1;
	int pad_axis = -1;
	switch (p_sdl_axis) {
		case SDL_GAMEPAD_AXIS_LEFTX:
			pad_axis = TEARS_PADSET_AXIS_LEFT_X;
			control = NXINPUT_GPTK_LEFT_STICK;
			break;
		case SDL_GAMEPAD_AXIS_LEFTY:
			pad_axis = TEARS_PADSET_AXIS_LEFT_Y;
			control = NXINPUT_GPTK_LEFT_STICK;
			break;
		case SDL_GAMEPAD_AXIS_RIGHTX:
			pad_axis = TEARS_PADSET_AXIS_RIGHT_X;
			control = NXINPUT_GPTK_RIGHT_STICK;
			break;
		case SDL_GAMEPAD_AXIS_RIGHTY:
			pad_axis = TEARS_PADSET_AXIS_RIGHT_Y;
			control = NXINPUT_GPTK_RIGHT_STICK;
			break;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
			pad_axis = TEARS_PADSET_AXIS_LEFT_TRIGGER;
			control = NXINPUT_GPTK_L2;
			break;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
			pad_axis = TEARS_PADSET_AXIS_RIGHT_TRIGGER;
			control = NXINPUT_GPTK_R2;
			break;
		default:
			return false;
	}
	/* Physical truth per pad first; the logical player takes the largest
	 * deflection across the admitted pads (sticks and triggers alike). */
	const bool previous_trigger[2] = {
		nxgptk.physical_trigger_down[0], nxgptk.physical_trigger_down[1]
	};
	tears_padset_set_axis(&nxgptk.pads, p_joy_id, pad_axis, p_value);
	nxgptk_refresh_from_pads();
	const float logical_value = tears_padset_axis(&nxgptk.pads, pad_axis);
	bool trigger_changed = false;
	bool trigger_now = false;
	if (control == NXINPUT_GPTK_L2 || control == NXINPUT_GPTK_R2) {
		const int trigger = control == NXINPUT_GPTK_L2 ? 0 : 1;
		trigger_now = nxgptk.physical_trigger_down[trigger];
		trigger_changed = trigger_now != previous_trigger[trigger];
	}
	/* As with buttons, cache native pre-init motion before GPTK is active. */
	if (!nxgptk.active) {
		return false;
	}
	if (!nxinput_godot_lifecycle_health_allowed(&nxgptk.lifecycle)) {
		return true;
	}
	if (control == NXINPUT_GPTK_LEFT_STICK || control == NXINPUT_GPTK_RIGHT_STICK) {
		const float x = control == NXINPUT_GPTK_LEFT_STICK ? nxgptk.left_x : nxgptk.right_x;
		const float y = control == NXINPUT_GPTK_LEFT_STICK ? nxgptk.left_y : nxgptk.right_y;
		if (nxinput_godot_handoff_vector(
				&nxgptk.suppressed_handoff, control, x, y)) {
			return true;
		}
		if (nxinput_godot_handoff_vector(&nxgptk.handoff, control, x, y)) {
			return false;
		}
		if (nxgptk.native_coop) {
			return false;
		}
		/* Stick axis cached above: governed sticks are suppressed from the
		 * native path and delivered on the tick; native sticks pass through. */
		return nxinput_gptk_live_should_consume(&nxgptk.live, control) != 0;
	}
	if (nxinput_godot_handoff_button(
			&nxgptk.suppressed_handoff, control, trigger_now)) {
		return true;
	}
	if (nxinput_godot_handoff_button(&nxgptk.handoff, control, trigger_now)) {
		return false;
	}
	if (nxgptk.native_coop) {
		return false;
	}
	if (trigger_changed) {
		nxgptk.current_control = control;
		const nxinput_gptk_live_result result = nxinput_gptk_live_feed(
				&nxgptk.live, control, trigger_now ? 1 : 0,
				trigger_now ? logical_value : 0.0f);
		nxgptk.current_control = -1;
		return nxgptk_result_consumes(result, control, trigger_now);
	}
	return nxinput_gptk_live_should_consume(&nxgptk.live, control) != 0;
}

static int nxgptk_vector_source_for_control(int p_control) {
	if (p_control == NXINPUT_GPTK_LEFT_STICK) {
		return 0;
	}
	if (p_control == NXINPUT_GPTK_RIGHT_STICK) {
		return 1;
	}
	return -1;
}

static bool nxgptk_vector_release_all() {
	/* Open stick gestures close on the evidence (forced return to neutral),
	 * one pressed=0 line each, before any latch is released. */
	for (unsigned int source = 0; source < NXINPUT_GODOT_VECTOR_ALIAS_SOURCES; source++) {
		nxgptk_vector_gesture_close((int)source);
	}
	for (size_t i = 0; i < sizeof(nxgptk_vector_sinks) / sizeof(nxgptk_vector_sinks[0]); i++) {
		const NxGptkVectorSink &sink = nxgptk_vector_sinks[i];
		if (sink.edge_action) {
			const NxGptkActionSink *action_sink =
					nxgptk_action_sink_for(sink.action);
			for (unsigned int source = 0;
					source < NXINPUT_GODOT_VECTOR_ALIAS_SOURCES; source++) {
				if (nxgptk.vector_edge_down[i][source]) {
					if (!action_sink ||
							nxgptk_commit_action_edge(
								action_sink, sink.action, false, 0.0f) != 0) {
						return false;
					}
					nxgptk.vector_edge_down[i][source] = false;
				}
			}
			continue;
		}
		const char *dirs[4] = { sink.up, sink.down, sink.left, sink.right };
		for (int d = 0; d < 4; d++) {
			if (dirs[d] && nxgptk.vector_strength[i][d] > 0.0f) {
				if (!nxgptk_deliver(dirs[d], false, 0.0f)) {
					return false;
				}
			}
			nxgptk.vector_strength[i][d] = 0.0f;
		}
		nxinput_godot_vector_alias_clear(&nxgptk.vector_alias[i]);
	}
	return true;
}

/* Deliver one stick vector with per-direction analog strength. The legacy
 * button-like form uses radial hysteresis and emits only transition edges. */
static bool nxgptk_vector_feed(int p_sink_index, const NxGptkVectorSink *p_sink,
		float p_x, float p_y) {
	const int source = nxgptk.vector_feed_source;
	if (source < 0 || source >= (int)NXINPUT_GODOT_VECTOR_ALIAS_SOURCES) {
		return false;
	}
	if (p_sink->edge_action) {
		const bool previous = nxgptk.vector_edge_down[p_sink_index][source];
		const bool next = tears_gptk_edge_next(previous, p_x, p_y);
		if (next != previous) {
			const NxGptkActionSink *action_sink =
					nxgptk_action_sink_for(p_sink->action);
			if (!action_sink || nxgptk_commit_action_edge(action_sink,
					p_sink->action, next, next ? 1.0f : 0.0f) != 0) {
				return false;
			}
			nxgptk.vector_edge_down[p_sink_index][source] = next;
			if (next && (nxgptk.vector_sink_receipts &
					(UINT32_C(1) << p_sink_index)) == 0) {
				nxgptk.vector_sink_receipts |= UINT32_C(1) << p_sink_index;
				print_line(vformat("nx/gptk: %s decision=ACTION context=%s context_source=%s action=%s sink=%s delivery=1 suppressed=true",
						nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
						nxgptk_context_source(), p_sink->action,
						nxgptk_adapter_sink(p_sink->action)));
			}
		}
		return true;
	}
	float strengths[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; /* aggregate: up/down/left/right */
	if (nxinput_godot_vector_alias_update(&nxgptk.vector_alias[p_sink_index],
			(unsigned int)source, p_x, p_y, strengths) != 0) {
		return false;
	}
	const char *dirs[4] = { p_sink->up, p_sink->down, p_sink->left, p_sink->right };
	for (int d = 0; d < 4; d++) {
		if (!dirs[d]) {
			continue;
		}
		const float previous = nxgptk.vector_strength[p_sink_index][d];
		if (strengths[d] > 0.0f) {
			if (fabsf(strengths[d] - previous) > 0.01f || previous <= 0.0f) {
				if (!nxgptk_deliver(dirs[d], true, strengths[d])) {
					return false;
				}
				nxgptk.vector_strength[p_sink_index][d] = strengths[d];
			}
		} else if (previous > 0.0f) {
			if (!nxgptk_deliver(dirs[d], false, 0.0f)) {
				return false;
			}
			nxgptk.vector_strength[p_sink_index][d] = 0.0f;
		}
	}
	const bool deflected = strengths[0] > 0.0f || strengths[1] > 0.0f ||
			strengths[2] > 0.0f || strengths[3] > 0.0f;
	/* The vector arrives every frame; the evidence is by EDGE: one line when
	 * this stick leaves neutral, one when it returns (or is released). */
	const int edge = tears_gptk_vector_gesture_feed(
			&nxgptk.vector_gesture[source], deflected);
	if (edge == TEARS_GPTK_VECTOR_EDGE_START) {
		nxgptk.vector_gesture_sink[source] = p_sink;
		nxgptk_receipt_delivery(nxgptk_vector_control_for_source(source),
				"motion", p_sink->action, true);
	} else if (edge == TEARS_GPTK_VECTOR_EDGE_STOP) {
		nxgptk_receipt_delivery(nxgptk_vector_control_for_source(source),
				"motion", p_sink->action, false);
		nxgptk.vector_gesture_sink[source] = nullptr;
	}
	if ((nxgptk.vector_sink_receipts & (UINT32_C(1) << p_sink_index)) == 0 && deflected) {
		nxgptk.vector_sink_receipts |= UINT32_C(1) << p_sink_index;
		print_line(vformat("nx/gptk: %s decision=ACTION context=%s context_source=%s action=%s sink=%s delivery=1 suppressed=true",
				nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
				nxgptk_context_source(), p_sink->action,
				nxgptk_adapter_sink(p_sink->action)));
	}
	return true;
}

extern "C" int tearscape_gptk_inputmap_vector_sink(
		void *p_user, const char *p_action, float p_x, float p_y) {
	const NxGptkVectorSink *sink = (const NxGptkVectorSink *)p_user;
	const size_t index = (size_t)(sink - nxgptk_vector_sinks);
	if (index >= sizeof(nxgptk_vector_sinks) / sizeof(nxgptk_vector_sinks[0]) ||
			strcmp(sink->action, p_action) != 0) {
		return -1;
	}
	return nxgptk_vector_feed((int)index, sink, p_x, p_y) ? 0 : -1;
}

static nxinput_gptk_live_result nxgptk_feed_vector_control(
		int p_control, float p_x, float p_y) {
	const int source = nxgptk_vector_source_for_control(p_control);
	if (source < 0) {
		return NXINPUT_GPTK_LIVE_FATAL;
	}
	nxgptk.vector_feed_source = source;
	nxgptk.current_control = p_control;
	const nxinput_gptk_live_result result = nxinput_gptk_live_feed_vector(
			&nxgptk.live, p_control, p_x, p_y);
	nxgptk.current_control = -1;
	nxgptk.vector_feed_source = -1;
	return result;
}

extern "C" int tearscape_gptk_resolve_context(
		const char *p_scene_path, int p_tree_paused) {
	return (int)tears_gptk_context_for_scene(p_scene_path, p_tree_paused != 0);
}

void nxgptk_godot_tick(double p_delta) {
	(void)p_delta;
	nxgptk_godot_init_once();
	if (!nxgptk.active) {
		return;
	}
	if (!nxinput_godot_lifecycle_health_allowed(&nxgptk.lifecycle)) {
		return;
	}

	/* Tearscape dynamically creates attack2 and the other device-specific P2
	 * actions only in co-op. InputEventAction cannot preserve the physical
	 * JoypadButton/Motion class used by that split, so co-op stays fully native. */
	InputMap *input_map = InputMap::get_singleton();
	const bool coop_now = input_map && input_map->has_action(StringName("attack2"));
	if (coop_now != nxgptk.native_coop) {
		const bool previous_context_proven =
				nxgptk.context_proven && !nxgptk.native_authority;
		const nxinput_gptk_context previous_context = nxgptk.context;
		if (!nxgptk_clear_live_context("co-op-transition-release-failed")) {
			return;
		}
		nxgptk_partition_handoff(previous_context_proven, previous_context);
		/* Do not clear physical stick caches here. While co-op was native they
		 * kept tracking the real pad; the following native->GPTK snapshot needs
		 * those values to wait for neutral instead of swallowing half a gesture. */
		nxgptk.native_authority = true;
		nxgptk.native_coop = coop_now;
		print_line(coop_now
				? "nx/gptk: co-op InputMap detected; all pads stay on the native device-aware path"
				: "nx/gptk: co-op InputMap removed; primary-pad mapping may resume");
	}
	if (nxgptk.native_coop) {
		return;
	}

	/* Context is a proved property of Tearscape's current scene, never a guess.
	 * UiManager owns SceneTree.paused around every in-game menu overlay; the
	 * custom selector focus is deliberately not used as evidence. Unknown scenes
	 * fall back to the native Godot path without suppression. */
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	Node *current_scene = tree ? tree->get_current_scene() : nullptr;
	const bool tree_paused = tree && tree->is_paused();
	const String scene_path = current_scene ? current_scene->get_scene_file_path() : String();
	const CharString scene_utf8 = scene_path.utf8();
	nxgptk.force_start_coop_native =
			tears_gptk_scene_requires_native_start_coop(scene_utf8.get_data());
	const TearsGptkContextPolicy policy = (TearsGptkContextPolicy)tearscape_gptk_resolve_context(
			scene_utf8.get_data(), tree_paused ? 1 : 0);
	if (policy == TEARS_GPTK_CONTEXT_UNPROVEN) {
		if (nxgptk.context_proven) {
			const nxinput_gptk_context previous_context = nxgptk.context;
			if (!nxgptk_clear_live_context("unproven-transition-release-failed")) {
				return;
			}
			nxgptk_partition_handoff(true, previous_context);
		}
		nxgptk.native_authority = true;
		if (!nxgptk.unproven_receipt_emitted) {
			nxgptk.unproven_receipt_emitted = true;
			print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=context-unproven",
					nxinput_gptk_event_evidence_schema()));
		}
		return;
	}
	const nxinput_gptk_context wanted = policy == TEARS_GPTK_CONTEXT_MENU
			? NXINPUT_GPTK_CONTEXT_MENU
			: NXINPUT_GPTK_CONTEXT_GAMEPLAY;
	const char *context_source = tears_gptk_context_source_for_scene(
			scene_utf8.get_data(), policy, tree_paused);
	const char *previous_source = nxinput_gptk_live_context_source(&nxgptk.live);
	if (!nxgptk.context_proven || wanted != nxgptk.context ||
			!previous_source || strcmp(previous_source, context_source) != 0) {
		const bool previous_context_proven =
				nxgptk.context_proven && !nxgptk.native_authority;
		const nxinput_gptk_context previous_context = nxgptk.context;
		if (!nxgptk_vector_release_all()) {
			nxgptk_mark_fatal("context-vector-release-failed", -1);
			return;
		}
		nxgptk_partition_handoff(previous_context_proven, previous_context);
		if (nxinput_gptk_live_set_context(&nxgptk.live, wanted, context_source) != 0) {
			nxgptk.context_proven = false;
			nxgptk.native_authority = true;
			if (nxinput_gptk_live_is_fatal(&nxgptk.live)) {
				nxgptk_mark_fatal("context-scalar-release-failed", -1);
				return;
			}
			print_line(vformat("nx/gptk: %s decision=PASSTHROUGH suppressed=false delivery=0 reason=context-rejected",
					nxinput_gptk_event_evidence_schema()));
			return;
		}
		nxgptk.context = wanted;
		nxgptk.context_proven = true;
		nxgptk.native_authority = false;
		nxgptk.unproven_receipt_emitted = false;
		print_line(vformat("nx/gptk: context proven=%s source=%s epoch=%d",
				nxgptk_context_name(), context_source,
				(int)nxinput_gptk_live_context_epoch(&nxgptk.live)));
		char context_line[320];
		if (tears_gptk_receipt_context_line(context_line, sizeof(context_line),
				nxinput_gptk_event_evidence_schema(), nxgptk_context_name(),
				context_source) == 0) {
			nxgptk_receipt_line(context_line);
		}
	}

	if (((nxgptk.handoff.controls | nxgptk.suppressed_handoff.controls) &
			 nxinput_godot_control_bit(NXINPUT_GPTK_LEFT_STICK)) == 0u) {
		const nxinput_gptk_live_result left_result = nxgptk_feed_vector_control(
				NXINPUT_GPTK_LEFT_STICK, nxgptk.left_x, nxgptk.left_y);
		(void)nxgptk_result_consumes(left_result, NXINPUT_GPTK_LEFT_STICK, false);
	}
	if (((nxgptk.handoff.controls | nxgptk.suppressed_handoff.controls) &
			 nxinput_godot_control_bit(NXINPUT_GPTK_RIGHT_STICK)) == 0u) {
		const nxinput_gptk_live_result right_result = nxgptk_feed_vector_control(
				NXINPUT_GPTK_RIGHT_STICK, nxgptk.right_x, nxgptk.right_y);
		(void)nxgptk_result_consumes(right_result, NXINPUT_GPTK_RIGHT_STICK, false);
	}
}

#else /* !FBDEV_ENABLED */

int nxgptk_godot_preinit() {
	return 0; /* auto */
}
void nxgptk_godot_init_once() {}
bool nxgptk_godot_active() {
	return false;
}
void nxgptk_godot_tick(double p_delta) {
	(void)p_delta;
}
bool nxgptk_godot_gamepad_button(int p_joy_id, int p_sdl_button, bool p_pressed) {
	(void)p_joy_id;
	(void)p_sdl_button;
	(void)p_pressed;
	return false;
}
bool nxgptk_godot_gamepad_axis(int p_joy_id, int p_sdl_axis, float p_value) {
	(void)p_joy_id;
	(void)p_sdl_axis;
	(void)p_value;
	return false;
}
bool nxgptk_godot_consume_quit_request() {
	return false;
}
bool nxgptk_godot_consume_fatal_request() {
	return false;
}
bool nxgptk_godot_gamepad_connected(int p_joy_id, SDL_Gamepad *p_gamepad) {
	(void)p_joy_id;
	(void)p_gamepad;
	return false;
}
bool nxgptk_godot_gamepad_disconnected(int p_joy_id) {
	(void)p_joy_id;
	return false;
}

#endif /* FBDEV_ENABLED */
