/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_CORPUS_H
#define NXINPUT_CORPUS_H
/*
 * nxinput_corpus -- V5 (0.11.0), mission 5.2 / B6: INVENTORY of the active
 * mapping corpus and its PRECEDENCE, before any line touches the store.
 *
 * Every mapping line the process could consume is registered with its
 * origin (built-in DB of the DSO, SDL_GAMECONTROLLERCONFIG hint, file,
 * live CFW database, port bundle, AddMapping call), its priority class
 * (proved for that provider, or not), its `platform:` field and a hash.
 * For one GUID the corpus then elects at most ONE line:
 *   - byte-identical duplicates collapse (`matching=N`, the field case
 *     `matching=2/domain_lines=3` is the fixture);
 *   - `platform:` other than the running platform is filtered before
 *     election, never after;
 *   - divergent duplicates: last-wins ONLY when the later line's priority
 *     is proved AND both lines were authored for the provider (same
 *     provenance class); otherwise the GUID is REFUSED before the store
 *     (`refused_divergent`), which is the C3 "same-GUID divergent" rule
 *     seen from the corpus side.
 * The corpus digest (all accepted lines, in precedence order) is the
 * `corpus_digest` field of the integral equivalence (nxinput_decision).
 *
 * Pure. Bounded storage; no I/O.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum nxinput_corpus_origin {
  NXINPUT_CORPUS_BUILTIN = 0, NXINPUT_CORPUS_HINT_ENV, NXINPUT_CORPUS_FILE,
  NXINPUT_CORPUS_LIVEDB, NXINPUT_CORPUS_BUNDLE, NXINPUT_CORPUS_ADDMAPPING,
  NXINPUT_CORPUS_ORIGIN_COUNT
} nxinput_corpus_origin;

#define NXINPUT_CORPUS_MAX_LINES 64u
#define NXINPUT_CORPUS_LINE_MAX 512u
#define NXINPUT_CORPUS_GUID_HEX 33u

typedef struct nxinput_corpus_line {
  char guid[NXINPUT_CORPUS_GUID_HEX];
  char text[NXINPUT_CORPUS_LINE_MAX];
  uint64_t hash;
  uint8_t origin;          /* nxinput_corpus_origin */
  uint8_t priority_proved; /* precedence of this origin proved for the provider */
  uint8_t provider_native; /* authored for the provider that will consume it */
  uint8_t platform_ok;     /* platform: matches, or absent */
  unsigned matching;       /* byte-identical duplicates collapsed into this */
  unsigned seq;            /* registration order */
} nxinput_corpus_line;

typedef struct nxinput_corpus {
  nxinput_corpus_line line[NXINPUT_CORPUS_MAX_LINES]; unsigned count;
  char platform[16];
  unsigned refused_divergent, filtered_platform, collapsed, overflow;
} nxinput_corpus;

void nxinput_corpus_init(nxinput_corpus *c, const char *platform);
/* Register one line. Returns index, or -1 (malformed/overflow). Byte-identical
 * duplicates for the same GUID bump `matching` and return the existing index. */
int nxinput_corpus_add(nxinput_corpus *c, const char *line, nxinput_corpus_origin origin,
                       int priority_proved, int provider_native);
/* Elect the line for `guid`: returns the index, -1 when none, -2 when the GUID
 * is REFUSED (divergent duplicates without a proved precedence). */
int nxinput_corpus_elect(nxinput_corpus *c, const char *guid);
/* Digest of every platform-accepted line in precedence order. */
uint64_t nxinput_corpus_digest(const nxinput_corpus *c);
const char *nxinput_corpus_origin_name(nxinput_corpus_origin o);
#ifdef __cplusplus
}
#endif
#endif
