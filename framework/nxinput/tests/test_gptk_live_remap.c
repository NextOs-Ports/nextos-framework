/* SPDX-License-Identifier: GPL-3.0-only */
/* _DEFAULT_SOURCE: mkdtemp/O_DIRECTORY under -std=c99. */
#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 700
/* V4-CONTROLS-LIVE gate: editing one action in NEXTOSCONTROLLERS.gptk MUST
 * change which sink fires, through the exact runtime chain a port embeds
 * (load_at -> decide -> dispatcher -> sink). This is the permanent proof
 * that the editable file governs the game instead of documenting it. */
#include "nxinput_gptk.h"
#include "nxinput_gptk_loader.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const ALLOWED[] = { "ui.confirm", "ui.cancel" };

static char g_dir[256];
static int g_fail;
static char g_last[80];

static void expect(int ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "live-remap FAIL: %s\n", what);
    g_fail = 1;
  }
}

static void sink(void *user, const char *action, int pressed, float value) {
  (void)value;
  snprintf(g_last, sizeof(g_last), "%s:%s:%d", (const char *)user, action,
           pressed);
}

static void write_map(const char *dir, const char *name, const char *body) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  fputs(body, f);
  fclose(f);
}

static int load(nxinput_gptk *map, nxinput_gptk_load_receipt *receipt) {
  char defaults[512];
  snprintf(defaults, sizeof(defaults), "%s/defaults", g_dir);
  int owner = open(g_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  int fallback = open(defaults, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (owner < 0 || fallback < 0) { perror("dirfd"); exit(1); }
  int rc = nxinput_gptk_load_at(owner, fallback, ALLOWED, 2, map, receipt);
  close(owner);
  close(fallback);
  return rc;
}

int main(void) {
  snprintf(g_dir, sizeof(g_dir), "/tmp/nxgptk-remap-XXXXXX");
  if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
  char defaults[512];
  snprintf(defaults, sizeof(defaults), "%s/defaults", g_dir);
  mkdir(defaults, 0755);
  write_map(defaults, "NEXTOSCONTROLLERS.gptk",
            "format = NEXTOS_CONTROLLERS/1\n"
            "[menu]\nA = ui.confirm\nB = ui.cancel\n"
            "[gameplay]\nA = ui.confirm\n");

  nxinput_gptk map;
  nxinput_gptk_load_receipt receipt;
  nxinput_gptk_dispatcher d;

  expect(load(&map, &receipt) == 0 &&
             receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING,
         "default selected without an owner file");
  nxinput_gptk_dispatcher_init(&d, &map);
  nxinput_gptk_dispatcher_register(&d, "ui.confirm", sink, (void *)"s");
  nxinput_gptk_dispatcher_register(&d, "ui.cancel", sink, (void *)"s");
  g_last[0] = 0;
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  expect(strcmp(g_last, "s:ui.confirm:1") == 0, "default: A -> ui.confirm");

  /* The player edits ONE action: A now cancels. Another sink must fire. */
  write_map(g_dir, "NEXTOSCONTROLLERS.gptk",
            "format = NEXTOS_CONTROLLERS/1\n"
            "[menu]\nA = ui.cancel\nB = ui.confirm\n"
            "[gameplay]\nA = ui.cancel\n");
  expect(load(&map, &receipt) == 0 &&
             receipt.source == NXINPUT_GPTK_LOAD_OWNER,
         "edited owner selected");
  nxinput_gptk_dispatcher_init(&d, &map);
  nxinput_gptk_dispatcher_register(&d, "ui.confirm", sink, (void *)"s");
  nxinput_gptk_dispatcher_register(&d, "ui.cancel", sink, (void *)"s");
  g_last[0] = 0;
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  expect(strcmp(g_last, "s:ui.cancel:1") == 0,
         "EDITED FILE CHANGES THE SINK: A -> ui.cancel");

  /* Context switch releases the latched edge in the old context. */
  g_last[0] = 0;
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
  expect(strcmp(g_last, "s:ui.cancel:0") == 0,
         "context switch releases the latched action");

  char owner_path[512];
  snprintf(owner_path, sizeof(owner_path), "%s/NEXTOSCONTROLLERS.gptk", g_dir);
  unlink(owner_path);
  snprintf(owner_path, sizeof(owner_path), "%s/defaults/NEXTOSCONTROLLERS.gptk", g_dir);
  unlink(owner_path);
  rmdir(defaults);
  rmdir(g_dir);
  if (g_fail) { return 1; }
  printf("gptk live-remap gate: PASS\n");
  return 0;
}
