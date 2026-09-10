#define _GNU_SOURCE 1
/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / E2-E3 bridge: a NEXTOS_CONTROLLERS/4 owner projected onto the live V3
 * dispatch (base reaches every context; sparse overrides; triggers/sticks by
 * mode; owner wins when valid, rejected owner keeps the default and its bytes). */
#include "../../include/nxinput_gptk4_bridge.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu", "pause", "cursor"};
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b; }
static char *with(const char *t, const char *a, const char *b) { const char *p = strstr(t, a); size_t la = strlen(a), lb = strlen(b); char *o; if (!p) return NULL; o = malloc(strlen(t) - la + lb + 1); memcpy(o, t, (size_t)(p - t)); memcpy(o + (p - t), b, lb); strcpy(o + (p - t) + lb, p + la); return o; }
static void put(const char *dir, const char *name, const char *text) { char p[512]; FILE *f; snprintf(p, sizeof p, "%s/%s", dir, name); f = fopen(p, "wb"); fputs(text, f); fclose(f); }
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0};
  nxinput_gptk4 g4; nxinput_gptk live; nxinput_gptk4_bridge_receipt r; nxinput_gptk4_error e; char *t, *m; char json[640];
  char work[] = "/tmp/nxinput-gptk4-bridge.XXXXXX"; char defs[600]; int owner_fd, defaults_fd;
  if (argc < 2) { printf("usage: corpus\n"); return 2; }
  t = slurp(argv[1]); if (!t) { printf("FAIL corpus\n"); return 1; }
  CHECK(nxinput_gptk4_parse(t, strlen(t), &c, &g4, &e) == 0, "complete corpus parses");
  memset(&r, 0, sizeof r);
  CHECK(nxinput_gptk4_project(&g4, &live, &r) == 0 && live.schema_version == NXINPUT_GPTK_SCHEMA_V4, "projection onto the live V3 structure (schema tag 4)");
  CHECK(live.kind[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A] == NXINPUT_GPTK_BINDING_ACTION && !strcmp(live.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A], "fp2.primary"), "base A -> gameplay A = fp2.primary");
  CHECK(live.kind[NXINPUT_GPTK_CONTEXT_MENU][NXINPUT_GPTK_B] == NXINPUT_GPTK_BINDING_ACTION, "base reaches the menu context (unified map)");
  CHECK(live.context_present[0] && live.context_present[1] && live.context_present[2], "every V3 context present");
  CHECK(live.kind[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_LEFT_STICK] == NXINPUT_GPTK_BINDING_ACTION && !strcmp(live.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_LEFT_STICK], "fp2.move"), "stick.left vector -> LEFT_STICK action");
  /* sparse override: the corpus declares [override.menu] X = null only in the menu */
  CHECK(nxinput_gptk4_parse(t, strlen(t), &c, &g4, &e) == 0 && nxinput_gptk4_project(&g4, &live, &r) == 0, "corpus with [override.menu] X = null projects");
  CHECK(live.kind[NXINPUT_GPTK_CONTEXT_MENU][NXINPUT_GPTK_X] == NXINPUT_GPTK_BINDING_NULL && live.kind[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_X] != NXINPUT_GPTK_BINDING_NULL, "override applies to menu only; gameplay keeps base (no third copy)");
  /* remap A -> R2 digital in one owner edit: A null, R2 digital carries the action */
  m = with(t, "A = action:fp2.primary", "A = null");
  { char *m2 = with(m, "[trigger.right]\nmode = analog\nanalog = native\ndigital = null", "[trigger.right]\nmode = digital\nanalog = null\ndigital = action:fp2.primary"); free(m); m = m2; }
  CHECK(m && nxinput_gptk4_parse(m, strlen(m), &c, &g4, &e) == 0 && nxinput_gptk4_project(&g4, &live, &r) == 0, "owner remap A->R2 (digital) parses and projects");
  CHECK(live.kind[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A] == NXINPUT_GPTK_BINDING_NULL && live.kind[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_R2] == NXINPUT_GPTK_BINDING_ACTION && !strcmp(live.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_R2], "fp2.primary"), "A suppressed, R2 carries fp2.primary in the live map (same edit changes the action)");
  free(m);
  /* stick digital mode: no V3 sink, reported not dropped silently */
  m = with(t, "mode = vector\nvector = action:fp2.move", "mode = digital\nvector = null");
  { char *m2 = with(m, "up = null\ndown = null\nleft = null\nright = null", "up = action:fp2.secondary\ndown = action:fp2.guard\nleft = action:fp2.primary\nright = action:fp2.special"); free(m); m = m2; }
  memset(&r, 0, sizeof r);
  CHECK(m && nxinput_gptk4_parse(m, strlen(m), &c, &g4, &e) == 0 && nxinput_gptk4_project(&g4, &live, &r) != 0 && r.rc == -2 && r.stick_digital_unsupported == 1 && strstr(r.what, "mode=vector") != NULL, "MUTANT killed: digital stick projected as a DEAD stick (NULL, native suppressed) -- 0.11.6 refuses the file with the reason; the previous generation/default stays live");
  free(m);
  /* loader: default then owner; rejected owner keeps default and its bytes */
  CHECK(mkdtemp(work) != NULL, "temp dir");
  snprintf(defs, sizeof defs, "%s/defaults", work); mkdir(defs, 0700);
  put(defs, "NEXTOSCONTROLLERS.gptk", t);
  owner_fd = open(work, O_RDONLY | O_DIRECTORY); defaults_fd = open(defs, O_RDONLY | O_DIRECTORY);
  CHECK(nxinput_gptk4_load_project_at(owner_fd, defaults_fd, &c, &g4, &live, &r) == 0 && r.source == NXINPUT_GPTK4_SRC_DEFAULT && !r.owner_present, "no owner: default selected");
  m = with(t, "A = action:fp2.primary", "A = action:fp2.secondary"); put(work, "NEXTOSCONTROLLERS.gptk", m); free(m);
  CHECK(nxinput_gptk4_load_project_at(owner_fd, defaults_fd, &c, &g4, &live, &r) == 0 && r.source == NXINPUT_GPTK4_SRC_OWNER && !strcmp(live.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A], "fp2.secondary"), "valid owner wins over the default");
  put(work, "NEXTOSCONTROLLERS.gptk", "format = NEXTOS_CONTROLLERS/4\nport = fp2\n[base]\nA = action:nope\n");
  CHECK(nxinput_gptk4_load_project_at(owner_fd, defaults_fd, &c, &g4, &live, &r) == 0 && r.source == NXINPUT_GPTK4_SRC_DEFAULT && r.owner_rejected && r.rc != 0 && r.line > 0, "rejected owner: default kept, receipt names line/reason");
  { char p[600]; FILE *f; snprintf(p, sizeof p, "%s/NEXTOSCONTROLLERS.gptk", work); f = fopen(p, "rb"); fseek(f, 0, SEEK_END); CHECK(ftell(f) == (long)strlen("format = NEXTOS_CONTROLLERS/4\nport = fp2\n[base]\nA = action:nope\n"), "owner bytes untouched after rejection"); fclose(f); }
  CHECK(nxinput_gptk4_bridge_receipt_json(&r, json, sizeof json) == 0 && strstr(json, "\"marker\":\"nxinput-gptk-runtime/4\"") && strstr(json, "\"owner_rejected\":1") && strlen(r.sha256) == 64, "receipt JSON carries the /4 marker, the rejection and the sha256 of the selected (default) bytes");
  { char p[600]; snprintf(p, sizeof p, "%s/NEXTOSCONTROLLERS.gptk", work); unlink(p); snprintf(p, sizeof p, "%s/NEXTOSCONTROLLERS.gptk", defs); unlink(p); rmdir(defs); rmdir(work); }
  close(owner_fd); close(defaults_fd); free(t);
  printf(fails ? "v5-gptk4-bridge: FAIL\n" : "v5-gptk4-bridge: OK\n"); return fails ? 1 : 0;
}
