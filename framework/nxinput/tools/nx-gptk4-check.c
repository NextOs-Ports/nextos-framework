/* SPDX-License-Identifier: GPL-3.0-only */
/* nx-gptk4-check -- parse a NEXTOS_CONTROLLERS/4 owner file with the real
 * nxinput parser and project it onto the live V3 map. Host/gate tool.
 *   nx-gptk4-check <file> [--contexts a,b] [--keyboard] [--ext EXT.NAME,...] id:kind ...
 * kind = digital|scalar|vector2 (nxproject: button|axis|vector). Exit 0 = ok. */
#include "nxinput_gptk4_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  static nxinput_gptk4_action_decl acts[NXINPUT_GPTK4_MAX_ACTIONS]; static const char *ctxs[16]; static const char *exts[NXINPUT_GPTK4_MAX_EXT];
  static char ctxbuf[256], extbuf[256]; size_t nact = 0, nctx = 0, next = 0; int kb = 0, i;
  nxinput_gptk4_contract c; nxinput_gptk4 g; nxinput_gptk live; nxinput_gptk4_error e; nxinput_gptk4_bridge_receipt r; char json[512];
  FILE *f; long n; char *text;
  if (argc < 2) { fprintf(stderr, "usage: %s <file> [--contexts a,b] [--keyboard] [--ext E,..] id:kind ...\n", argv[0]); return 2; }
  for (i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--keyboard")) { kb = 1; continue; }
    if (!strcmp(argv[i], "--contexts") && i + 1 < argc) { char *t; snprintf(ctxbuf, sizeof ctxbuf, "%s", argv[++i]); for (t = strtok(ctxbuf, ","); t && nctx < 16; t = strtok(NULL, ",")) ctxs[nctx++] = t; continue; }
    if (!strcmp(argv[i], "--ext") && i + 1 < argc) { char *t; snprintf(extbuf, sizeof extbuf, "%s", argv[++i]); for (t = strtok(extbuf, ","); t && next < NXINPUT_GPTK4_MAX_EXT; t = strtok(NULL, ",")) exts[next++] = t; continue; }
    { char *colon = strrchr(argv[i], ':'); const char *kind = colon ? colon + 1 : "digital"; if (colon) *colon = 0;
      if (nact >= NXINPUT_GPTK4_MAX_ACTIONS) { fprintf(stderr, "too many actions\n"); return 2; }
      acts[nact].id = argv[i];
      acts[nact].value_kind = (!strcmp(kind, "scalar") || !strcmp(kind, "axis")) ? NXINPUT_GPTK4_V_SCALAR : (!strcmp(kind, "vector2") || !strcmp(kind, "vector")) ? NXINPUT_GPTK4_V_VECTOR2 : NXINPUT_GPTK4_V_DIGITAL;
      nact++; }
  }
  memset(&c, 0, sizeof c); c.actions = acts; c.count = nact; c.keyboard_backend = (uint8_t)kb; c.contexts = ctxs; c.context_count = nctx; c.extensions = exts; c.extension_count = next;
  f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0 || n > (long)NXINPUT_GPTK4_MAX_BYTES) { fprintf(stderr, "file too large\n"); fclose(f); return 1; }
  text = malloc((size_t)n + 1); if (fread(text, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; } text[n] = 0; fclose(f);
  memset(&e, 0, sizeof e);
  if (nxinput_gptk4_parse(text, (size_t)n, &c, &g, &e) != 0) { printf("REJECT NXI%d %s:%u:%u: %s\n", e.code, argv[1], e.line, e.column, e.what); return 1; }
  memset(&r, 0, sizeof r);
  if (nxinput_gptk4_project(&g, &live, &r) != 0) { printf("REJECT projection\n"); return 1; }
  r.source = NXINPUT_GPTK4_SRC_OWNER;
  nxinput_gptk4_bridge_receipt_json(&r, json, sizeof json);
  printf("OK port=%s overrides=%u keybinds=%u exts=%u digest=%016llx %s\n", g.port, g.overrides, g.keybinds, g.exts, (unsigned long long)g.digest, json);
  return 0;
}
