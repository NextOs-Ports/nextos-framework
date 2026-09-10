/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / G1-G2, G4-G5: registry, Xbox default, epochs, remap in the same session. */
#include "../../include/nxinput_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"menu.confirm", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu"};
static char *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); b = malloc(*n + 1); fread(b, 1, *n, f); b[*n] = 0; fclose(f); return b; }
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 1, NULL, 0}; nxinput_gptk4 g, g2; nxinput_gptk4_error e; nxinput_registry r; nxinput_prompt p; nxinput_binding_ref refs[8]; size_t n; char *t;
  t = slurp(argc > 1 ? argv[1] : "tests/v5/corpus/fp2-complete.gptk", &n);
  CHECK(t && nxinput_gptk4_parse(t, n, &c, &g, &e) == 0, "fixture parses");
  nxinput_registry_bind(&r, &g, 7, 3, 1);
  CHECK(nxinput_registry_prompt(&r, "fp2.primary", "gameplay", 0, &p) == 0 && !strcmp(p.token, "face.south") && !strcmp(p.text, "A") && p.mapping_generation == 7 && p.context_epoch == 3 && p.source_edge_id == 0 && p.modality == NXINPUT_MODALITY_GAMEPAD, "initial prompt: face.south shows 'A', epochs bound, source_edge none, default modality");
  CHECK(!strcmp(nxinput_registry_glyph("face.south", "nintendo"), "B") && !strcmp(nxinput_registry_glyph("face.south", "xbox"), "A"), "opt-in glyph theme changes the drawing only; token/position unchanged");
  CHECK(nxinput_registry_prompt(&r, "fp2.special", "menu", 0, &p) == 0 && p.unbound, "action nulled in menu override => unbound prompt in menu");
  CHECK(nxinput_registry_prompt(&r, "fp2.special", "gameplay", 0, &p) == 0 && !strcmp(p.text, "X"), "same action in gameplay => X");
  /* remap A -> R2 in one transaction: new generation, prompt follows */
  { char *x = (char *)malloc(n + 256); const char *a = strstr(t, "A = action:fp2.primary"); const char *tr = strstr(t, "[trigger.right]\nmode = analog\nanalog = native\ndigital = null"); size_t la = strlen("A = action:fp2.primary"), ltr = strlen("[trigger.right]\nmode = analog\nanalog = native\ndigital = null");
    x[0] = 0; strncat(x, t, (size_t)(a - t)); strcat(x, "A = null"); strncat(x, a + la, (size_t)(tr - (a + la))); strcat(x, "[trigger.right]\nmode = digital\nanalog = null\ndigital = action:fp2.primary"); strcat(x, tr + ltr);
    CHECK(nxinput_gptk4_parse(x, strlen(x), &c, &g2, &e) == 0, "remapped owner parses"); if (fails) printf("     %d %u:%u %s\n", e.code, e.line, e.column, e.what);
    nxinput_registry_bind(&r, &g2, 8, 4, 1);
    CHECK(nxinput_registry_prompt(&r, "fp2.primary", "gameplay", 0, &p) == 0 && !strcmp(p.token, "r2") && !strcmp(p.text, "RT") && p.mapping_generation == 8, "after remap A->R2 the primary prompt is R2 in the new generation");
    CHECK(nxinput_registry_bindings_for_action(&r, "fp2.primary", "gameplay", 0, refs, 8) == 1 && !strcmp(refs[0].token, "r2"), "face.south no longer binds the action");
    free(x); }
  /* keyboard alternate + modality debounce */
  { char buf[8192]; snprintf(buf, sizeof buf, "%s\n[keyboard.base]\nSPACE = action:fp2.primary\n", t); CHECK(nxinput_gptk4_parse(buf, strlen(buf), &c, &g2, &e) == 0, "owner with keyboard source parses"); nxinput_registry_bind(&r, &g2, 9, 5, 1);
    CHECK(nxinput_registry_bindings_for_action(&r, "fp2.primary", "gameplay", 0, refs, 8) == 2 && refs[0].primary && refs[0].modality == NXINPUT_MODALITY_GAMEPAD && !strcmp(refs[1].token, "key.SPACE"), "ordered set: gamepad primary, keyboard alternate");
    nxinput_registry_observe_input(&r, NXINPUT_MODALITY_KEYBOARD, 41); CHECK(nxinput_registry_prompt(&r, "fp2.primary", "gameplay", 0, &p) == 0 && !strcmp(p.text, "A"), "one keyboard sample does not flip the prompt (debounce)");
    nxinput_registry_observe_input(&r, NXINPUT_MODALITY_KEYBOARD, 42); CHECK(nxinput_registry_prompt(&r, "fp2.primary", "gameplay", 0, &p) == 0 && !strcmp(p.text, "SPACE") && p.modality_epoch == 2 && p.source_edge_id == 42, "keyboard modality after debounce: prompt shows the key, modality epoch and edge bound"); }
  CHECK(nxinput_registry_text_has_raw_ordinal("Press Joystick Button 10") && nxinput_registry_text_has_raw_ordinal("Axis -2 to move") && !nxinput_registry_text_has_raw_ordinal("Press A") && !nxinput_registry_text_has_raw_ordinal("Button"), "raw ordinal gate");
  free(t);   /* 0.11.1: the human glyph of an SDL ordinal -- never a number */
  { char tok[32]; const char *g; int b, bad = 0;
    g = nxinput_registry_glyph_for_sdl(0, 0, "xbox", tok, sizeof tok);
    CHECK(g && !strcmp(g, "A") && !strcmp(tok, "face.south"), "SDL button 0 -> face.south -> \"A\" (Xbox pack)");
    g = nxinput_registry_glyph_for_sdl(6, 0, "xbox", tok, sizeof tok);
    CHECK(g && !strcmp(g, "Start") && !strcmp(tok, "start"), "SDL button 6 -> start -> \"Start\"");
    g = nxinput_registry_glyph_for_sdl(4, 0, "xbox", tok, sizeof tok);
    CHECK(g && !strcmp(g, "Select"), "SDL button 4 -> select -> \"Select\"");
    g = nxinput_registry_glyph_for_sdl(5, 1, "xbox", tok, sizeof tok);
    CHECK(g && !strcmp(g, "RT") && !strcmp(tok, "r2"), "SDL axis 5 -> r2 -> \"RT\"");
    g = nxinput_registry_glyph_for_sdl(0, 0, "nintendo", tok, sizeof tok);
    CHECK(g && !strcmp(g, "B"), "opt-in nintendo style draws the south face as \"B\" (drawing only; the token stays face.south)");
    for (b = 0; b < 21; b++) { g = nxinput_registry_glyph_for_sdl(b, 0, "xbox", tok, sizeof tok); if (g == NULL) continue; if (nxinput_registry_text_has_raw_ordinal(g) || strspn(g, "0123456789") == strlen(g)) bad++; }
    CHECK(bad == 0, "MUTANT killed: a numeric/ordinal glyph for any SDL button (none of 21 semantics prints a number)");
    CHECK(nxinput_registry_glyph_for_sdl(99, 0, "xbox", tok, sizeof tok) == NULL && nxinput_registry_glyph_for_sdl(-1, 1, "xbox", tok, sizeof tok) == NULL && tok[0] == '\0', "unknown semantic -> NULL (never \"Button 99\")");
    CHECK(nxinput_registry_glyph_for_token("dpad.up", "xbox") && !strcmp(nxinput_registry_glyph_for_token("dpad.up", "xbox"), "D-Pad Up") && nxinput_registry_glyph_for_token("button.10", "xbox") == NULL, "token glyph: known -> text, unknown -> NULL"); }
    /* 0.11.1 (M1c NEG-2): isolated number on a prompt surface */
  CHECK(nxinput_registry_prompt_text_has_isolated_number("PRESS 10 TO BEGIN") && !nxinput_registry_prompt_text_clean("PRESS 10 TO BEGIN"), "MUTANT killed: \"PRESS 10 TO BEGIN\" passing the prompt gate (isolated ordinal inside the icon)");
  CHECK(nxinput_registry_prompt_text_has_isolated_number("[10]") && nxinput_registry_prompt_text_has_isolated_number("Dragon Cyclone (2)") && nxinput_registry_prompt_text_has_isolated_number("1"), "bracketed and bare 1-2 digit tokens are ordinals");
  CHECK(!nxinput_registry_prompt_text_has_isolated_number("PRESS A TO BEGIN") && !nxinput_registry_prompt_text_has_isolated_number("Level 100") && !nxinput_registry_prompt_text_has_isolated_number("F1 help") && !nxinput_registry_prompt_text_has_isolated_number("Score 12345") && nxinput_registry_prompt_text_clean("Press Start"), "glyph text, long numbers, F1 and scores are clean");
  printf(fails ? "v5-registry: FAIL\n" : "v5-registry: OK\n"); return fails ? 1 : 0;
}
