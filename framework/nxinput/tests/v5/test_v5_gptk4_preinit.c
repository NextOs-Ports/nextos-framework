#define _GNU_SOURCE 1
/* SPDX-License-Identifier: GPL-3.0-only */
/* V5-M1a item 1: nxinput_gptk4_preinit_load() -- the ONE schema-4 pre-init
 * boundary that replaces the four port copies. Proves: real directories
 * only (symlinked gamedir / defaults are NOT followed: mutant killed),
 * default-then-owner selection, rejected owner keeps the default and its
 * bytes, the V3 projection is exactly the hand-written one of the 0.11.0
 * ports (source enum, error codes, schema 4, sha256), receipt JSON and
 * log line, "." fallback recorded, invalid arguments refused. */
#include "../../include/nxinput_gptk4_preinit.h"
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
static void put(const char *dir, const char *name, const char *text) { char p[768]; FILE *f; snprintf(p, sizeof p, "%s/%s", dir, name); f = fopen(p, "wb"); fputs(text, f); fclose(f); }
static long fsize(const char *dir, const char *name) { char p[768]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, name); return stat(p, &st) == 0 ? (long)st.st_size : -1; }
/* The exact projection the 0.11.0 ports wrote by hand (fp2/nc/blossom/tearscape). */
static uint8_t port_copy_source(const nxinput_gptk4_bridge_receipt *r) {
  return (uint8_t)(r->source == NXINPUT_GPTK4_SRC_OWNER ? NXINPUT_GPTK_LOAD_OWNER
      : r->source == NXINPUT_GPTK4_SRC_DEFAULT ? (r->owner_rejected ? NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED : NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING)
      : NXINPUT_GPTK_LOAD_NONE);
}
static nxinput_gptk4_preinit_result R; /* ~50 KiB: static like the ports keep it */
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0};
  char *t, *m; char base[512], game[600], defs[700], link[600], badlink[700];
  const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
  if (argc < 2) { printf("usage: corpus\n"); return 2; }
  t = slurp(argv[1]); if (!t) { printf("FAIL corpus\n"); return 1; }
  snprintf(base, sizeof base, "%s/nxinput-gptk4-preinit.XXXXXX", tmp);
  CHECK(mkdtemp(base) != NULL, "temp dir");
  snprintf(game, sizeof game, "%s/game", base); mkdir(game, 0700);
  snprintf(defs, sizeof defs, "%s/defaults", game); mkdir(defs, 0700);
  /* invalid arguments */
  CHECK(nxinput_gptk4_preinit_load(game, NULL, &R) == -1 && nxinput_gptk4_preinit_load(game, &c, NULL) == -1, "NULL contract / NULL out refused (-1)");
  /* 1. nothing on disk: boundary ran, port stays native, receipt says why */
  CHECK(nxinput_gptk4_preinit_load(game, &c, &R) == 0 && !R.v3.loaded && R.gamedir_opened && R.defaults_opened && R.bridge.source == NXINPUT_GPTK4_SRC_NONE && R.v3.receipt.source == NXINPUT_GPTK_LOAD_NONE && strstr(R.log_line, "NXI") && strstr(R.log_line, "stay native"), "empty game dir: ran, not loaded, source none, NXI log line");
  CHECK(R.api_version == NXINPUT_GPTK4_PREINIT_API_VERSION && R.struct_size == sizeof R && R.v3.api_version == NXINPUT_GPTK_PREINIT_API_VERSION && R.v3.receipt.api_version == 1u && R.v3.face_layout == (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_AUTO, "api/struct versions and AUTO layout set even when not loaded");
  /* 2. default only */
  put(defs, NXINPUT_GPTK4_OWNER_BASENAME, t);
  CHECK(nxinput_gptk4_preinit_load(game, &c, &R) == 0 && R.v3.loaded && R.bridge.source == NXINPUT_GPTK4_SRC_DEFAULT && !R.bridge.owner_present, "default only: loaded from the default");
  CHECK(R.v3.receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING && R.v3.receipt.source == port_copy_source(&R.bridge) && R.v3.receipt.owner_present == 0 && R.v3.receipt.owner_error_code == 0 && R.v3.receipt.default_error_code == 0 && R.v3.receipt.selected_gptk_schema == 4u && strlen(R.v3.receipt.selected_sha256) == 64 && !strcmp(R.v3.receipt.selected_sha256, R.bridge.sha256), "V3 projection == the ports' hand-written one (source, codes, schema 4, sha256)");
  CHECK(R.v3.map.schema_version == 4u && R.v3.map.kind[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A] == NXINPUT_GPTK_BINDING_ACTION && !strcmp(R.v3.map.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A], "fp2.primary") && !strcmp(R.map4.port, "fp2"), "live V3 map projected and schema-4 map kept for the registry");
  CHECK(strstr(R.log_line, "preinit: NEXTOS_CONTROLLERS/4 source=default_owner_missing layout=auto sha256=") != NULL, "log line names schema 4 and the source");
  CHECK(strstr(R.receipt_json, "\"schema\":\"nxinput-gptk4-bridge/1\"") && strstr(R.receipt_json, "\"source\":\"default\"") && strstr(R.receipt_json, "\"marker\":\"nxinput-gptk-runtime/4\""), "receipt JSON present (bridge schema, source, /4 marker)");
  /* 3. valid owner wins */
  m = with(t, "A = action:fp2.primary", "A = action:fp2.secondary"); put(game, NXINPUT_GPTK4_OWNER_BASENAME, m); free(m);
  CHECK(nxinput_gptk4_preinit_load(game, &c, &R) == 0 && R.v3.loaded && R.bridge.source == NXINPUT_GPTK4_SRC_OWNER && R.v3.receipt.source == NXINPUT_GPTK_LOAD_OWNER && R.v3.receipt.owner_present == 1 && !strcmp(R.v3.map.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A], "fp2.secondary") && R.v3.rc == 0, "valid owner wins; V3 source=owner");
  /* 4. rejected owner keeps the default and its bytes; codes as the ports set them */
  put(game, NXINPUT_GPTK4_OWNER_BASENAME, "format = NEXTOS_CONTROLLERS/4\nport = fp2\n[base]\nA = action:nope\n");
  CHECK(nxinput_gptk4_preinit_load(game, &c, &R) == 0 && R.v3.loaded && R.bridge.source == NXINPUT_GPTK4_SRC_DEFAULT && R.bridge.owner_rejected && R.v3.receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED && R.v3.receipt.source == port_copy_source(&R.bridge) && R.v3.receipt.owner_error_code == R.bridge.rc && R.v3.receipt.owner_error_code != 0 && R.v3.receipt.default_error_code == 0 && R.v3.rc == R.bridge.rc, "rejected owner: default kept, owner_error_code = NXI code, default_error_code 0 (as the ports)");
  CHECK(fsize(game, NXINPUT_GPTK4_OWNER_BASENAME) == (long)strlen("format = NEXTOS_CONTROLLERS/4\nport = fp2\n[base]\nA = action:nope\n") && !strcmp(R.v3.map.action[NXINPUT_GPTK_CONTEXT_GAMEPLAY][NXINPUT_GPTK_A], "fp2.primary"), "owner bytes untouched; live map is the default's");
  CHECK(strstr(R.receipt_json, "\"owner_rejected\":1") && strstr(R.log_line, "source=default_owner_rejected") , "receipt/log carry the rejection");
  /* 5. MUTANT: a symlinked game directory must NOT be followed (O_NOFOLLOW) */
  { char p[700]; snprintf(p, sizeof p, "%s/" NXINPUT_GPTK4_OWNER_BASENAME, game); unlink(p); }
  snprintf(link, sizeof link, "%s/game-link", base);
  CHECK(symlink(game, link) == 0, "symlink to the game dir created");
  CHECK(nxinput_gptk4_preinit_load(link, &c, &R) == 0 && !R.gamedir_opened && !R.v3.loaded && R.bridge.source == NXINPUT_GPTK4_SRC_NONE, "MUTANT killed: symlinked game directory followed (O_NOFOLLOW refuses it: not opened, not loaded)");
  /* 6. MUTANT: a symlinked defaults/ must NOT be followed either */
  { char real[700]; snprintf(real, sizeof real, "%s/game2", base); mkdir(real, 0700);
    snprintf(badlink, sizeof badlink, "%s/defaults", real);
    CHECK(symlink(defs, badlink) == 0, "symlink game2/defaults -> game/defaults created");
    CHECK(nxinput_gptk4_preinit_load(real, &c, &R) == 0 && R.gamedir_opened && !R.defaults_opened && !R.v3.loaded && R.bridge.default_rejected, "MUTANT killed: symlinked defaults/ followed (refused; nothing loaded)");
    put(real, NXINPUT_GPTK4_OWNER_BASENAME, t);
    CHECK(nxinput_gptk4_preinit_load(real, &c, &R) == 0 && R.v3.loaded && R.bridge.source == NXINPUT_GPTK4_SRC_OWNER && R.bridge.default_rejected && R.v3.receipt.source == NXINPUT_GPTK_LOAD_OWNER && R.v3.rc == 0, "owner alone (defaults refused): owner selected, default_rejected recorded, rc 0 (owner wins)");
    { char p[900]; snprintf(p, sizeof p, "%s/" NXINPUT_GPTK4_OWNER_BASENAME, real); unlink(p); unlink(badlink); rmdir(real); }
  }
  unlink(link);
  /* 7. missing game dir */
  { char none[700]; snprintf(none, sizeof none, "%s/does-not-exist", base);
    CHECK(nxinput_gptk4_preinit_load(none, &c, &R) == 0 && !R.gamedir_opened && !R.defaults_opened && !R.v3.loaded && !R.gamedir_fallback_cwd, "missing game dir: ran, native, no fallback"); }
  /* 8. "." fallback recorded (the ports' behaviour), nothing there */
  { char cwd[1024]; if (getcwd(cwd, sizeof cwd) && chdir(base) == 0) {
      CHECK(nxinput_gptk4_preinit_load(NULL, &c, &R) == 0 && R.gamedir_fallback_cwd && R.gamedir_opened && !R.v3.loaded, "NULL gamedir: '.' used and recorded as fallback");
      CHECK(nxinput_gptk4_preinit_load("", &c, &R) == 0 && R.gamedir_fallback_cwd, "empty gamedir: same fallback");
      (void)chdir(cwd); } }
  /* cleanup */
  { char p[900]; snprintf(p, sizeof p, "%s/" NXINPUT_GPTK4_OWNER_BASENAME, defs); unlink(p); rmdir(defs); rmdir(game); rmdir(base); }
  free(t);
  printf(fails ? "v5-gptk4-preinit: FAIL\n" : "v5-gptk4-preinit: OK\n"); return fails ? 1 : 0;
}
