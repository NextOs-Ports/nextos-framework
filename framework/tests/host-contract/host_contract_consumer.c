#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: GPL-3.0-only
 * V3 audit (blocker 2): end-to-end proof that an on-disk NEXTOSCONTROLLERS.gptk
 * FILE and a NEXTOSSETTINGS.txt FILE flow through the installed parsers to the
 * final sink / language snapshot. argv[1] = .gptk path, argv[2] = settings
 * path, argv[3] = expected gameplay action for control A, argv[4] = expected
 * resolved language. Reads the files itself (symlink-refused), parses via the
 * installed libraries, feeds A in gameplay to a fake sink and resolves the
 * language against a fixed supported set. */
#include <nxinput_gptk.h>
#include <nxcompat_language_v2.h>
#include <nxcompat_settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char sink_action[80];
static int sink_pressed;
static void sink(void *u, const char *a, int p, float v) {
  (void)u; (void)v; strncpy(sink_action, a, sizeof sink_action - 1u);
  sink_pressed = p;
}

static long read_file(const char *path, char *buf, size_t cap) {
  struct stat st;
  FILE *f;
  size_t n;
  if (lstat(path, &st) != 0) return -1;
  if (S_ISLNK(st.st_mode)) return -1; /* refuse symlink */
  f = fopen(path, "rb");
  if (f == NULL) return -1;
  n = fread(buf, 1u, cap, f);
  fclose(f);
  return (long)n;
}

int main(int argc, char **argv) {
  char gptk[70000], cfg[8192];
  long gn, cn;
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  char err[128];
  nxcompat_settings s;
  nxcompat_language_snapshot snap;
  static const char *supported[] = { "en", "pt-BR" };

  if (argc != 5) { printf("host-contract: usage\n"); return 2; }
  gn = read_file(argv[1], gptk, sizeof gptk);
  cn = read_file(argv[2], cfg, sizeof cfg);
  if (gn < 0 || cn < 0) { printf("host-contract: file read failed\n"); return 1; }

  if (nxinput_gptk_parse(gptk, (size_t)gn, &map, err, sizeof err) != 0) {
    printf("host-contract: gptk parse failed: %s\n", err); return 1;
  }
  nxinput_gptk_dispatcher_init(&d, &map);
  nxinput_gptk_dispatcher_register(&d, argv[3], sink, NULL);
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  if (strcmp(sink_action, argv[3]) != 0 || sink_pressed != 1) {
    printf("host-contract: A in gameplay reached '%s', expected '%s'\n",
           sink_action, argv[3]);
    return 1;
  }

  if (nxcompat_settings_parse(cfg, (size_t)cn, &s, NULL, NULL) != 0) {
    printf("host-contract: settings parse failed\n"); return 1;
  }
  if (nxcompat_language_resolve_v2(NULL, s.language, NULL, NULL,
                                   supported, 2, "en", &snap) != 0) {
    printf("host-contract: language resolve failed\n"); return 1;
  }
  if (strcmp(snap.canonical_tag, argv[4]) != 0) {
    printf("host-contract: language resolved '%s', expected '%s'\n",
           snap.canonical_tag, argv[4]);
    return 1;
  }
  printf("host-contract: FILE->parser->sink action=%s language=%s\n",
         sink_action, snap.canonical_tag);
  return 0;
}
