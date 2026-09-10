/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_coexist -- see include/nxinput_coexist.h. */
#define _POSIX_C_SOURCE 200809L
#include "nxinput_coexist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ENV "SDL_GAMECONTROLLERCONFIG"

int nxinput_coexist_init(nxinput_coexist *c) { if (!c) return -1; memset(c, 0, sizeof *c); return 0; }

int nxinput_coexist_stage(nxinput_coexist *c) {
  const char *v;
  if (!c || c->staged_done) return -1;
  v = getenv(ENV);
  if (v) {
    size_t n = strlen(v);
    if (n >= NXINPUT_COEXIST_STAGE_MAX) return -1;
    c->staged = malloc(n + 1); if (!c->staged) return -1;
    memcpy(c->staged, v, n + 1); c->staged_len = n; c->had_ambient = 1;
    if (unsetenv(ENV) != 0 || getenv(ENV) != NULL) { free(c->staged); c->staged = NULL; return -1; }
  }
  c->staged_done = 1;
  return 0;
}

int nxinput_coexist_begin(nxinput_coexist *c, uint8_t api, const char *corpus) {
  if (!c || !c->staged_done) return -1;
  if (c->open) { c->concurrent_refused++; return -1; }
  if (corpus && corpus[0]) { if (setenv(ENV, corpus, 1) != 0) return -1; }
  else (void)unsetenv(ENV);
  c->open = 1; c->open_api = api;
  return 0;
}

int nxinput_coexist_begin_with_hint(nxinput_coexist *c, uint8_t api, const char *corpus, nxinput_coexist_sethint_fn sethint) {
  int rc = nxinput_coexist_begin(c, api, corpus);
  if (rc != 0) return rc;
  if (sethint) (void)sethint(ENV, corpus && corpus[0] ? corpus : "");
  return 0;
}

int nxinput_coexist_end(nxinput_coexist *c) {
  if (!c || !c->open) return -1;
  (void)unsetenv(ENV); /* the staged state: nothing in the environment */
  c->open = 0; c->sequenced++;
  return 0;
}

void nxinput_coexist_restore(nxinput_coexist *c) {
  if (!c) return;
  if (c->open) (void)nxinput_coexist_end(c);
  if (c->had_ambient && c->staged) (void)setenv(ENV, c->staged, 1);
  free(c->staged); c->staged = NULL; c->staged_len = 0; c->staged_done = 0;
}

int nxinput_coexist_receipt(const nxinput_coexist *c, char *out, size_t cap) {
  int n;
  if (!c || !out || cap == 0) return -1;
  n = snprintf(out, cap, "NXC6-COEXIST ambient=%u staged_bytes=%lu sequenced=%u concurrent_refused=%u open=%u",
               (unsigned)c->had_ambient, (unsigned long)c->staged_len, c->sequenced, c->concurrent_refused, (unsigned)c->open);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

nxinput_coexist_verdict nxinput_coexist_arbitrate(const nxinput_provider_descriptor *sdl2, const nxinput_provider_descriptor *sdl3) {
  if (!sdl2 || !sdl3 || sdl2->evidence.api != NXINPUT_SDL_API_2 || sdl3->evidence.api != NXINPUT_SDL_API_3) return NXINPUT_COEXIST_AMBIGUOUS;
  if (sdl2->evidence.sha256[0] == '\0' || sdl3->evidence.sha256[0] == '\0') return NXINPUT_COEXIST_AMBIGUOUS; /* bytes not bound: cannot correlate */
  if (nxinput_provider_shared_core(sdl2, sdl3)) return NXINPUT_COEXIST_SHARED_CORE;
  return NXINPUT_COEXIST_SEPARATE;
}
const char *nxinput_coexist_verdict_name(nxinput_coexist_verdict v) {
  switch (v) { case NXINPUT_COEXIST_SEPARATE: return "separate"; case NXINPUT_COEXIST_SHARED_CORE: return "shared-core"; default: return "ambiguous"; }
}
