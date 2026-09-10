/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_corpus -- see include/nxinput_corpus.h. Pure. */
#include "nxinput_corpus.h"
#include <stdio.h>
#include <string.h>

static uint64_t fnv(const void *d, size_t n, uint64_t h) {
  const unsigned char *p = d; size_t i; if (!h) h = 0xcbf29ce484222325ull;
  for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; } return h;
}

void nxinput_corpus_init(nxinput_corpus *c, const char *platform) {
  if (!c) return;
  memset(c, 0, sizeof *c);
  snprintf(c->platform, sizeof c->platform, "%s", platform ? platform : "Linux");
}

static int platform_matches(const char *line, const char *platform) {
  const char *p = strstr(line, "platform:");
  size_t n;
  if (!p) return 1;
  p += 9; n = strcspn(p, ",");
  return n == strlen(platform) && strncmp(p, platform, n) == 0;
}

int nxinput_corpus_add(nxinput_corpus *c, const char *line, nxinput_corpus_origin origin,
                       int priority_proved, int provider_native) {
  size_t n; unsigned i; nxinput_corpus_line *l; uint64_t h;
  if (!c || !line || origin >= NXINPUT_CORPUS_ORIGIN_COUNT) return -1;
  n = strlen(line);
  if (n < 34 || n >= NXINPUT_CORPUS_LINE_MAX || line[32] != ',') return -1;
  for (i = 0; i < 32; i++) { char ch = line[i]; if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return -1; }
  h = fnv(line, n, 0);
  for (i = 0; i < c->count; i++) {
    if (c->line[i].hash == h && strcmp(c->line[i].text, line) == 0) { c->line[i].matching++; c->collapsed++; return (int)i; }
  }
  if (c->count >= NXINPUT_CORPUS_MAX_LINES) { c->overflow++; return -1; }
  l = &c->line[c->count];
  memset(l, 0, sizeof *l);
  memcpy(l->guid, line, 32); l->guid[32] = 0;
  memcpy(l->text, line, n + 1);
  l->hash = h; l->origin = (uint8_t)origin; l->priority_proved = (uint8_t)!!priority_proved;
  l->provider_native = (uint8_t)!!provider_native; l->matching = 1; l->seq = c->count;
  l->platform_ok = (uint8_t)platform_matches(line, c->platform);
  if (!l->platform_ok) c->filtered_platform++;
  return (int)c->count++;
}

int nxinput_corpus_elect(nxinput_corpus *c, const char *guid) {
  unsigned i; int elected = -1;
  if (!c || !guid) return -1;
  for (i = 0; i < c->count; i++) {
    const nxinput_corpus_line *l = &c->line[i];
    if (strcmp(l->guid, guid) != 0 || !l->platform_ok) continue;
    if (elected < 0) { elected = (int)i; continue; }
    /* divergent duplicate: last-wins only with proved precedence and the
     * same provider-native provenance; otherwise refuse the GUID. */
    if (l->priority_proved && l->provider_native && c->line[elected].provider_native) { elected = (int)i; continue; }
    c->refused_divergent++;
    return -2;
  }
  return elected;
}

uint64_t nxinput_corpus_digest(const nxinput_corpus *c) {
  unsigned i; uint64_t h = 0;
  if (!c) return 0;
  for (i = 0; i < c->count; i++) {
    if (!c->line[i].platform_ok) continue;
    h = fnv(&c->line[i].hash, sizeof c->line[i].hash, h);
    h = fnv(&c->line[i].origin, 1, h);
    h = fnv(&c->line[i].matching, sizeof c->line[i].matching, h);
  }
  return h;
}

const char *nxinput_corpus_origin_name(nxinput_corpus_origin o) {
  static const char *const n[] = {"builtin", "hint-env", "file", "livedb", "bundle", "addmapping"};
  return o < NXINPUT_CORPUS_ORIGIN_COUNT ? n[o] : "?";
}
