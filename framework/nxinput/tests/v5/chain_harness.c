/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 H5/H8: the COMPLETE CHAIN on the host, one stimulus at a time, as a
 * harness the independent Python oracle (test_v5_chain.py) drives and judges:
 *
 *   EV_KEY/EV_ABS stimulus (fixture position)          -> argv
 *   -> provider ordinal (measured table of the domain)  nxinput_sdl_button_code
 *   -> CFW line translated for the provider             nxinput_translate_line
 *   -> canonical control (SDL semantic -> gptk4 slot)   this file's table
 *   -> owner decision (schema 4, context)               nxinput_gptk4_resolve
 *   -> exactly one primary typed route -> sink          nxinput_router
 *   -> prompt/glyph of the same mapping_generation      nxinput_registry
 *   -> release                                          nxinput_router
 *   also: keyboard source (F2) through the same router; trigger digital edge;
 *   stick 8-way digital directions; touch route kind (typed, no ordinal).
 *
 * It prints CHAIN lines; it never decides what is right. NX_CHAIN_MUTANT
 * selects a deliberate wrong implementation the oracle must catch:
 *   swap_ab | wrong_provider | no_release | double_route | stale_prompt |
 *   ordinal_prompt | keyboard_double | trigger_no_hysteresis
 * usage: chain_harness <provider-domain> <key-codes hex,csv> <cfw-line>
 *        <ev_key hex> <owner.gptk> <context> [keysym]
 */
