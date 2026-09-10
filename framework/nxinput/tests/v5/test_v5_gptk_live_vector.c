#define _GNU_SOURCE 1
/* SPDX-License-Identifier: GPL-3.0-only */
/* V5-M1a item 2: the vector gesture EDGE with a NEUTRAL FLOOR in the
 * framework (nxinput_gptk_live). RED reproduced first: with the floor at 0
 * (the FP2/Blossom `!= 0.0f` and the pre-1.2.8 Nameless Cat), a pad whose
 * rest normalizes to +0.0039 (0..255 range, centre 127.5) opens the gesture
 * at boot and never closes it. GREEN: the default floor (1/64) keeps it
 * neutral; a real deflection opens the edge, the return closes it;
 * release-all closes open gestures and reports them; the adapter may raise
 * the floor to its own deadzone (cursor), never above 0.9. */
#include "../../include/nxinput_gptk4_bridge.h"
#include "../../include/nxinput_gptk_live.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu", "pause", "cursor"};
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b; }
static unsigned deliveries; static float last_x, last_y;
static int vsink(void *u, const char *a, float x, float y) { (void)u; (void)a; deliveries++; last_x = x; last_y = y; return 0; }
static int ssink(void *u, const char *a, int p, float v) { (void)u; (void)a; (void)p; (void)v; return 0; }
static nxinput_gptk4 g4; static nxinput_gptk map; static nxinput_gptk_live live;
static const float REST_255 = 0.00392f; /* (128 - 127.5) / 127.5 : the 0..255 pad at rest after asymmetric normalization */
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0};
  nxinput_gptk4_bridge_receipt r; nxinput_gptk4_error e; char *t; char err[128]; size_t i;
  if (argc < 2) { printf("usage: corpus\n"); return 2; }
  t = slurp(argv[1]); if (!t) { printf("FAIL corpus\n"); return 1; }
  CHECK(nxinput_gptk4_parse(t, strlen(t), &c, &g4, &e) == 0 && nxinput_gptk4_project(&g4, &map, &r) == 0, "corpus projected (stick.left vector -> fp2.move)");
  nxinput_gptk_live_init(&live, &map);
  CHECK(nxinput_gptk_live_vector_neutral_floor(&live, NXINPUT_GPTK_LEFT_STICK) == NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_DEFAULT && nxinput_gptk_live_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK) == NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_DEFAULT, "init: the universal default floor (1/64) on both sticks");
  CHECK(nxinput_gptk_live_vector_neutral_floor(&live, NXINPUT_GPTK_A) == 0.0f && nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_A, 0.2f) == -1, "a non-stick control has no floor and refuses one");
  for (i = 0; i < sizeof acts / sizeof acts[0]; i++) {
    if (acts[i].value_kind == 2) nxinput_gptk_live_register_vector(&live, acts[i].id, vsink, NULL); else nxinput_gptk_live_register(&live, acts[i].id, ssink, NULL);
  }
  CHECK(nxinput_gptk_live_seal(&live, err, sizeof err) == 0 && nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_GAMEPLAY, "scene:gameplay") == 0, "sealed + gameplay context proven");
  /* --- MUTANT: floor 0 (`!= 0.0f`) with the 0..255 rest: the gesture opens at boot */
  nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_LEFT_STICK, 0.0f);
  CHECK(nxinput_gptk_live_vector_neutral_floor(&live, NXINPUT_GPTK_LEFT_STICK) == 0.0f, "floor forced to 0 (the mutant)");
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, REST_255, REST_255) == NXINPUT_GPTK_LIVE_DELIVERED && nxinput_gptk_live_last_vector_edge(&live) == NXINPUT_GPTK_VECTOR_EDGE_OPENED && nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_LEFT_STICK), "MUTANT killed: floor 0 + centre +0.0039 reopens the bug (gesture OPENED at rest -- the RED of NC 1.2.7 / FP2 / Blossom)");
  nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, REST_255, REST_255);
  CHECK(nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_LEFT_STICK) && nxinput_gptk_live_last_vector_edge(&live) == NXINPUT_GPTK_VECTOR_EDGE_NONE, "and it never closes while the pad rests");
  /* --- GREEN: the default floor */
  nxinput_gptk_live_clear_context(&live);
  CHECK(nxinput_gptk_live_vector_released_mask(&live) == (1u << NXINPUT_GPTK_LEFT_STICK) && !nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_LEFT_STICK), "release-all closes the open gesture and reports it in the mask");
  nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_GAMEPLAY, "scene:gameplay");
  nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_LEFT_STICK, NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_DEFAULT);
  deliveries = 0;
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, REST_255, REST_255) == NXINPUT_GPTK_LIVE_DELIVERED && deliveries == 1 && nxinput_gptk_live_last_vector_edge(&live) == NXINPUT_GPTK_VECTOR_EDGE_NONE && !nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_LEFT_STICK), "default floor: the resting 0..255 pad is delivered (the sink sees the vector) but NO gesture opens");
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, 0.000015f, 0.0f) == NXINPUT_GPTK_LIVE_DELIVERED && !nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_LEFT_STICK), "the 0..65535 residual (+1.5e-5) is neutral too");
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, 0.4f, -0.2f) == NXINPUT_GPTK_LIVE_DELIVERED && nxinput_gptk_live_last_vector_edge(&live) == NXINPUT_GPTK_VECTOR_EDGE_OPENED && live.vector_gestures_opened == 2, "a real deflection OPENS the gesture (edge once)");
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, 0.5f, 0.1f) == NXINPUT_GPTK_LIVE_DELIVERED && nxinput_gptk_live_last_vector_edge(&live) == NXINPUT_GPTK_VECTOR_EDGE_NONE, "while deflected: no new edge (one gesture)");
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_LEFT_STICK, REST_255, 0.0f) == NXINPUT_GPTK_LIVE_DELIVERED && nxinput_gptk_live_last_vector_edge(&live) == NXINPUT_GPTK_VECTOR_EDGE_CLOSED && !nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_LEFT_STICK), "return inside the floor CLOSES the gesture (edge once)");
  /* boundary: exactly the floor radius is neutral; just above is not */
  { float f = NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_DEFAULT;
    CHECK(nxinput_gptk_live_vector_is_neutral(&live, NXINPUT_GPTK_LEFT_STICK, f, 0.0f) == 1 && nxinput_gptk_live_vector_is_neutral(&live, NXINPUT_GPTK_LEFT_STICK, f * 1.01f, 0.0f) == 0, "radius compare: <= floor neutral, > floor not"); }
  /* adapter raises the floor to its cursor deadzone (the NC 1.2.8 case) */
  CHECK(nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK, 0.25f) == 0 && nxinput_gptk_live_vector_is_neutral(&live, NXINPUT_GPTK_RIGHT_STICK, 0.2f, 0.1f) == 1, "per-control floor raised to the cursor deadzone: 0.22 is neutral for the right stick");
  CHECK(nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK, 5.0f) == 0 && nxinput_gptk_live_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK) == NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_MAX, "floor clamped to 0.9");
  CHECK(nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK, -1.0f) == 0 && nxinput_gptk_live_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK) == 0.0f, "negative floor -> 0");
  /* a control the map suppresses (null) closes an open gesture on the evidence */
  nxinput_gptk_live_set_vector_neutral_floor(&live, NXINPUT_GPTK_RIGHT_STICK, NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_DEFAULT);
  { nxinput_gptk_live_result rr = nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_RIGHT_STICK, 0.6f, 0.0f);
    CHECK(rr != NXINPUT_GPTK_LIVE_FATAL && !nxinput_gptk_live_vector_active(&live, NXINPUT_GPTK_RIGHT_STICK), "a stick the map does not route (native/null) never keeps a gesture open"); }
  free(t);
  printf(fails ? "v5-gptk-live-vector: FAIL\n" : "v5-gptk-live-vector: OK\n"); return fails ? 1 : 0;
}
