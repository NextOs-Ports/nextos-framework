/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TEARSCAPE_GPTK_CONTEXT_POLICY_H
#define TEARSCAPE_GPTK_CONTEXT_POLICY_H

#include <stdbool.h>
#include <string.h>

/* Port-specific, dependency-free context policy. Unknown scenes are never
 * guessed: the caller must leave the physical input on Godot's native path. */
typedef enum TearsGptkContextPolicy {
	TEARS_GPTK_CONTEXT_UNPROVEN = 0,
	TEARS_GPTK_CONTEXT_MENU = 1,
	TEARS_GPTK_CONTEXT_GAMEPLAY = 2,
} TearsGptkContextPolicy;

static inline bool tears_gptk_path_has_prefix(const char *p_path, const char *p_prefix) {
	return p_path && p_prefix && strncmp(p_path, p_prefix, strlen(p_prefix)) == 0;
}

static inline TearsGptkContextPolicy tears_gptk_context_for_scene(
		const char *p_scene_path, bool p_tree_paused) {
	if (!p_scene_path || !p_scene_path[0]) {
		return TEARS_GPTK_CONTEXT_UNPROVEN;
	}
	if (tears_gptk_path_has_prefix(p_scene_path, "res://scenes/ui/")) {
		return TEARS_GPTK_CONTEXT_MENU;
	}
	if (strcmp(p_scene_path, "res://scenes/map.tscn") == 0 ||
			tears_gptk_path_has_prefix(p_scene_path, "res://scenes/level/")) {
		/* Tearscape's UiManager is the sole writer of SceneTree.paused and
		 * brackets every in-game menu/dialog/screen with it. Its selectors use
		 * custom focus, so gui_get_focus_owner() is not authoritative. */
		return p_tree_paused ? TEARS_GPTK_CONTEXT_MENU : TEARS_GPTK_CONTEXT_GAMEPLAY;
	}
	return TEARS_GPTK_CONTEXT_UNPROVEN;
}

/* StartGameScreen identifies P1/P2 from the physical JoypadButton event used
 * by start_coop.  Its X/WEST event must therefore bypass every editable GPTK
 * owner mapping, including `null`, instead of being synthesized later. */
static inline bool tears_gptk_scene_requires_native_start_coop(
		const char *p_scene_path) {
	return p_scene_path &&
			strcmp(p_scene_path,
					"res://scenes/ui/screens/start_game_screen.tscn") == 0;
}

/* The source is an authority epoch, not only a diagnostic label. Two scenes
 * may share MENU actions but still differ on which physical controls must stay
 * native. Distinguishing start-coop forces the existing handoff partition to
 * release/suppress the old owner before X changes authority. */
static inline const char *tears_gptk_context_source_for_scene(
		const char *p_scene_path, TearsGptkContextPolicy p_policy,
		bool p_tree_paused) {
	if (p_policy == TEARS_GPTK_CONTEXT_MENU) {
		if (tears_gptk_scene_requires_native_start_coop(p_scene_path)) {
			return "scene:ui:start-coop";
		}
		return p_tree_paused &&
				!tears_gptk_path_has_prefix(p_scene_path, "res://scenes/ui/")
				? "scene:gameplay-overlay"
				: "scene:ui";
	}
	if (p_policy == TEARS_GPTK_CONTEXT_GAMEPLAY) {
		return p_scene_path && strcmp(p_scene_path, "res://scenes/map.tscn") == 0
				? "scene:map"
				: "scene:level";
	}
	return NULL;
}

#endif /* TEARSCAPE_GPTK_CONTEXT_POLICY_H */
