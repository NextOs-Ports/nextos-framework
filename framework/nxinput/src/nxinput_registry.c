/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_registry.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const tokens[NXINPUT_GPTK4_SLOT_COUNT] = {
  "face.south", "face.east", "face.west", "face.north", "l1", "r1", "l3", "r3", "start", "select", "guide",
  "dpad.up", "dpad.down", "dpad.left", "dpad.right",
  "left_stick.up", "left_stick.down", "left_stick.left", "left_stick.right",
  "right_stick.up", "right_stick.down", "right_stick.left", "right_stick.right",
  "l2", "r2", "left_stick", "right_stick", "l2", "r2"};
const char *nxinput_registry_token_for_slot(nxinput_gptk4_control s) { return (unsigned)s < NXINPUT_GPTK4_SLOT_COUNT ? tokens[s] : "?"; }

void nxinput_registry_bind(nxinput_registry *r, const nxinput_gptk4 *map, uint32_t gen, uint32_t epoch, int native_ok) {
  if (!r) return;
  memset(r, 0, sizeof *r); r->map = map; r->mapping_generation = gen; r->context_epoch = epoch; r->modality_epoch = 1; r->native_readback_ok = native_ok ? 1 : 0; r->last_modality = NXINPUT_MODALITY_GAMEPAD;
}
void nxinput_registry_observe_input(nxinput_registry *r, nxinput_modality m, uint64_t edge) {
  if (!r) return;
  if (m != r->last_modality) { if (++r->modality_debounce_left >= 2) { r->last_modality = (uint8_t)m; r->modality_epoch++; r->modality_debounce_left = 0; } }
  else r->modality_debounce_left = 0;
  r->last_source_edge = edge;
}

unsigned nxinput_registry_bindings_for_action(const nxinput_registry *r, const char *action, const char *context, uint8_t player, nxinput_binding_ref *refs, unsigned cap) {
  unsigned n = 0; int s; unsigned k;
  if (!r || !r->map || !action) return 0;
  for (s = 0; s < NXINPUT_GPTK4_SLOT_COUNT && n < cap; s++) {
    const nxinput_gptk4_binding *b = nxinput_gptk4_resolve(r->map, context, (nxinput_gptk4_control)s);
    if (!b || b->kind != NXINPUT_GPTK4_ACTION || strcmp(b->action, action)) continue;
    memset(&refs[n], 0, sizeof refs[n]); refs[n].modality = NXINPUT_MODALITY_GAMEPAD; refs[n].player = player; refs[n].primary = n == 0; refs[n].available = 1;
    snprintf(refs[n].token, sizeof refs[n].token, "%s", tokens[s]); refs[n].mapping_generation = r->mapping_generation; n++;
    if (b->key[0] && n < cap) { /* @key output of the same action: alternate, keyboard modality, never primary */
      memset(&refs[n], 0, sizeof refs[n]); refs[n].modality = NXINPUT_MODALITY_KEYBOARD; refs[n].player = player; refs[n].available = 1;
      snprintf(refs[n].token, sizeof refs[n].token, "key.%s", b->key); refs[n].mapping_generation = r->mapping_generation; n++;
    }
  }
  for (k = 0; k < r->map->keybinds && n < cap; k++) {
    const nxinput_gptk4_keybind *kb = &r->map->keyboard[k];
    if (kb->binding.kind != NXINPUT_GPTK4_ACTION || strcmp(kb->binding.action, action)) continue;
    if (kb->context[0] && (!context || strcmp(kb->context, context))) continue;
    memset(&refs[n], 0, sizeof refs[n]); refs[n].modality = NXINPUT_MODALITY_KEYBOARD; refs[n].player = player; refs[n].primary = n == 0; refs[n].available = 1;
    snprintf(refs[n].token, sizeof refs[n].token, "key.%s", kb->chord); refs[n].mapping_generation = r->mapping_generation; n++;
  }
  return n;
}

int nxinput_registry_prompt(const nxinput_registry *r, const char *action, const char *context, uint8_t player, nxinput_prompt *out) {
  nxinput_binding_ref refs[16]; unsigned n, i; const nxinput_binding_ref *pick = NULL;
  if (!r || !out) return -1;
  memset(out, 0, sizeof *out);
  out->mapping_generation = r->mapping_generation; out->context_epoch = r->context_epoch; out->modality_epoch = r->modality_epoch; out->source_edge_id = r->last_source_edge; out->modality = r->last_modality;
  n = nxinput_registry_bindings_for_action(r, action, context, player, refs, 16);
  for (i = 0; i < n; i++) if (refs[i].modality == r->last_modality) { pick = &refs[i]; break; }
  if (!pick && n) pick = &refs[0];
  if (!pick) { out->unbound = 1; snprintf(out->text, sizeof out->text, "%s", "unbound"); return 0; }
  snprintf(out->token, sizeof out->token, "%s", pick->token);
  snprintf(out->text, sizeof out->text, "%s", nxinput_registry_glyph(pick->token, r->map ? r->map->glyph_style : "xbox"));
  return 0;
}

