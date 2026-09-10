/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / B6 (5.2): corpus inventory + precedence, matching=2/domain_lines=3,
 * platform filter, divergent duplicate refused before the store. */
#include "../../include/nxinput_corpus.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
#define G "19000000010000000100000000010000"
static const char *muos = G ",muOS-Keys,a:b3,b:b4,x:b6,y:b5,leftshoulder:b7,rightshoulder:b8,start:b10,back:b9,platform:Linux,";
static const char *modern = G ",muOS-Keys,a:b4,b:b3,x:b5,y:b6,leftshoulder:b7,rightshoulder:b8,start:b10,back:b9,platform:Linux,";
static const char *win = G ",muOS-Keys,a:b0,b:b1,platform:Windows,";
int main(void) {
  nxinput_corpus c; int i, j, k; uint64_t d1, d2;
  nxinput_corpus_init(&c, "Linux");
  CHECK(nxinput_corpus_add(&c, "zz,bad", NXINPUT_CORPUS_BUNDLE, 0, 0) == -1, "malformed line refused");
  CHECK(nxinput_corpus_add(&c, muos, NXINPUT_CORPUS_BUNDLE, 1, 1) == 0, "bundle line registered (index 0)");
  /* the muOS field log: the same line arrives from the env AND the live db -> matching=2 */
  i = nxinput_corpus_add(&c, muos, NXINPUT_CORPUS_HINT_ENV, 1, 1);
  j = nxinput_corpus_add(&c, muos, NXINPUT_CORPUS_LIVEDB, 1, 1);
  CHECK(i == 0 && j == 0 && c.line[0].matching == 3 && c.collapsed == 2 && c.count == 1, "byte-identical duplicates collapse (matching counted, one store line)");
  k = nxinput_corpus_add(&c, win, NXINPUT_CORPUS_BUILTIN, 1, 0);
  CHECK(k == 1 && !c.line[1].platform_ok && c.filtered_platform == 1, "platform:Windows line filtered before election");
  CHECK(nxinput_corpus_elect(&c, G) == 0, "election ignores the filtered platform line: the muOS line is elected");
  d1 = nxinput_corpus_digest(&c);
  /* divergent duplicate (modern face swap) from a bundle without proved precedence -> refused */
  nxinput_corpus_init(&c, "Linux");
  nxinput_corpus_add(&c, muos, NXINPUT_CORPUS_LIVEDB, 1, 1);
  nxinput_corpus_add(&c, modern, NXINPUT_CORPUS_BUNDLE, 0, 1);
  CHECK(nxinput_corpus_elect(&c, G) == -2 && c.refused_divergent == 1, "divergent same-GUID lines without proved precedence: GUID refused before the store");
  /* divergent duplicate whose later line has PROVED precedence and both are provider-native -> last-wins */
  nxinput_corpus_init(&c, "Linux");
  nxinput_corpus_add(&c, muos, NXINPUT_CORPUS_LIVEDB, 1, 1);
  nxinput_corpus_add(&c, modern, NXINPUT_CORPUS_HINT_ENV, 1, 1);
  CHECK(nxinput_corpus_elect(&c, G) == 1, "proved precedence + provider-native on both sides: last-wins accepted");
  /* MUTANT: the later line is not authored for the provider (foreign domain) -> refused even with priority */
  nxinput_corpus_init(&c, "Linux");
  nxinput_corpus_add(&c, muos, NXINPUT_CORPUS_LIVEDB, 1, 1);
  nxinput_corpus_add(&c, modern, NXINPUT_CORPUS_ADDMAPPING, 1, 0);
  CHECK(nxinput_corpus_elect(&c, G) == -2, "MUTANT killed: last-wins with a line not native to the provider -> refused");
  d2 = nxinput_corpus_digest(&c);
  CHECK(d1 != d2 && d1 != 0, "corpus digest changes with the accepted lines (feeds integral equivalence)");
  CHECK(nxinput_corpus_elect(&c, "00000000000000000000000000000000") == -1, "unknown GUID: no line");
  CHECK(!strcmp(nxinput_corpus_origin_name(NXINPUT_CORPUS_LIVEDB), "livedb"), "origin names");
  /* overflow is counted, never silent */
  nxinput_corpus_init(&c, "Linux");
  { char line[128]; unsigned n; int r = 0; for (n = 0; n < NXINPUT_CORPUS_MAX_LINES + 2; n++) { snprintf(line, sizeof line, "%08x000000000000000000000000,pad%u,a:b0,", n, n); r = nxinput_corpus_add(&c, line, NXINPUT_CORPUS_BUILTIN, 1, 1); } CHECK(r == -1 && c.overflow == 2 && c.count == NXINPUT_CORPUS_MAX_LINES, "overflow visible (counted), bounded storage"); }
  printf(fails ? "v5-corpus: FAIL\n" : "v5-corpus: OK\n"); return fails ? 1 : 0;
}
