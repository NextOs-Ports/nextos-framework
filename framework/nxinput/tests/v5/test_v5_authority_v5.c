#define _GNU_SOURCE 1
/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 E3: nextos / engine / synchronized as ONE executable authority.
 * Mutants: SYNCHRONIZED elected without engine hooks (silent NEXTOS);
 * ENGINE mode presenting an editable owner GPTK; native edge inside NEXTOS
 * creating two authorities; stale generation resolving; chord hold not
 * suppressing. */
#include "../../include/nxinput_authority_v5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu", "pause", "cursor"};
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b; }
static char *with(const char *t, const char *a, const char *b) { const char *p = strstr(t, a); size_t la = strlen(a), lb = strlen(b); char *o; if (!p) return NULL; o = malloc(strlen(t) - la + lb + 1); memcpy(o, t, (size_t)(p - t)); memcpy(o + (p - t), b, lb); strcpy(o + (p - t) + lb, p + la); return o; }
static int e_apply(void *u, uint64_t d) { (void)u; (void)d; return 0; }
static int e_read(void *u, uint64_t *d) { (void)u; *d = 0; return 0; }
static int e_roll(void *u, uint64_t d) { (void)u; (void)d; return 0; }
static int o_cas(void *u, uint64_t e, uint64_t n) { (void)u; (void)e; (void)n; return 0; }
static nxinput_gptk4 g4;
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0};
  nxinput_gptk4_error e; nxinput_authority_v5 a; char *t, *m; nxinput_sync_ops ops = {NULL, e_apply, e_read, e_roll, o_cas}, none = {NULL, NULL, NULL, NULL, NULL};
  if (argc < 2) { printf("usage: corpus\n"); return 2; }
  t = slurp(argv[1]); if (!t) { printf("FAIL corpus\n"); return 1; }
  CHECK(nxinput_gptk4_parse(t, strlen(t), &c, &g4, &e) == 0 && g4.authority == NXINPUT_GPTK4_AUTH_NEXTOS, "corpus parses with AUTHORITY = nextos");
  memset(&a, 0, sizeof a);
  CHECK(nxinput_authority_v5_edge_owner(&a, 1, NXINPUT_GPTK4_ACTION, 0) == NXINPUT_OWNER_SUPPRESSED, "un-elected: every edge SUPPRESSED (nothing acts before the election)");
  CHECK(nxinput_authority_v5_elect(&a, &g4, 7, NULL) == 0 && a.mode == NXINPUT_AUTHORITY_NEXTOS && nxinput_authority_v5_owner_file(&a) == NXINPUT_OWNER_FILE_EDITABLE, "NEXTOS elected: editable owner file");
  CHECK(nxinput_authority_v5_resolve(&a, 7, "", NXINPUT_GPTK4_A, 0) == NXINPUT_OWNER_AUTHORITY_ACTION, "A = action -> NEXTOS_ACTION");
  CHECK(nxinput_authority_v5_resolve(&a, 7, "", NXINPUT_GPTK4_L2_ANALOG, 0) == NXINPUT_OWNER_ENGINE_NATIVE, "trigger analog = native -> ENGINE_NATIVE for that edge only (no second authority)");
  CHECK(nxinput_authority_v5_resolve(&a, 7, "menu", NXINPUT_GPTK4_X, 0) == NXINPUT_OWNER_SUPPRESSED, "[override.menu] X = null -> SUPPRESSED in menu");
  CHECK(nxinput_authority_v5_resolve(&a, 7, "", NXINPUT_GPTK4_A, 1) == NXINPUT_OWNER_SUPPRESSED && a.edges_suppressed_by_chord == 1, "MUTANT killed: chord hold not suppressing the edge (pre-router outranks the mode)");
  CHECK(nxinput_authority_v5_resolve(&a, 8, "", NXINPUT_GPTK4_A, 0) == NXINPUT_OWNER_SUPPRESSED && a.refused_stale_generation == 1, "MUTANT killed: an edge of another mapping generation resolving (stale => SUPPRESSED, counted)");
  CHECK(!nxinput_authority_v5_registry_from_readback(&a), "NEXTOS: registry fed from the GPTK generation");
  /* ENGINE */
  m = with(t, "AUTHORITY = nextos", "AUTHORITY = engine");
  CHECK(m && nxinput_gptk4_parse(m, strlen(m), &c, &g4, &e) == 0 && nxinput_authority_v5_elect(&a, &g4, 9, NULL) == 0 && a.mode == NXINPUT_AUTHORITY_ENGINE, "ENGINE elected");
  CHECK(nxinput_authority_v5_owner_file(&a) == NXINPUT_OWNER_FILE_MIRROR_READBACK && strstr(a.mirror_name, ".engine-readback.mirror") && nxinput_authority_v5_registry_from_readback(&a), "MUTANT killed: ENGINE mode presenting an editable owner GPTK (only a named readback mirror; registry from readback)");
  CHECK(nxinput_authority_v5_resolve(&a, 9, "", NXINPUT_GPTK4_A, 0) == NXINPUT_OWNER_ENGINE_NATIVE, "ENGINE: an action binding still resolves to the engine (no NextOS delivery)");
  CHECK(nxinput_authority_v5_resolve(&a, 9, "menu", NXINPUT_GPTK4_X, 0) == NXINPUT_OWNER_SUPPRESSED, "ENGINE: null suppresses");
  free(m);
  /* SYNCHRONIZED */
  m = with(t, "AUTHORITY = nextos", "AUTHORITY = synchronized");
  CHECK(m && nxinput_gptk4_parse(m, strlen(m), &c, &g4, &e) == 0, "synchronized corpus parses");
  CHECK(nxinput_authority_v5_elect(&a, &g4, 10, &none) == -1 && !a.elected && a.refused_sync_without_engine == 1 && nxinput_authority_v5_edge_owner(&a, 10, NXINPUT_GPTK4_ACTION, 0) == NXINPUT_OWNER_SUPPRESSED, "MUTANT killed: SYNCHRONIZED elected without engine hooks (refused: no silent NEXTOS, everything SUPPRESSED)");
  CHECK(nxinput_authority_v5_elect(&a, &g4, 10, NULL) == -1, "NULL hooks refused too");
  CHECK(nxinput_authority_v5_elect(&a, &g4, 10, &ops) == 0 && a.mode == NXINPUT_AUTHORITY_SYNCHRONIZED && nxinput_authority_v5_owner_file(&a) == NXINPUT_OWNER_FILE_SYNCHRONIZED && nxinput_authority_v5_resolve(&a, 10, "", NXINPUT_GPTK4_A, 0) == NXINPUT_OWNER_AUTHORITY_ACTION, "SYNCHRONIZED with the four engine hooks: elected, edges resolve like NEXTOS (the CAS keeps both sides equal)");
  free(m);
  CHECK(nxinput_authority_v5_elect(&a, NULL, 11, NULL) == -1 && !a.elected, "NULL map refused, un-elected");
  free(t);
  printf(fails ? "v5-authority-v5: FAIL\n" : "v5-authority-v5: OK\n"); return fails ? 1 : 0;
}