#define _POSIX_C_SOURCE 200809L
#include "nxinput_axis_calib.h"
#include "nxinput_gptk4.h"
#include "nxinput_keyboard.h"
#include "nxinput_registry.h"
#include "nxinput_route.h"
#include "nxinput_sdl.h"
#include "nxinput_translate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *mut;
static int is_mut(const char *m) { return mut && strcmp(mut, m) == 0; }
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); b = malloc((size_t)n + 1); if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b; }
static nxinput_sdl_domain domain_of(const char *n) { int d; for (d = 1; d < (int)NXINPUT_SDL_DOMAIN_COUNT; d++) if (!strcmp(nxinput_sdl_domain_name((nxinput_sdl_domain)d), n)) return (nxinput_sdl_domain)d; return NXINPUT_SDL_DOMAIN_UNDECLARED; }
/* SDL semantic -> schema-4 slot */
static int slot_of(const char *sem) {
  static const struct { const char *s; int slot; } t[] = {{"a", NXINPUT_GPTK4_A}, {"b", NXINPUT_GPTK4_B}, {"x", NXINPUT_GPTK4_X}, {"y", NXINPUT_GPTK4_Y}, {"leftshoulder", NXINPUT_GPTK4_L1}, {"rightshoulder", NXINPUT_GPTK4_R1}, {"leftstick", NXINPUT_GPTK4_L3}, {"rightstick", NXINPUT_GPTK4_R3}, {"start", NXINPUT_GPTK4_START}, {"back", NXINPUT_GPTK4_SELECT}, {"guide", NXINPUT_GPTK4_GUIDE}, {"dpup", NXINPUT_GPTK4_UP}, {"dpdown", NXINPUT_GPTK4_DOWN}, {"dpleft", NXINPUT_GPTK4_LEFT}, {"dpright", NXINPUT_GPTK4_RIGHT}, {"lefttrigger", NXINPUT_GPTK4_L2_DIGITAL}, {"righttrigger", NXINPUT_GPTK4_R2_DIGITAL}};
  size_t i; for (i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].s, sem)) return t[i].slot; return -1;
}
static int presses, releases; static char last_action[80];
static void sink(void *u, const nxinput_route_output *o, int p, uint32_t g, uint32_t e, uint64_t s) { (void)u; (void)e; (void)s; if (p) presses++; else releases++; printf("CHAIN sink kind=%u code=%u pressed=%d mapping_generation=%u\n", o->kind, o->code, p, g); }
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu", "pause", "cursor"};
int main(int argc, char **argv) {
  nxinput_sdl_domain provider; unsigned long kb[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, ab[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
  nxinput_godot_caps caps; char out[1024]; nxinput_translate_evidence ev; nxinput_translate_result tr; int code, ordinal = -1; const char *sem = NULL;
  nxinput_gptk4 g4; nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0}; nxinput_gptk4_error e; char *owner; int slot;
  nxinput_router r; nxinput_registry reg; nxinput_prompt pr; nxinput_route_output o; const nxinput_gptk4_binding *b; const char *context; char *csv, *tok;
  if (argc < 7) { fprintf(stderr, "usage: chain_harness <domain> <keys> <line> <ev_key> <owner.gptk> <context> [keysym]\n"); return 2; }
  mut = getenv("NX_CHAIN_MUTANT");
  provider = domain_of(argv[1]); if (is_mut("wrong_provider")) provider = provider == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED ? NXINPUT_SDL_DOMAIN_SDL2_EVDEV : NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED;
  csv = strdup(argv[2]); for (tok = strtok(csv, ","); tok; tok = strtok(NULL, ",")) { unsigned k = (unsigned)strtoul(tok, NULL, 16); kb[k / 64] |= 1ul << (k % 64); }
  ab[0] |= 0x3ful; ab[0] |= 3ul << 0x10; /* x y z rx ry rz + hat0 */
  nxinput_godot_caps_init(&caps, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS);
  code = (int)strtol(argv[4], NULL, 16); context = argv[6];
  /* 1. provider ordinal of the stimulus */
  ordinal = nxinput_sdl_button_ordinal(provider, &caps, (unsigned)code);
  printf("CHAIN provider=%s ev_key=0x%x ordinal=%d\n", nxinput_sdl_domain_name(provider), code, ordinal);
  /* 2. the CFW line translated for the provider */
  tr = nxinput_translate_line(argv[3], kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, provider, NULL, out, sizeof out, &ev);
  printf("CHAIN translate=%s source_domain=%s rewritten=%u\n", nxinput_translate_result_name(tr), nxinput_sdl_domain_name((nxinput_sdl_domain)ev.source_domain), ev.rewritten_bindings);
  if (tr == NXINPUT_TRANSLATE_REJECTED || tr == NXINPUT_TRANSLATE_ERROR) { printf("CHAIN result=source-yields\n"); return 0; }
  /* 3. semantic the provider delivers for the ordinal */
  { char pat[24]; const char *p; const char *cur = out; snprintf(pat, sizeof pat, ":b%d,", ordinal);
    p = strstr(cur, pat);
    if (p && ordinal >= 0) { const char *k = p; while (k > out && k[-1] != ',') k--; { static char semb[32]; snprintf(semb, sizeof semb, "%.*s", (int)(p - k), k); sem = semb; } } }
  if (is_mut("swap_ab") && sem) { if (!strcmp(sem, "a")) sem = "b"; else if (!strcmp(sem, "b")) sem = "a"; }
  printf("CHAIN semantic=%s\n", sem ? sem : "-");
  /* 4. owner decision */
  owner = slurp(argv[5]); if (!owner || nxinput_gptk4_parse(owner, strlen(owner), &c, &g4, &e) != 0) { printf("CHAIN result=owner-invalid code=%d\n", e.code); return 1; }
  slot = sem ? slot_of(sem) : -1;
  b = slot >= 0 ? nxinput_gptk4_resolve(&g4, context, (nxinput_gptk4_control)slot) : NULL;
  printf("CHAIN slot=%s binding=%s action=%s\n", slot >= 0 ? nxinput_gptk4_control_name((nxinput_gptk4_control)slot) : "-", b ? (b->kind == NXINPUT_GPTK4_ACTION ? "action" : b->kind == NXINPUT_GPTK4_NULL ? "null" : "native") : "-", b && b->kind == NXINPUT_GPTK4_ACTION ? b->action : "-");
  /* 5. route -> sink; 6. prompt; 7. release */
  nxinput_router_init(&r, sink, NULL, 0xFEEDu); nxinput_router_lease_acquire(&r, 1);
  nxinput_registry_bind(&reg, &g4, 5, 1, 0);
  if (b && b->kind == NXINPUT_GPTK4_ACTION) {
    o.kind = NXINPUT_ROUTE_ENGINE_DIRECT; o.code = nxinput_keyboard_action_code(b->action); o.player = 0;
    nxinput_router_press(&r, 0x77, 0x0A, &o, NULL, 0);
    if (is_mut("double_route")) { nxinput_route_output o2 = o; o2.kind = NXINPUT_ROUTE_SDL_KEYBOARD; nxinput_router_press(&r, 0x78, 0x0A, &o2, NULL, 0); }
    snprintf(last_action, sizeof last_action, "%s", b->action);
    nxinput_registry_prompt(&reg, b->action, context, 0, &pr);
    if (is_mut("stale_prompt")) pr.mapping_generation = 4;
    printf("CHAIN prompt token=%s text=%s mapping_generation=%u%s\n", pr.token, is_mut("ordinal_prompt") ? "Button 10" : pr.text, pr.mapping_generation, pr.unbound ? " unbound=1" : "");
    if (!is_mut("no_release")) nxinput_router_release(&r, 0x77, &o, NULL, 0);
    if (is_mut("double_route")) { nxinput_route_output o2 = o; o2.kind = NXINPUT_ROUTE_SDL_KEYBOARD; nxinput_router_release(&r, 0x78, &o2, NULL, 0); }
  }
  /* 8. keyboard source through the SAME router (F2): keysym -> action -> one press/one release */
  if (argc > 7 && argv[7][0]) {
    nxinput_keyboard kbd; char *buf = malloc(strlen(owner) + 200); snprintf(buf, strlen(owner) + 200, "%s\n[keyboard.base]\n%s = action:player.jump\n", owner, argv[7]);
    if (nxinput_gptk4_parse(buf, strlen(buf), &c, &g4, &e) == 0 && nxinput_keyboard_init(&kbd, &g4, &r, 0x0Bu) == 0) {
      int before = presses;
      nxinput_keyboard_event(&kbd, argv[7], 1);
      if (is_mut("keyboard_double")) { nxinput_route_output ko = {NXINPUT_ROUTE_ENGINE_DIRECT, 0, 0}; ko.code = nxinput_keyboard_action_code("player.jump"); nxinput_router_press(&r, 0x79, 0x0C, &ko, NULL, 0); nxinput_router_release(&r, 0x79, &ko, NULL, 0); nxinput_router_press(&r, 0x7a, 0x0C, &ko, NULL, 0); }
      nxinput_keyboard_event(&kbd, argv[7], 0);
      printf("CHAIN keyboard keysym=%s presses=%d releases=%d held=%u\n", argv[7], presses - before, releases, r.held_count);
    }
    free(buf);
  }
  /* 9. trigger digital edge with hysteresis; 10. stick 8-way; 11. touch route kind */
  { nxinput_trigger_digital tg; int e1, e2, e3; nxinput_trigger_digital_init(&tg, 0.55f, 0.40f);
    e1 = nxinput_trigger_digital_update(&tg, 0.6f); e2 = nxinput_trigger_digital_update(&tg, is_mut("trigger_no_hysteresis") ? 0.30f : 0.50f); e3 = nxinput_trigger_digital_update(&tg, 0.30f);
    printf("CHAIN trigger edges=%d,%d,%d\n", e1, e2, e3); }
  { nxinput_stick_digital st; float ox, oy; nxinput_stick_digital_init(&st, 0.55f, 0.40f, 1, 1); nxinput_axis_radial(0.8f, 0.8f, 0.1f, &ox, &oy);
    printf("CHAIN stick8 x=%.2f y=%.2f\n", (double)ox, (double)oy); }
  { nxinput_route_output tch = {NXINPUT_ROUTE_ENGINE_TOUCH, 7, 0}; nxinput_router_press(&r, 0x90, 0x0D, &tch, NULL, 0); nxinput_router_release(&r, 0x90, &tch, NULL, 0); printf("CHAIN touch kind=%u typed=1\n", tch.kind); }
  printf("CHAIN result=done presses=%d releases=%d held=%u\n", presses, releases, r.held_count);
  free(owner); free(csv);
  return 0;
}