const char *nxinput_registry_glyph(const char *token, const char *style) {
  static const struct { const char *t, *xbox, *nintendo, *ps; } g[] = {
    {"face.south", "A", "B", "Cross"}, {"face.east", "B", "A", "Circle"}, {"face.west", "X", "Y", "Square"}, {"face.north", "Y", "X", "Triangle"},
    {"l1", "LB", "L", "L1"}, {"r1", "RB", "R", "R1"}, {"l2", "LT", "ZL", "L2"}, {"r2", "RT", "ZR", "R2"}, {"l3", "LS", "L3", "L3"}, {"r3", "RS", "R3", "R3"},
    {"start", "Start", "+", "Options"}, {"select", "Select", "-", "Share"}, {"guide", "Guide", "Home", "PS"},
    {"dpad.up", "D-Pad Up", "D-Pad Up", "D-Pad Up"}, {"dpad.down", "D-Pad Down", "D-Pad Down", "D-Pad Down"}, {"dpad.left", "D-Pad Left", "D-Pad Left", "D-Pad Left"}, {"dpad.right", "D-Pad Right", "D-Pad Right", "D-Pad Right"},
    {"left_stick", "Left Stick", "Left Stick", "Left Stick"}, {"right_stick", "Right Stick", "Right Stick", "Right Stick"},
    {"left_stick.up", "Left Stick Up", "Left Stick Up", "Left Stick Up"}, {"left_stick.down", "Left Stick Down", "Left Stick Down", "Left Stick Down"}, {"left_stick.left", "Left Stick Left", "Left Stick Left", "Left Stick Left"}, {"left_stick.right", "Left Stick Right", "Left Stick Right", "Left Stick Right"},
    {"right_stick.up", "Right Stick Up", "Right Stick Up", "Right Stick Up"}, {"right_stick.down", "Right Stick Down", "Right Stick Down", "Right Stick Down"}, {"right_stick.left", "Right Stick Left", "Right Stick Left", "Right Stick Left"}, {"right_stick.right", "Right Stick Right", "Right Stick Right", "Right Stick Right"}};
  static char keybuf[64]; size_t i;
  if (!token) return "?";
  if (strncmp(token, "key.", 4) == 0) { snprintf(keybuf, sizeof keybuf, "%s", token + 4); for (i = 0; keybuf[i]; i++) if (keybuf[i] == '+') keybuf[i] = '+'; return keybuf; }
  for (i = 0; i < sizeof g / sizeof g[0]; i++) if (!strcmp(g[i].t, token)) {
    if (style && !strcmp(style, "nintendo")) return g[i].nintendo;
    if (style && !strcmp(style, "playstation")) return g[i].ps;
    return g[i].xbox;
  }
  return "?";
}

int nxinput_registry_text_has_raw_ordinal(const char *text) {
  const char *p; static const char *const bad[] = {"Button ", "Axis ", "Joystick Button ", "button "};
  size_t i;
  if (!text) return 0;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) for (p = strstr(text, bad[i]); p; p = strstr(p + 1, bad[i])) {
    const char *q = p + strlen(bad[i]); if (*q == '+' || *q == '-') q++; if (isdigit((unsigned char)*q)) return 1;
  }
  return 0;
}

/* SDL2 GameController button semantics (SDL_GameControllerButton order) and
 * axis semantics (SDL_GameControllerAxis order) -> positional tokens. */
static const char *const sdl_button_tokens[] = {
  "face.south", "face.east", "face.west", "face.north", "select", "guide", "start", "l3", "r3", "l1", "r1",
  "dpad.up", "dpad.down", "dpad.left", "dpad.right", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"};
static const char *const sdl_axis_tokens[] = {"left_stick", "left_stick", "right_stick", "right_stick", "l2", "r2"};

const char *nxinput_registry_glyph_for_token(const char *token, const char *style) {
  const char *g = nxinput_registry_glyph(token, style);
  return (g == NULL || strcmp(g, "?") == 0) ? NULL : g;
}

const char *nxinput_registry_glyph_for_sdl(int sdl_semantic, int is_axis, const char *style, char *token, size_t cap) {
  const char *t;
  if (token != NULL && cap > 0) token[0] = '\0';
  if (sdl_semantic < 0) return NULL;
  if (is_axis) {
    if ((size_t)sdl_semantic >= sizeof sdl_axis_tokens / sizeof sdl_axis_tokens[0]) return NULL;
    t = sdl_axis_tokens[sdl_semantic];
  } else {
    if ((size_t)sdl_semantic >= sizeof sdl_button_tokens / sizeof sdl_button_tokens[0]) return NULL;
    t = sdl_button_tokens[sdl_semantic];
  }
  if (token != NULL && cap > 0) snprintf(token, cap, "%s", t);
  return nxinput_registry_glyph_for_token(t, style);
}

/* 0.11.1 (M1c NEG-2): on a PROMPT SURFACE (text where a glyph was declared)
 * an ISOLATED 1-2 digit token -- "PRESS 10 TO BEGIN", "[10]", "(2)" -- is a
 * raw ordinal leaking through an icon or a formatter, whatever words
 * surround it. Longer numbers (scores, "Level 100") and digits glued to
 * letters ("F1", "2x") are not ordinals here. */
int nxinput_registry_prompt_text_has_isolated_number(const char *text) {
  const char *p;
  if (!text) return 0;
  for (p = text; *p; p++) {
    if (isdigit((unsigned char)*p)) {
      const char *q = p; int n = 0;
      while (isdigit((unsigned char)*q)) { q++; n++; }
      {
        char before = p > text ? p[-1] : ' ', after = *q;
        int lead_ok = before == ' ' || before == '[' || before == '(' || before == '{' || before == '<' || before == '\t' || p == text;
        int trail_ok = after == '\0' || after == ' ' || after == ']' || after == ')' || after == '}' || after == '>' || after == '\t' || after == '.' || after == ',' || after == '!';
        if (n >= 1 && n <= 2 && lead_ok && trail_ok) return 1;
      }
      p = q - 1;
    }
  }
  return 0;
}
int nxinput_registry_prompt_text_clean(const char *text) {
  return !nxinput_registry_text_has_raw_ordinal(text) && !nxinput_registry_prompt_text_has_isolated_number(text);
}
