#define _GNU_SOURCE 1
/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 F2/F3/E9a: physical keyboard -> actions through the shared router.
 * Event and polling backends produce the same sequence; keyboard + gamepad
 * on one action = one press/one release (refcount); chord releases with its
 * modifier; overlay context; loop refused; release-all; auto-repeat ignored;
 * prompts for keyboard bindings are key tokens, never digits. */
#include "../../include/nxinput_keyboard.h"
#include "../../include/nxinput_registry.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu", "pause", "cursor"};
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b; }
static char trace[2048];
static void sink(void *u, const nxinput_route_output *o, int p, uint32_t g, uint32_t e, uint64_t src) { char b[48]; (void)u; (void)g; (void)e; (void)src;
  const char *name = o->code == nxinput_keyboard_action_code("player.jump") ? "jump" : o->code == nxinput_keyboard_action_code("menu.confirm") ? "confirm" : o->code == nxinput_keyboard_action_code("fp2.pause") ? "pause" : o->code == nxinput_keyboard_action_code("menu.cancel") ? "cancel" : "?";
  snprintf(b, sizeof b, "%s%c ", name, p ? '+' : '-'); strcat(trace, b); }
static nxinput_gptk4 g4;
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0};
  nxinput_gptk4_error e; nxinput_router r; nxinput_keyboard kb, kp; char *t; char buf[8192]; uint8_t snap[NXINPUT_KEYBOARD_KEYSYM_COUNT]; char trace_ev[2048];
  if (argc < 2) { printf("usage: corpus\n"); return 2; }
  t = slurp(argv[1]); if (!t) { printf("FAIL corpus\n"); return 1; }
  snprintf(buf, sizeof buf, "%s\n[keyboard.base]\nSPACE = action:player.jump\nENTER = action:menu.confirm\nS+LCTRL = action:fp2.pause\n[keyboard.override.menu]\nESCAPE = action:menu.cancel\nSPACE = action:menu.confirm\n", t);
  CHECK(nxinput_gptk4_parse(buf, strlen(buf), &c, &g4, &e) == 0 && g4.keybinds == 5, "owner with [keyboard.base] + [keyboard.override.menu] parses");
  nxinput_router_init(&r, sink, NULL, 0xFEEDu); nxinput_router_lease_acquire(&r, 1);
  CHECK(nxinput_keyboard_init(&kb, &g4, &r, 0xFEEDu) == -1, "MUTANT killed: keyboard source claiming the router's own identity (refused)");
  CHECK(nxinput_keyboard_init(&kb, &g4, &r, 0x0Bu) == 0, "keyboard source bound to the shared router");
  /* event backend */
  trace[0] = 0;
  CHECK(nxinput_keyboard_event(&kb, "SPACE", 1) == 1 && !strcmp(trace, "jump+ "), "SPACE down -> player.jump press");
  CHECK(nxinput_keyboard_event(&kb, "SPACE", 2) == 0 && kb.repeats_ignored == 1 && !strcmp(trace, "jump+ "), "auto-repeat ignored");
  CHECK(nxinput_keyboard_event(&kb, "SPACE", 1) == 0 && !strcmp(trace, "jump+ "), "second down without up: no double press");
  CHECK(nxinput_keyboard_event(&kb, "SPACE", 0) == 1 && !strcmp(trace, "jump+ jump- "), "SPACE up -> release once");
  /* keyboard + gamepad on one action: refcount */
  { nxinput_route_output jump = {NXINPUT_ROUTE_ENGINE_DIRECT, 0, 0}; jump.code = nxinput_keyboard_action_code("player.jump");
    trace[0] = 0; nxinput_router_press(&r, 0x900, 0x0A, &jump, NULL, 0); /* gamepad source */
    nxinput_keyboard_event(&kb, "SPACE", 1);
    CHECK(!strcmp(trace, "jump+ "), "MUTANT killed: keyboard and gamepad on one action delivering two presses (refcount: one)");
    nxinput_keyboard_event(&kb, "SPACE", 0);
    CHECK(!strcmp(trace, "jump+ "), "keyboard released first: the gamepad still holds it (no release yet)");
    nxinput_router_release(&r, 0x900, &jump, NULL, 0);
    CHECK(!strcmp(trace, "jump+ jump- "), "last holder releases: exactly one release"); }
  /* chord with modifier: releases when the modifier releases */
  trace[0] = 0; nxinput_keyboard_event(&kb, "LCTRL", 1); nxinput_keyboard_event(&kb, "S", 1);
  CHECK(!strcmp(trace, "pause+ ") && kb.held[0].used && !strcmp(kb.held[0].chord, "LCTRL+S"), "LCTRL+S -> fp2.pause (canonical chord)");
  nxinput_keyboard_event(&kb, "LCTRL", 0);
  CHECK(!strcmp(trace, "pause+ pause- "), "MUTANT killed: chord kept after its modifier released (released when LCTRL goes up)");
  nxinput_keyboard_event(&kb, "S", 0);
  CHECK(!strcmp(trace, "pause+ pause- "), "the key's own release afterwards: nothing more");
  trace[0] = 0; nxinput_keyboard_event(&kb, "S", 1); CHECK(kb.refused_unbound == 1 && !strcmp(trace, ""), "S alone: unbound, nothing emitted, counted"); nxinput_keyboard_event(&kb, "S", 0);
  trace[0] = 0; nxinput_keyboard_event(&kb, "S", 1); nxinput_keyboard_event(&kb, "LCTRL", 1);
  CHECK(!strcmp(trace, ""), "modifier AFTER the key never retro-forms the chord"); nxinput_keyboard_event(&kb, "S", 0); nxinput_keyboard_event(&kb, "LCTRL", 0);
  /* overlay context */
  trace[0] = 0; nxinput_keyboard_set_context(&kb, "menu");
  nxinput_keyboard_event(&kb, "SPACE", 1); nxinput_keyboard_event(&kb, "SPACE", 0); nxinput_keyboard_event(&kb, "ESCAPE", 1); nxinput_keyboard_event(&kb, "ESCAPE", 0);
  CHECK(!strcmp(trace, "confirm+ confirm- cancel+ cancel- "), "[keyboard.override.menu]: SPACE -> menu.confirm, ESCAPE -> menu.cancel");
  /* release-all on context change while held */
  trace[0] = 0; nxinput_keyboard_event(&kb, "SPACE", 1); nxinput_keyboard_set_context(&kb, "");
  CHECK(!strcmp(trace, "confirm+ confirm- ") && kb.released_all >= 1, "MUTANT killed: context change with a key held leaving a latch (release-all)");
  nxinput_keyboard_event(&kb, "SPACE", 0);
  /* loop: an action whose @key names the chord that fed it -- refused by the
   * PARSER (NXI4009) and, should a map bypass the parser, by the SOURCE. */
  { char buf2[8192]; nxinput_gptk4 g2; nxinput_keyboard kl; const char *bpos; size_t head;
    bpos = strstr(t, "B = action:fp2.secondary"); head = bpos ? (size_t)(bpos - t) : 0;
    snprintf(buf2, sizeof buf2, "%.*sB = action:player.jump@key:SPACE%s\n[keyboard.base]\nSPACE = action:player.jump\n", (int)head, t, bpos ? bpos + strlen("B = action:fp2.secondary") : "");
    CHECK(bpos && nxinput_gptk4_parse(buf2, strlen(buf2), &c, &g2, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KEYBOARD, "MUTANT killed: (parser) keyboard SPACE -> player.jump -> @key:SPACE refused at parse (NXI4009)");
    /* bypass the parser: take the valid map and forge the loop in memory */
    memcpy(&g2, &g4, sizeof g2);
    snprintf(g2.base.slot[NXINPUT_GPTK4_B].key, sizeof g2.base.slot[NXINPUT_GPTK4_B].key, "%s", "SPACE");
    snprintf(g2.base.slot[NXINPUT_GPTK4_B].action, sizeof g2.base.slot[NXINPUT_GPTK4_B].action, "%s", "player.jump");
    g2.base.slot[NXINPUT_GPTK4_B].kind = NXINPUT_GPTK4_ACTION;
    nxinput_keyboard_init(&kl, &g2, &r, 0x0Bu); trace[0] = 0;
    CHECK(nxinput_keyboard_event(&kl, "SPACE", 1) == 0 && kl.refused_loop == 1 && !strcmp(trace, ""), "MUTANT killed: keyboard -> action -> @key re-entering the same key (loop refused at the source even when the parser was bypassed)"); }
  /* polling backend == event backend */
  nxinput_keyboard_init(&kp, &g4, &r, 0x0Cu);
  trace[0] = 0; memset(snap, 0, sizeof snap);
  snap[nxinput_keyboard_keysym_index("LCTRL")] = 1; snap[nxinput_keyboard_keysym_index("S")] = 1; nxinput_keyboard_poll(&kp, snap, sizeof snap); /* chord in one frame */
  snap[nxinput_keyboard_keysym_index("SPACE")] = 1; nxinput_keyboard_poll(&kp, snap, sizeof snap);                                                /* LCTRL+SPACE: unbound */
  snap[nxinput_keyboard_keysym_index("LCTRL")] = 0; nxinput_keyboard_poll(&kp, snap, sizeof snap);                                                /* chord releases */
  snap[nxinput_keyboard_keysym_index("SPACE")] = 0; nxinput_keyboard_poll(&kp, snap, sizeof snap);
  snap[nxinput_keyboard_keysym_index("SPACE")] = 1; nxinput_keyboard_poll(&kp, snap, sizeof snap);                                                /* plain SPACE: jump */
  snap[nxinput_keyboard_keysym_index("S")] = 0; snap[nxinput_keyboard_keysym_index("SPACE")] = 0; nxinput_keyboard_poll(&kp, snap, sizeof snap);
  snprintf(trace_ev, sizeof trace_ev, "%s", trace);
  trace[0] = 0; nxinput_keyboard_init(&kb, &g4, &r, 0x0Bu);
  nxinput_keyboard_event(&kb, "LCTRL", 1); nxinput_keyboard_event(&kb, "S", 1); nxinput_keyboard_event(&kb, "SPACE", 1); nxinput_keyboard_event(&kb, "LCTRL", 0); nxinput_keyboard_event(&kb, "SPACE", 0); nxinput_keyboard_event(&kb, "SPACE", 1); nxinput_keyboard_event(&kb, "S", 0); nxinput_keyboard_event(&kb, "SPACE", 0);
  printf("     poll=[%s] event=[%s]\n", trace_ev, trace);
  CHECK(!strcmp(trace, trace_ev) && !strcmp(trace, "pause+ pause- jump+ jump- ") && kp.refused_unbound == 1 && kb.refused_unbound == 1, "polling backend and event backend produce the SAME router sequence (LCTRL+SPACE unbound in both; plain SPACE = jump)");
  CHECK(nxinput_keyboard_poll(&kp, snap, sizeof snap) == 0, "idle poll: no edges");
  /* evdev -> keysym */
  CHECK(!strcmp(nxinput_keyboard_keysym_of_evdev(KEY_SPACE), "SPACE") && !strcmp(nxinput_keyboard_keysym_of_evdev(KEY_LEFTCTRL), "LCTRL") && nxinput_keyboard_keysym_of_evdev(KEY_VOLUMEUP) == NULL, "evdev KEY_* -> schema-4 keysym; keys outside the allow-list have no name");
  CHECK(nxinput_keyboard_event(&kb, "VOLUMEUP", 1) == -1 && nxinput_keyboard_event(&kb, "SPACE", 5) == -1, "unknown keysym / value refused");
  /* E9a: prompts for keyboard bindings are key tokens (never digits), gamepad tokens positional */
  { nxinput_registry reg; nxinput_prompt pr; nxinput_registry_bind(&reg, &g4, 1, 1, 0);
    nxinput_registry_observe_input(&reg, NXINPUT_MODALITY_KEYBOARD, 1); nxinput_registry_observe_input(&reg, NXINPUT_MODALITY_KEYBOARD, 2);
    nxinput_registry_prompt(&reg, "player.jump", "", 0, &pr);
    CHECK(!strcmp(pr.token, "key.SPACE") && !strcmp(pr.text, "SPACE") && !nxinput_registry_text_has_raw_ordinal(pr.text), "keyboard modality: prompt = key.SPACE -> \"SPACE\"");
    nxinput_registry_prompt(&reg, "fp2.pause", "", 0, &pr);
    CHECK(!strcmp(pr.token, "key.LCTRL+S"), "chord prompt token key.LCTRL+S");
    nxinput_registry_observe_input(&reg, NXINPUT_MODALITY_GAMEPAD, 3); nxinput_registry_observe_input(&reg, NXINPUT_MODALITY_GAMEPAD, 4);
    nxinput_registry_prompt(&reg, "fp2.pause", "", 0, &pr);
    CHECK(!strcmp(pr.token, "start") && !strcmp(pr.text, "Start"), "gamepad modality: the same action prompts the positional Xbox token"); }
  free(t);
  printf(fails ? "v5-keyboard: FAIL\n" : "v5-keyboard: OK\n"); return fails ? 1 : 0;
}
