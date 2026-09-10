/* SPDX-License-Identifier: GPL-3.0-only */
/* NEXTOS_CONTROLLERS/3 -- FACE_LAYOUT parser gates (nxinput 0.10.0).
 *
 * V3 inherits V2 completeness and tri-state and adds exactly one mandatory
 * preamble line `FACE_LAYOUT = auto|modern|retro`, case-exact for key and
 * value. V1/V2 stay byte- and semantics-identical and mean `auto`. The
 * negative cases follow the sealed V4-CTRL-01 oracle corpus (N01-N09,
 * N26, N27). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nxinput_gptk.h"

/* One complete, tri-state-valid section body (the 18 controls, once). */
#define V3_SECTION_BODY \
  "A = example.accept\n" \
  "B = example.back\n" \
  "X = null\n" \
  "Y = native\n" \
  "L1 = null\n" \
  "R1 = null\n" \
  "L2 = null\n" \
  "R2 = null\n" \
  "L3 = null\n" \
  "R3 = null\n" \
  "START = null\n" \
  "SELECT = null\n" \
  "UP = null\n" \
  "DOWN = null\n" \
  "LEFT = null\n" \
  "RIGHT = null\n" \
  "LEFT_STICK = null\n" \
  "RIGHT_STICK = null\n"

#define V3_BODY "[menu]\n" V3_SECTION_BODY "[gameplay]\n" V3_SECTION_BODY

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *label) {
  checks++;
  if (condition) {
    printf("ok   %s\n", label);
  } else {
    failures++;
    printf("FAIL %s\n", label);
  }
}

static int parse_text(const char *text, nxinput_gptk *map, char *error,
                      size_t error_size) {
  return nxinput_gptk_parse(text, strlen(text), map, error, error_size);
}

