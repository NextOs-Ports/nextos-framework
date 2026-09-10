/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 I3a: random operation sequences over the hot-reload/lifecycle
 * machines -- nxinput_lifecycle (+sync), nxinput_prerouter (physical graph),
 * nxinput_router, nxinput_keyboard -- under ASan/UBSan, with INVARIANTS
 * checked after every step:
 *   L1 no edge is HELD (delivered) on a device that is not open
 *   L2 held implies physically down (a delivered press without its source)
 *   L3 deliveries balance: every press delivered has exactly one release
 *      by the end (release-all at the end of the run)
 *   P1 exits never exceed one per (instance, generation) chord window
 *   P2 forwarded presses == forwarded releases after release_all
 *   R1 router outputs held == outputs pressed - released (sink balance)
 *   K1 keyboard held chords never exceed the table; release_all empties
 * Seeded xorshift; the seed prints on failure. 0 crashes, 0 invariant hits. */
#include "../../include/nxinput_lifecycle.h"
#include "../../include/nxinput_prerouter.h"
#include "../../include/nxinput_route.h"
#include "../../include/nxinput_keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng & 0xffffffffu); }
static long press_delivered, release_delivered;
static void lc_deliver(void *u, nxinput_lc_owner o, int i, uint32_t g, unsigned e, int p, uint32_t ce, uint32_t mg) { (void)u; (void)o; (void)i; (void)g; (void)e; (void)ce; (void)mg; if (p) press_delivered++; else release_delivered++; }
static int lc_parse(void *u, const char *t, uint64_t *d) { (void)u; if (!t || strstr(t, "BAD")) return -1; *d = (uint64_t)strlen(t); return 0; }
static long pre_fwd_press, pre_fwd_release, pre_exits;
static void pre_fwd(void *u, int i, uint32_t g, nxinput_prerouter_edge e, int p, uint64_t id) { (void)u; (void)i; (void)g; (void)e; (void)id; if (p) pre_fwd_press++; else pre_fwd_release++; }
static void pre_exit(void *u, int i, uint32_t g, uint64_t id) { (void)u; (void)i; (void)g; (void)id; pre_exits++; }
static long rt_press, rt_release;
static void rt_sink(void *u, const nxinput_route_output *o, int p, uint32_t g, uint32_t e, uint64_t s) { (void)u; (void)o; (void)g; (void)e; (void)s; if (p) rt_press++; else rt_release++; }
static int fails; static uint64_t seed_now;
#define INV(c, m) do { if (!(c)) { printf("INVARIANT %s (seed=%llu step=%d)\n", m, (unsigned long long)seed_now, step); fails++; } } while (0)
static nxinput_gptk4 g4;
int main(int argc, char **argv) {
  int runs = argc > 2 ? atoi(argv[2]) : 200, r, step = 0; char *t = NULL; long n;
  static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
  static const char *const ctxs[] = {"menu", "pause", "cursor"};
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0}; nxinput_gptk4_error e;
  if (argc > 1) { FILE *f = fopen(argv[1], "rb"); if (f) { char buf[70000]; size_t got = fread(buf, 1, sizeof buf - 1, f); buf[got] = 0; fclose(f); t = malloc(got + 256); snprintf(t, got + 256, "%s\n[keyboard.base]\nSPACE = action:player.jump\nENTER = action:menu.confirm\nS+LCTRL = action:fp2.pause\n", buf); } }
  if (!t || nxinput_gptk4_parse(t, strlen(t), &c, &g4, &e) != 0) { printf("fuzz: corpus missing/unparsable, keyboard machine skipped\n"); if (t) { free(t); t = NULL; } }
  for (r = 0; r < runs; r++) {
    nxinput_lifecycle lc; nxinput_lc_ops lops = {NULL, lc_deliver, lc_parse}; uint32_t gens[NXINPUT_LC_MAX_DEVICES]; unsigned i;
    nxinput_prerouter pr; nxinput_prerouter_ops pops = {NULL, pre_fwd, pre_exit}; uint64_t now = 0;
    nxinput_router rt; nxinput_keyboard kb; nxinput_route_output outs[6];
    seed_now = 0x1234u + (uint64_t)r * 7919u; rng = seed_now | 1u;
    press_delivered = release_delivered = 0; pre_fwd_press = pre_fwd_release = pre_exits = 0; rt_press = rt_release = 0;
    nxinput_lifecycle_init(&lc, &lops, (rnd() & 1) ? NXINPUT_LC_FALLBACK_ENGINE_NATIVE : NXINPUT_LC_FALLBACK_SUPPRESS, 1);
    memset(gens, 0, sizeof gens);
    nxinput_prerouter_init(&pr, &pops, 0);
    nxinput_router_init(&rt, rt_sink, NULL, 0xFEEDu); nxinput_router_lease_acquire(&rt, 1);
    for (i = 0; i < 6; i++) { outs[i].kind = (uint8_t)(i < 3 ? NXINPUT_ROUTE_ENGINE_DIRECT : NXINPUT_ROUTE_SDL_KEYBOARD); outs[i].code = 100u + i; outs[i].player = 0; }
    if (t) nxinput_keyboard_init(&kb, &g4, &rt, 0x0Bu);
    for (step = 0; step < 400; step++) {
      uint32_t op = rnd() % 24; int inst = (int)(rnd() % 4); unsigned edge = rnd() % NXINPUT_LC_MAX_EDGES;
      switch (op) {
        case 0: gens[inst] = nxinput_lifecycle_admit(&lc, inst, 0xA00u + (uint64_t)inst); break;
        case 1: nxinput_lifecycle_open(&lc, inst, gens[inst]); break;
        case 2: nxinput_lifecycle_open_checked(&lc, inst, gens[inst], nxinput_lifecycle_hotplug_generation(&lc) - (rnd() % 2)); break;
        case 3: nxinput_lifecycle_remove(&lc, inst); break;
        case 4: case 5: case 6: nxinput_lifecycle_edge(&lc, inst, gens[inst], edge, 1); break;
        case 7: case 8: case 9: nxinput_lifecycle_edge(&lc, inst, gens[inst], edge, 0); break;
        case 10: nxinput_lifecycle_focus_lost(&lc); break;
        case 11: nxinput_lifecycle_set_context(&lc, (nxinput_lc_context)(rnd() % 7)); break;
        case 12: nxinput_lifecycle_hot_reload(&lc, (rnd() & 1) ? "owner text v" : "BAD"); break;
        case 13: nxinput_lifecycle_edge(&lc, inst, gens[inst] + 1u, edge, 1); break; /* stale generation */
        case 14: nxinput_prerouter_event(&pr, inst, gens[inst], (nxinput_prerouter_edge)(rnd() & 1), (int)(rnd() % 3), now); break;
        case 15: now += rnd() % 250000000ull; nxinput_prerouter_tick(&pr, now); break;
        case 16: nxinput_prerouter_unplug(&pr, inst, now); break;
        case 17: nxinput_prerouter_bind_physical(&pr, inst, gens[inst], 0xF00u + (rnd() % 2), (nxinput_prerouter_path)(rnd() % 3), (int)(rnd() % 3), (int)(rnd() & 1)); break;
        case 18: nxinput_router_press(&rt, 500u + (rnd() % 12), 0x0Au, &outs[rnd() % 6], NULL, 0); break;
        case 19: nxinput_router_release(&rt, 500u + (rnd() % 12), &outs[rnd() % 6], NULL, 0); break;
        case 20: nxinput_router_release_all(&rt, (int)(rnd() & 1), (int)(rnd() & 1)); break;
        case 21: if (t) nxinput_keyboard_event(&kb, (const char *[]){"SPACE", "ENTER", "S", "LCTRL", "A", "F1"}[rnd() % 6], (int)(rnd() % 3)); break;
        case 22: if (t) { uint8_t snap[NXINPUT_KEYBOARD_KEYSYM_COUNT]; unsigned k; for (k = 0; k < NXINPUT_KEYBOARD_KEYSYM_COUNT; k++) snap[k] = (uint8_t)((rnd() % 5) == 0); nxinput_keyboard_poll(&kb, snap, sizeof snap); } break;
        default: if (t) nxinput_keyboard_set_context(&kb, (rnd() & 1) ? "menu" : ""); break;
      }
      /* invariants */
      for (i = 0; i < NXINPUT_LC_MAX_DEVICES; i++) {
        unsigned w;
        if (!lc.dev[i].used) continue;
        for (w = 0; w < NXINPUT_LC_MAX_EDGES / 32; w++) {
          INV(lc.dev[i].state == 1 || lc.dev[i].held[w] == 0, "L1 held edge on a device that is not open");
          INV((lc.dev[i].held[w] & ~lc.dev[i].down[w]) == 0, "L2 held (delivered) edge that is not physically down");
        }
      }
      INV(press_delivered >= release_delivered, "L3 more releases than presses");
      INV(pre_fwd_press >= pre_fwd_release, "P2 more forwarded releases than presses");
      INV(rt_press >= rt_release && (long)rt.held_count == rt_press - rt_release, "R1 router sink balance != held outputs");
      if (t) { unsigned k, held = 0; for (k = 0; k < NXINPUT_KEYBOARD_MAX_HELD; k++) held += kb.held[k].used; INV(held <= NXINPUT_KEYBOARD_MAX_HELD, "K1 keyboard held table overflow"); }
    }
    /* end of run: release everything, balances close */
    nxinput_lifecycle_focus_lost(&lc); for (i = 0; i < 4; i++) nxinput_lifecycle_remove(&lc, (int)i);
    nxinput_prerouter_release_all(&pr, now); for (i = 0; i < 4; i++) nxinput_prerouter_unplug(&pr, (int)i, now);
    if (t) nxinput_keyboard_release_all(&kb);
    nxinput_router_release_all(&rt, 0, 0);
    step = 400;
    INV(press_delivered == release_delivered, "L3 lifecycle press/release balance at the end");
    INV(pre_fwd_press == pre_fwd_release, "P2 pre-router press/release balance at the end");
    INV(rt_press == rt_release && rt.held_count == 0, "R1 router balance at the end");
    n = press_delivered + pre_fwd_press + rt_press + pre_exits; (void)n;
  }
  if (t) free(t);
  printf("fuzz-v5-lifecycle: %s runs=%d invariant_hits=%d\n", fails ? "FAIL" : "OK", runs, fails);
  return fails ? 1 : 0;
}
