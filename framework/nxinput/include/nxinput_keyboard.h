/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_KEYBOARD_H
#define NXINPUT_KEYBOARD_H
/*
 * nxinput_keyboard -- 0.11.1 (F2/F3, E9a): the PHYSICAL KEYBOARD SOURCE.
 *
 *   physical key (evdev KEY_* or the adapter's keysym) + live modifiers
 *     -> canonical chord ("LCTRL+S", the schema-4 spelling)
 *     -> [keyboard.override.<ctx>] then [keyboard.base] of the owner map
 *     -> action (digital) -> the SAME output router the gamepad uses
 *        (nxinput_router: refcount per output => one press, one release,
 *         whoever holds it; the keyboard never fights the gamepad)
 *     -> registry token "key.<chord>" for prompts (positional Xbox tokens
 *        for gamepad, key tokens for keyboard: the same registry, E9a)
 *
 * Two BACKENDS, one edge stream: the EVENT backend receives key down/up
 * as they arrive; the POLLING backend receives a snapshot of every key
 * each frame and derives the edges itself. Both feed the same machine, so
 * an engine that polls (SDL_GetKeyboardState, Godot Input.is_key_pressed)
 * and an engine that receives events (SDL_KEYDOWN, MonoGame) produce
 * IDENTICAL router sequences -- the test proves it.
 *
 * A held chord releases when its key releases OR when one of its modifiers
 * releases (the chord no longer holds); a modifier pressed AFTER the key
 * never retro-changes the chord. Auto-repeat is ignored. Release-all on
 * focus loss / context change drops every held chord exactly once. An
 * action whose own @key output names the chord that fed it is a LOOP:
 * refused at the source (never emitted), counted.
 *
 * Pure: no I/O.
 */
#include "nxinput_gptk4.h"
#include "nxinput_route.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_KEYBOARD_MAX_HELD 16u
#define NXINPUT_KEYBOARD_KEYSYM_COUNT 96u

typedef struct nxinput_keyboard_held {
  uint8_t used; char keysym[24]; char chord[NXINPUT_GPTK4_KEY_MAX + 1];
  uint8_t mods; nxinput_route_output out; uint64_t edge_id;
} nxinput_keyboard_held;

typedef struct nxinput_keyboard {
  const nxinput_gptk4 *map;        /* not owned */
  nxinput_router *router;          /* not owned; shared with the gamepad source */
  uint64_t source_identity;        /* never the router's own identity */
  char context[24];
  uint8_t mods;                    /* live modifier bitmask (LCTRL..RGUI, bit order of the chord grammar) */
  uint8_t prev_snapshot[NXINPUT_KEYBOARD_KEYSYM_COUNT]; /* polling backend */
  nxinput_keyboard_held held[NXINPUT_KEYBOARD_MAX_HELD];
  uint64_t next_edge_id;
  unsigned presses, releases, refused_loop, refused_unbound, repeats_ignored, released_all, refused_router;
} nxinput_keyboard;

/* Bind to the owner map and the shared router. `source_identity` must
 * differ from the router's identity token (self-source refusal). */
int nxinput_keyboard_init(nxinput_keyboard *kb, const nxinput_gptk4 *map, nxinput_router *router, uint64_t source_identity);
/* Context for the [keyboard.override.<ctx>] overlay ("" = base only).
 * Changing it is a release-all. */
void nxinput_keyboard_set_context(nxinput_keyboard *kb, const char *context);
/* EVENT backend: one key edge. `keysym` is the schema-4 spelling ("A",
 * "SPACE", "LCTRL"...). value: 1 press, 0 release, 2 auto-repeat. Returns
 * 1 when an action edge reached the router, 0 when nothing was bound or a
 * modifier moved, -1 on invalid input. */
int nxinput_keyboard_event(nxinput_keyboard *kb, const char *keysym, int value);
/* POLLING backend: a snapshot of every keysym (index = position in
 * nxinput_keyboard_keysym_name(), 1 = down). Edges are derived against the
 * previous snapshot and fed to the event path. Returns the number of
 * action edges emitted. */
int nxinput_keyboard_poll(nxinput_keyboard *kb, const uint8_t *down, size_t count);
/* Focus lost / context change / fatal: release every held chord once. */
unsigned nxinput_keyboard_release_all(nxinput_keyboard *kb);
/* Keysym table (the schema-4 allow-list order). NULL past the end. */
const char *nxinput_keyboard_keysym_name(size_t index);
int nxinput_keyboard_keysym_index(const char *keysym);
/* evdev KEY_* -> keysym name (NULL when the key has no schema-4 name). */
const char *nxinput_keyboard_keysym_of_evdev(int key_code);
/* Router output for an action id: kind ENGINE_DIRECT, code = fnv1a32 of the id. */
uint32_t nxinput_keyboard_action_code(const char *action);
#ifdef __cplusplus
}
#endif
#endif