int main(void) {
  nxinput_gptk map;
  char error[256];
  int rc;

  /* Positive: the three exact values. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "port = tearscape\n"
                  "FACE_LAYOUT = auto\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == 0, "V3 with FACE_LAYOUT = auto parses");
  check(map.schema_version == NXINPUT_GPTK_SCHEMA_V3, "schema is 3");
  check(nxinput_gptk_face_layout_of(&map) == NXINPUT_GPTK_FACE_LAYOUT_AUTO,
        "layout reads back as auto");
  check(strcmp(map.port, "tearscape") == 0, "port line still parses");
  check(nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_X,
                            0) == NXINPUT_GPTK_DECIDE_SUPPRESS,
        "V3 inherits V2 null/SUPPRESS");
  check(nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_Y,
                            0) == NXINPUT_GPTK_DECIDE_NATIVE,
        "V3 inherits V2 native");

  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = modern\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == 0 && nxinput_gptk_face_layout_of(&map) ==
                       NXINPUT_GPTK_FACE_LAYOUT_MODERN,
        "FACE_LAYOUT = modern parses (port line optional as in V2)");
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = retro\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == 0 && nxinput_gptk_face_layout_of(&map) ==
                       NXINPUT_GPTK_FACE_LAYOUT_RETRO,
        "FACE_LAYOUT = retro parses");

  /* N01: V3 without FACE_LAYOUT fails closed naming the key. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n" V3_BODY, &map, error,
                  sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "FACE_LAYOUT") != NULL,
        "N01: V3 without FACE_LAYOUT is rejected naming the key");
  check(nxinput_gptk_face_layout_of(&map) == NXINPUT_GPTK_FACE_LAYOUT_AUTO,
        "a failed parse leaves the cleared map at auto");

  /* N02: duplicate FACE_LAYOUT. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = retro\nFACE_LAYOUT = retro\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_DUPLICATE,
        "N02: duplicate FACE_LAYOUT is NXI1003");

  /* N03: wrong-case key is never accepted silently. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "face_layout = retro\nFACE_LAYOUT = retro\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc != 0, "N03: lowercase face_layout key is rejected");
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "Face_Layout = retro\nFACE_LAYOUT = retro\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc != 0, "N03: Face_Layout key is rejected");

  /* N04: wrong-case value fails with its OWN message. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = AUTO\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "exactly in lowercase") != NULL,
        "N04: FACE_LAYOUT = AUTO names the case mistake");
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = Modern\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "exactly in lowercase") != NULL,
        "N04: FACE_LAYOUT = Modern names the case mistake");

  /* N05: values outside the set. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = classic\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED,
        "N05: FACE_LAYOUT = classic is rejected");
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = 1\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED,
        "N05: FACE_LAYOUT = 1 is rejected");

  /* N06: FACE_LAYOUT inside a context section is an unknown control. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = auto\n"
                  "[menu]\nFACE_LAYOUT = retro\n" V3_SECTION_BODY
                  "[gameplay]\n" V3_SECTION_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_UNKNOWN_NAME,
        "N06: FACE_LAYOUT inside [menu] is an unknown control");

  /* N07: FACE_LAYOUT before the magic. */
  rc = parse_text("FACE_LAYOUT = auto\n"
                  "format = NEXTOS_CONTROLLERS/3\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_BAD_MAGIC,
        "N07: FACE_LAYOUT before the magic is a magic error");

  /* N08/N09: the key never enters schema 1 or 2. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/1\n"
                  "FACE_LAYOUT = auto\n"
                  "[menu]\nA = example.accept\n[gameplay]\nB = example.back\n",
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "NEXTOS_CONTROLLERS/3") != NULL,
        "N08: FACE_LAYOUT in a V1 file is rejected naming V3");
  rc = parse_text("format = NEXTOS_CONTROLLERS/2\n"
                  "FACE_LAYOUT = auto\n" V3_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "NEXTOS_CONTROLLERS/3") != NULL,
        "N09: FACE_LAYOUT in a V2 file is rejected naming V3");

  /* N26: incomplete section under V3 keeps the V2 completeness error. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = auto\n"
                  "[menu]\nA = example.accept\n"
                  "[gameplay]\n" V3_SECTION_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "NEXTOS_CONTROLLERS/3 section [menu] omits") !=
                NULL,
        "N26: V3 inherits section completeness");

  /* N27: tri-state near-misses stay case-sensitive. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/3\n"
                  "FACE_LAYOUT = auto\n"
                  "[menu]\n"
                  "A = example.accept\nB = example.back\nX = NULL\n"
                  "Y = null\nL1 = null\nR1 = null\nL2 = null\nR2 = null\n"
                  "L3 = null\nR3 = null\nSTART = null\nSELECT = null\n"
                  "UP = null\nDOWN = null\nLEFT = null\nRIGHT = null\n"
                  "LEFT_STICK = null\nRIGHT_STICK = null\n"
                  "[gameplay]\n" V3_SECTION_BODY,
                  &map, error, sizeof error);
  check(rc == NXINPUT_GPTK_ERR_MALFORMED &&
            strstr(error, "lowercase") != NULL,
        "N27: X = NULL stays a named case error under V3");

  /* V1/V2 equivalence: both schemas read back as auto and keep parsing. */
  rc = parse_text("format = NEXTOS_CONTROLLERS/1\n"
                  "[menu]\nA = example.accept\n[gameplay]\nB = example.back\n",
                  &map, error, sizeof error);
  check(rc == 0 && nxinput_gptk_face_layout_of(&map) ==
                       NXINPUT_GPTK_FACE_LAYOUT_AUTO,
        "a V1 file still parses and means auto");
  rc = parse_text("format = NEXTOS_CONTROLLERS/2\n" V3_BODY, &map, error,
                  sizeof error);
  check(rc == 0 && nxinput_gptk_face_layout_of(&map) ==
                       NXINPUT_GPTK_FACE_LAYOUT_AUTO,
        "a V2 file still parses and means auto");

  check(strcmp(nxinput_gptk_face_layout_name(
                   (int)NXINPUT_GPTK_FACE_LAYOUT_MODERN),
               "modern") == 0 &&
            strcmp(nxinput_gptk_face_layout_name(
                       (int)NXINPUT_GPTK_FACE_LAYOUT_RETRO),
                   "retro") == 0 &&
            strcmp(nxinput_gptk_face_layout_name(99), "auto") == 0,
        "layout names are stable and out-of-range defaults to auto");

  printf("test_gptk_v3: %d checks, %d failures\n", checks, failures);
  if (failures != 0) {
    return 1;
  }
  puts("test_gptk_v3: ALL PASS");
  return 0;
}
