/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / D2-D3: whole vector from ONE instance; overflow visible. */
#include "../../engine-glue/nxinput_padset.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static int n_joy = 2;
static int16_t axes[8][2];   /* per pad: x, y */
static int fake_num(void) { return n_joy; }
static int32_t fake_inst(int i) { return 100 + i; }
static int fake_isgc(int i) { (void)i; return 1; }
static void *fake_open(int i) { return (void *)(long)(1 + i); }
static void fake_close(void *c) { (void)c; }
static void *fake_joy(void *c) { return c; }
static int32_t fake_jinst(void *j) { return 100 + (int32_t)(long)j - 1; }
static void fake_update(void) {}
static uint8_t fake_button(void *c, int b) { (void)c; (void)b; return 0; }
static int16_t fake_axis(void *c, int a) { int p = (int)(long)c - 1; return a == 0 ? axes[p][0] : a == 1 ? axes[p][1] : 0; }
static int logs; static void logf_(const char *l, void *u) { (void)u; printf("     log: %s\n", l); logs++; }
static uint64_t now_ns = 1000000000ull; static uint64_t clk(void) { return now_ns; }
static uint8_t btn[8][21];
static uint8_t fake_button2(void *c, int b) { int p = (int)(long)c - 1; return btn[p][b]; }
static int race_on; static int32_t fake_jinst_race(void *j) { int p = (int)(long)j - 1; return race_on && p == 1 ? 555 : 100 + p; }
int main(void) {
  nxinput_padset_sdl sdl = {fake_num, fake_inst, fake_isgc, fake_open, fake_close, fake_joy, fake_jinst, fake_update, fake_button, fake_axis};
  nxinput_padset set; int16_t x, y;
  CHECK(nxinput_padset_init(&set, &sdl, logf_, NULL) == 0, "init");
  CHECK(nxinput_padset_open_all(&set, NULL, NULL, NULL) == 2, "two pads open");
  axes[0][0] = 0; axes[0][1] = 0; axes[1][0] = 20000; axes[1][1] = -300;
  /* Legacy union (documented defect, kept for regression): X and Y come
   * from pad 1 although player 1 (pad 0) is at rest. */
  CHECK(nxinput_padset_axis(&set, 0) == 20000, "legacy per-axis union takes the other pad's X (negative path 9bf3b3f)");
  /* V5: the vector is whole and from ONE pad. 0.11.6: without an explicit
   * primary that pad is the most deflected one (a two-node pad or a clone
   * admitted after the real pad is never slot 0; "first admitted" left its
   * stick dead on the device, FP2 03/09). */
  CHECK(nxinput_padset_vector(&set, 0, 1, &x, &y) == 1 && x == 0 && y == 0, "before any sample the first admitted pad is primary: exactly (0,0)");
  nxinput_padset_sample(&set);
  CHECK(nxinput_padset_vector(&set, 0, 1, &x, &y) == 1 && x == 20000 && y == -300 && nxinput_padset_primary_instance(&set) == 101, "MUTANT killed: whole vector of the pad the user is moving (slot 1) ignored because slot 0 was 'primary' (election by activity)");
  axes[0][0] = 12000; axes[1][1] = 32000; nxinput_padset_sample(&set);
  nxinput_padset_vector(&set, 0, 1, &x, &y);
  CHECK(x == 20000 && y == 32000, "V5 never mixes X of pad 0 with Y of pad 1: the active primary keeps its seat (hysteresis), whole vector");
  axes[1][0] = 0; axes[1][1] = 0; nxinput_padset_sample(&set);
  nxinput_padset_vector(&set, 0, 1, &x, &y);
  CHECK(x == 12000 && y == 0 && nxinput_padset_primary_instance(&set) == 100, "pad 1 at rest and pad 0 active: the seat moves to pad 0 (never a union of axes)");
  axes[1][0] = 20000; axes[1][1] = 32000; axes[0][0] = 5000;
  CHECK(nxinput_padset_set_primary(&set, 101) == 0, "primary can be set explicitly");
  nxinput_padset_vector(&set, 0, 1, &x, &y);
  CHECK(x == 20000 && y == 32000, "explicit primary reads its own whole vector");
  CHECK(nxinput_padset_set_primary(&set, 999) == -1, "unknown instance refused as primary");
  nxinput_padset_remove_instance(&set, 101);
  CHECK(nxinput_padset_primary_instance(&set) == 100, "removed primary falls back to first admitted");
  /* D3: overflow is counted and logged once. */
  nxinput_padset_close_all(&set);
  n_joy = 6;
  CHECK(nxinput_padset_open_all(&set, NULL, NULL, NULL) == NXINPUT_PADSET_MAX, "opens up to the limit");
  CHECK(nxinput_padset_overflowed(&set) == 2 && logs == 1, "two extra pads refused, counted, logged once");
  /* ---------------- 0.11.1: pre-router wired (D5/D7), instance race (C5), calibrated vector (5.5) */
  { nxinput_padset_sdl sdl2 = {fake_num, fake_inst, fake_isgc, fake_open, fake_close, fake_joy, fake_jinst, fake_update, fake_button2, fake_axis};
    nxinput_padset P; int sel, sta; float fx, fy;
    n_joy = 2; memset(btn, 0, sizeof btn);
    nxinput_padset_init(&P, &sdl2, logf_, NULL); nxinput_padset_set_clock(&P, clk);
    CHECK(nxinput_padset_open_all(&P, NULL, NULL, NULL) == 2, "two pads open");
    btn[0][NXINPUT_PADSET_BUTTON_START] = 1; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "MUTANT killed: START leaking to the game before the pre-router window (retained)");
    now_ns += 100000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "still inside the 180 ms window: retained");
    now_ns += 100000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 1 && nxinput_padset_exit_requested(&P) == 0, "window over: START forwarded to the union, no exit");
    btn[0][NXINPUT_PADSET_BUTTON_START] = 0; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "release forwarded");
    /* 0.11.5 (field, FP2 03/09): a 150 ms START tap -- shorter than the
     * 180 ms window -- must reach the union in at least ONE sample. */
    btn[0][NXINPUT_PADSET_BUTTON_START] = 1; now_ns += 20000000ull; nxinput_padset_sample(&P);
    btn[0][NXINPUT_PADSET_BUTTON_START] = 0; now_ns += 150000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "tap released inside the window: still retained");
    now_ns += 60000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 1, "MUTANT killed: a tap shorter than the window never reached the game (press and release flushed in one sample)");
    now_ns += 16000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0 && nxinput_padset_exit_requested(&P) == 0, "the deferred release lands on the next sample; no exit, no latch");
    /* 0.11.6 (field, FP2 03/09): a DOUBLE tap inside the window. The first
     * tap's press is flushed by the second press EVENT and the padset ticks
     * in that same sample: the union must still show the button down there
     * (before: press+release in one sample, first tap invisible). */
    btn[0][NXINPUT_PADSET_BUTTON_START] = 1; now_ns += 50000000ull; nxinput_padset_sample(&P);
    btn[0][NXINPUT_PADSET_BUTTON_START] = 0; now_ns += 50000000ull; nxinput_padset_sample(&P);
    btn[0][NXINPUT_PADSET_BUTTON_START] = 1; now_ns += 50000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 1, "MUTANT killed: first tap of a double tap never reached the union (its release paid in the same sample as its press)");
    now_ns += 16000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "first tap's release on the next sample; second press still retained");
    btn[0][NXINPUT_PADSET_BUTTON_START] = 0; now_ns += 50000000ull; nxinput_padset_sample(&P);
    now_ns += 200000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 1 && nxinput_padset_exit_requested(&P) == 0, "second tap forwarded after its window: down in one sample, no exit");
    now_ns += 16000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "second tap released on the next sample; nothing latched");
    /* chord: SELECT then START inside the window, same instance */
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 1; nxinput_padset_sample(&P); now_ns += 50000000ull;
    btn[0][NXINPUT_PADSET_BUTTON_START] = 1; nxinput_padset_sample(&P);
    nxinput_padset_chord_inputs(&P, &sel, &sta);
    CHECK(nxinput_padset_exit_requested(&P) == 1 && P.exits == 1 && sel == 1 && sta == 1 && P.buttons[NXINPUT_PADSET_BUTTON_BACK] == 0 && P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "SELECT+START same instance inside the window: one exit, chord inputs armed, both edges consumed (never in the union)");
    now_ns += 400000000ull; nxinput_padset_sample(&P); nxinput_padset_chord_inputs(&P, &sel, &sta);
    CHECK(sel == 1 && sta == 1 && P.exits == 1 && P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "held chord: inputs stay armed for the exit-chord hold polls, no second exit, still consumed");
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 0; btn[0][NXINPUT_PADSET_BUTTON_START] = 0; nxinput_padset_sample(&P);
    CHECK(nxinput_padset_exit_requested(&P) == 0 && P.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "released: request cleared, nothing leaked");
    /* MUTANT: SELECT tap then START press = no chord (two presses) */
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 1; nxinput_padset_sample(&P); now_ns += 30000000ull;
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 0; nxinput_padset_sample(&P); now_ns += 30000000ull;
    btn[0][NXINPUT_PADSET_BUTTON_START] = 1; nxinput_padset_sample(&P);
    CHECK(nxinput_padset_exit_requested(&P) == 0 && P.exits == 1, "MUTANT killed: SELECT tap + START press exiting the game (no chord)");
    now_ns += 300000000ull; nxinput_padset_sample(&P);
    CHECK(P.buttons[NXINPUT_PADSET_BUTTON_START] == 1, "the START press reaches the game after the window");
    btn[0][NXINPUT_PADSET_BUTTON_START] = 0; nxinput_padset_sample(&P);
    /* cross-pad never chords */
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 1; btn[1][NXINPUT_PADSET_BUTTON_START] = 1; nxinput_padset_sample(&P);
    CHECK(nxinput_padset_exit_requested(&P) == 0 && P.chord_cross_pad == 1, "cross-pad SELECT/START: no exit, denial evidence");
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 0; btn[1][NXINPUT_PADSET_BUTTON_START] = 0; now_ns += 300000000ull; nxinput_padset_sample(&P);
    /* hotplug: reconnect of the same instance number gets a NEW generation; a retained SELECT of the old one cannot chord */
    btn[0][NXINPUT_PADSET_BUTTON_BACK] = 1; nxinput_padset_sample(&P);
    { uint32_t g_old = P.generations[0];
      nxinput_padset_remove_instance(&P, 100); btn[0][NXINPUT_PADSET_BUTTON_BACK] = 0;
      nxinput_padset_open_all(&P, NULL, NULL, NULL);
      CHECK(P.count == 2 && P.generations[1] != g_old, "reconnect reusing the instance id carries a new device_instance_generation");
      btn[0][NXINPUT_PADSET_BUTTON_START] = 1; nxinput_padset_sample(&P);
      CHECK(nxinput_padset_exit_requested(&P) == 0, "MUTANT killed: SELECT of the previous generation completing a chord after reconnect");
      btn[0][NXINPUT_PADSET_BUTTON_START] = 0; now_ns += 300000000ull; nxinput_padset_sample(&P); }
    /* C5: the index re-used by another device between admission and open */
    nxinput_padset_close_all(&P); race_on = 1;
    { nxinput_padset_sdl sdl3 = sdl2; sdl3.joystick_instance = fake_jinst_race;
      nxinput_padset_init(&P, &sdl3, logf_, NULL); nxinput_padset_set_clock(&P, clk);
      CHECK(nxinput_padset_open_all(&P, NULL, NULL, NULL) == 1 && P.count == 1 && nxinput_padset_instance_races(&P) == 1, "MUTANT killed: opened pad is not the admitted instance (closed, counted, not adopted)");
      race_on = 0; nxinput_padset_close_all(&P); }
    /* 5.5: calibrated vector with an exact zero and a radial deadzone */
    nxinput_padset_init(&P, &sdl2, logf_, NULL); nxinput_padset_set_clock(&P, clk); nxinput_padset_open_all(&P, NULL, NULL, NULL);
    axes[0][0] = 0; axes[0][1] = 0;
    CHECK(nxinput_padset_vector_norm(&P, 0, 1, 0.0f, &fx, &fy) == 1 && fx == 0.0f && fy == 0.0f, "rest -> exact 0.0/0.0");
    axes[0][0] = 257; axes[0][1] = 0; /* the 0..255 pad's +1 residual as SDL scales it */
    CHECK(nxinput_padset_vector_norm(&P, 0, 1, 0.1f, &fx, &fy) == 1 && fx == 0.0f && fy == 0.0f, "MUTANT killed: the 0..255 residual (+257 raw) surviving the deadzone (exact 0.0 after radial 0.1)");
    axes[0][0] = 16384; axes[0][1] = -32768;
    CHECK(nxinput_padset_vector_norm(&P, 0, 1, 0.0f, &fx, &fy) == 1 && fx > 0.49f && fx < 0.51f && fy == -1.0f, "half-right / full-up normalized once ([-1,1], no double normalization)");
    /* 0.11.4 (review 2, P2): trigger of the PRIMARY pad only, [0,1], exact 0 at rest */
    { float tv = 9.0f; axes[0][0] = 0; axes[1][0] = 32767;
      CHECK(nxinput_padset_trigger_norm(&P, 0, &tv) == 1 && tv == 0.0f, "MUTANT killed: trigger unioned across pads (the second pad's full pull leaked into the primary's rest)");
      axes[0][0] = 32767; CHECK(nxinput_padset_trigger_norm(&P, 0, &tv) == 1 && tv > 0.999f && tv <= 1.0f, "primary full pull -> 1.0");
      axes[0][0] = 16384; CHECK(nxinput_padset_trigger_norm(&P, 0, &tv) == 1 && tv > 0.49f && tv < 0.51f, "half pull -> ~0.5 ([0,1], not [-1,1])");
      axes[0][0] = -5; CHECK(nxinput_padset_trigger_norm(&P, 0, &tv) == 1 && tv == 0.0f, "negative raw (below rest) clamps to exact 0"); axes[0][0] = 0; axes[1][0] = 0; }
    CHECK(nxinput_padset_vector_norm(&P, 0, 1, 0.95f, &fx, &fy) == -1 && nxinput_padset_vector_norm(&P, 0, 1, -0.1f, &fx, &fy) == -1, "invalid deadzone refused");
    nxinput_padset_close_all(&P); }
  printf(fails ? "v5-padset: FAIL\n" : "v5-padset: OK\n");
  return fails ? 1 : 0;
}
