/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_REGISTRY_H
#define NXINPUT_REGISTRY_H
/*
 * nxinput_registry -- V5 (mission 7): the BINDING REGISTRY behind prompts
 * and glyphs, fed by the active NEXTOS_CONTROLLERS/4 generation.
 *
 *   bindings_for_action(action, context, player)  -> ordered set
 *   prompt_choice_for_action(...)                 -> one token, epochs bound
 *   glyph_for_token(token, style)                 -> human text/glyph id
 *
 * Tokens are POSITIONAL (face.south ...), never raw ordinals. The default
 * pack is Xbox on every port: face.south shows "A", face.east "B",
 * face.west "X", face.north "Y". An opt-in GLYPH_STYLE changes the drawing
 * only. `native` yields a prompt only after provider readback (the caller
 * passes the readback-proved physical position for the SDL semantic).
 * Every prompt carries (mapping_generation, context_epoch) and, when input
 * driven, (modality_epoch, source_edge_id); an initial prompt declares the
 * default modality and source_edge_id = none (0).
 */
#include "nxinput_gptk4.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum nxinput_modality { NXINPUT_MODALITY_GAMEPAD = 0, NXINPUT_MODALITY_KEYBOARD, NXINPUT_MODALITY_TOUCH } nxinput_modality;

typedef struct nxinput_binding_ref {
  uint8_t modality; uint8_t player; uint8_t primary; uint8_t available;
  char token[56];      /* face.south, l2, left_stick.up, key.space ... */
  uint32_t mapping_generation;
} nxinput_binding_ref;

typedef struct nxinput_prompt {
  char token[56]; char text[48]; uint8_t unbound;
  uint32_t mapping_generation, context_epoch, modality_epoch; uint64_t source_edge_id; uint8_t modality;
} nxinput_prompt;

typedef struct nxinput_registry {
  const nxinput_gptk4 *map; uint32_t mapping_generation, context_epoch, modality_epoch;
  uint8_t last_modality; uint64_t last_source_edge; uint32_t modality_debounce_left;
  /* readback-proved: for each SDL/native semantic slot, whether the provider
   * confirmed the physical position (needed before a `native` prompt). */
  uint8_t native_readback_ok;
} nxinput_registry;

void nxinput_registry_bind(nxinput_registry *r, const nxinput_gptk4 *map, uint32_t mapping_generation, uint32_t context_epoch, int native_readback_ok);
/* input-driven modality with debounce (samples). */
void nxinput_registry_observe_input(nxinput_registry *r, nxinput_modality m, uint64_t source_edge_id);
/* Ordered set: fills up to cap refs, returns count. */
unsigned nxinput_registry_bindings_for_action(const nxinput_registry *r, const char *action, const char *context, uint8_t player, nxinput_binding_ref *refs, unsigned cap);
/* The prompt: primary binding of the active modality; unbound when none. */
int nxinput_registry_prompt(const nxinput_registry *r, const char *action, const char *context, uint8_t player, nxinput_prompt *out);
/* Token -> human text for a glyph style ("xbox" default). Never a raw ordinal. */
const char *nxinput_registry_glyph(const char *token, const char *style);
/* Gate helper (G5): 1 when `text` contains a raw ordinal such as "Button 10", "Axis -2", "Joystick Button 3". */
int nxinput_registry_text_has_raw_ordinal(const char *text);
/* 0.11.1 (M1c NEG-2): 1 when a PROMPT-surface text carries an isolated 1-2
 * digit token ("PRESS 10 TO BEGIN", "[10]"): an ordinal leaking through an
 * icon/formatter. The OCR proves the screen; this proves the source text. */
int nxinput_registry_prompt_text_has_isolated_number(const char *text);
/* Both gates at once: 1 when the prompt text is clean. */
int nxinput_registry_prompt_text_clean(const char *text);
const char *nxinput_registry_token_for_slot(nxinput_gptk4_control slot);

/* 0.11.1: the HUMAN glyph for an SDL GameController semantic (the number
 * SDL_GameControllerButton/Axis carries: 0 = A/face.south ... as SDL2
 * defines them) under a glyph style. The positional token is returned in
 * `token` (cap >= 24). Returns the glyph text ("A", "Start", "Left Stick"
 * ...) or NULL when the semantic has no glyph -- NEVER a digit string, never
 * "Button N": an engine that used to print the raw ordinal calls this
 * instead. `is_axis` selects the axis table (leftx.. righttrigger). */
const char *nxinput_registry_glyph_for_sdl(int sdl_semantic, int is_axis, const char *style, char *token, size_t cap);
/* The same for a POSITIONAL token (face.south, l2, dpad.up...). NULL when
 * unknown. Convenience for adapters that already speak tokens. */
const char *nxinput_registry_glyph_for_token(const char *token, const char *style);
#ifdef __cplusplus
}
#endif
#endif
