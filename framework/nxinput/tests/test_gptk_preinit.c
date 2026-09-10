/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_gptk_preinit -- the narrow pre-init boundary (nxinput 0.10.0).
 * One read, the layout extracted for the declare boundary, native-safe on
 * every miss. Uses a scratch directory fixture; no SDL. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nxinput_gptk_preinit.h"

#define SECTION \
  "A = example.accept\nB = example.back\nX = null\nY = null\nL1 = null\n" \
  "R1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\nSTART = null\n" \
  "SELECT = null\nUP = null\nDOWN = null\nLEFT = null\nRIGHT = null\n" \
  "LEFT_STICK = null\nRIGHT_STICK = null\n"

static const char *const allowed[] = {"example.accept", "example.back"};

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *label) {
  checks++;
  printf("%s %s\n", condition ? "ok  " : "FAIL", label);
  if (!condition) {
    failures++;
  }
}

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL) { perror(path); exit(2); } /* never inside assert(): NDEBUG builds must still fail loudly */
  fputs(text, f);
  fclose(f);
}

int main(void) {
  char root[] = "/tmp/nxgptk-preinit-XXXXXX";
  char path[512];
  nxinput_gptk_preinit_result result;

  if (mkdtemp(root) == NULL) { perror("mkdtemp"); return 2; } /* side effect kept out of assert() (NDEBUG) */
  (void)snprintf(path, sizeof path, "%s/defaults", root);
  if (mkdir(path, 0755) != 0) { perror(path); return 2; }
  (void)snprintf(path, sizeof path, "%s/defaults/NEXTOSCONTROLLERS.gptk",
                 root);
  write_file(path,
             "format = NEXTOS_CONTROLLERS/3\n"
             "port = example\n"
             "FACE_LAYOUT = retro\n"
             "[menu]\n" SECTION "[gameplay]\n" SECTION);

  /* Default only: loads, layout = retro. */
  assert(nxinput_gptk_preinit_load(root, allowed, 2u, &result) == 0);
  check(result.loaded == 1 && result.rc == 0, "default V3 map loads");
  check(result.face_layout == (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_RETRO,
        "FACE_LAYOUT retro is extracted for the declare boundary");
  check(result.map.schema_version == NXINPUT_GPTK_SCHEMA_V3,
        "the map is held in memory with its schema");
  check(result.receipt.source == (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING,
        "receipt says the owner copy is missing");

  /* Owner copy wins and carries its own layout. */
  (void)snprintf(path, sizeof path, "%s/NEXTOSCONTROLLERS.gptk", root);
  write_file(path,
             "format = NEXTOS_CONTROLLERS/3\n"
             "port = example\n"
             "FACE_LAYOUT = modern\n"
             "[menu]\n" SECTION "[gameplay]\n" SECTION);
  assert(nxinput_gptk_preinit_load(root, allowed, 2u, &result) == 0);
  check(result.loaded == 1 &&
            result.face_layout == (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_MODERN,
        "the owner's FACE_LAYOUT wins over the default's");
  check(result.receipt.source == (uint8_t)NXINPUT_GPTK_LOAD_OWNER,
        "receipt says owner");

  /* A rejected owner falls back to the default -- and to ITS layout. */
  write_file(path, "format = NEXTOS_CONTROLLERS/3\nFACE_LAYOUT = broken\n");
  assert(nxinput_gptk_preinit_load(root, allowed, 2u, &result) == 0);
  check(result.loaded == 1 &&
            result.face_layout == (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_RETRO,
        "a rejected owner falls back to the default and its layout");
  (void)unlink(path);

  /* V1/V2 files mean auto. */
  (void)snprintf(path, sizeof path, "%s/defaults/NEXTOSCONTROLLERS.gptk",
                 root);
  write_file(path,
             "format = NEXTOS_CONTROLLERS/1\n"
             "[menu]\nA = example.accept\n[gameplay]\nB = example.back\n");
  assert(nxinput_gptk_preinit_load(root, allowed, 2u, &result) == 0);
  check(result.loaded == 1 &&
            result.face_layout == (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_AUTO,
        "a V1 default means auto");

  /* No gamedir / missing defaults stay native with layout auto. */
  assert(nxinput_gptk_preinit_load("/nonexistent-nx-preinit", allowed, 2u,
                                   &result) == 0);
  check(result.loaded == 0 &&
            result.face_layout == (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_AUTO,
        "a missing game directory stays native at auto");
  assert(nxinput_gptk_preinit_load(NULL, allowed, 2u, &result) == -1);
  check(result.loaded == 0, "NULL gamedir is a structural error");

  printf("test_gptk_preinit: %d checks, %d failures\n", checks, failures);
  if (failures != 0) {
    return 1;
  }
  puts("test_gptk_preinit: ALL PASS");
  return 0;
}
