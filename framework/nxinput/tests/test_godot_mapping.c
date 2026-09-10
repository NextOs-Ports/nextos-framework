/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C5B (audits 116A and 116B): the pure mapping adapter.
 *
 * CLAIM CLASS: FIXTURE. Nothing here executes an engine. The ordinal domains
 * are verified against the pinned upstream sources by godot_domain_gate.py
 * (SOURCE_AUDIT) and the engine's actual behaviour by the C5B matrix, which
 * runs two REAL engines with the seam linked into them
 * (tests/godot_c5b_matrix_gate.py, REAL_API_HOST). This file only proves the
 * pure decisions. */
#include "nxinput_godot.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, name)                                    \
  do {                                                       \
    if (cond) { printf("ok %s\n", name); }                   \
    else { printf("FAIL %s (line %d)\n", name, __LINE__); g_failures++; } \
  } while (0)

#define BPL (8u * (unsigned int)sizeof(unsigned long))
#define KEY_WORDS ((NXINPUT_GODOT_KEY_BITS + BPL - 1u) / BPL)
#define ABS_WORDS ((NXINPUT_GODOT_ABS_BITS + BPL - 1u) / BPL)

static unsigned long g_keys[KEY_WORDS];
static unsigned long g_abs[ABS_WORDS];
static nxinput_godot_absinfo g_absinfo[NXINPUT_GODOT_ABS_BITS];

static void set_key(unsigned long *b, unsigned int c) {
  b[c / BPL] |= 1ul << (c % BPL);
}

static void build_pad(int with_low_keys, int with_misc_range) {
  static const unsigned int gamepad[] = {
      0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c,
      0x13d, 0x13e, 0x220, 0x221, 0x222, 0x223};
  size_t i;

  memset(g_keys, 0, sizeof g_keys);
  memset(g_abs, 0, sizeof g_abs);
  for (i = 0u; i < sizeof(gamepad) / sizeof(gamepad[0]); i++) {
    set_key(g_keys, gamepad[i]);
  }
  if (with_low_keys) {
    set_key(g_keys, 114);
    set_key(g_keys, 115);
    set_key(g_keys, 116);
  }
  if (with_misc_range) {
    set_key(g_keys, 0x100);
    set_key(g_keys, 0x101);
  }
  set_key(g_abs, 0);
  set_key(g_abs, 1);
  set_key(g_abs, 2);
  set_key(g_abs, 5);
  memset(g_absinfo, 0, sizeof g_absinfo);
  /* Sticks are centred, triggers are one-sided: the shape a kernel really
   * reports, so a half-range binding can be checked against it. */
  for (i = 0u; i < 2u; i++) {
    g_absinfo[i].minimum = -32768;
    g_absinfo[i].maximum = 32767;
    g_absinfo[i].flat = 128;
    g_absinfo[i].present = 1u;
  }
  g_absinfo[2].maximum = 255;
  g_absinfo[2].present = 1u;
  g_absinfo[5].maximum = 255;
  g_absinfo[5].present = 1u;
}

static const char BASE[] =
    "0300000009120000a1c5000010010000,NXC5 Pad,a:b1,b:b0,x:b3,y:b2,"
    "leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,guide:b8,"
    "leftstick:b9,rightstick:b10,dpup:b11,dpdown:b12,dpleft:b13,"
    "dpright:b14,leftx:a0,lefty:a1,lefttrigger:a2,righttrigger:a3,"
    "platform:Linux,";

int main(void) {
  nxinput_godot_origin origin;
  nxinput_godot_caps caps;
  nxinput_godot_evidence evidence;
  char served[NXINPUT_GODOT_LINE_MAX];
  char line[640];
  int engine;

  build_pad(0, 0);
  (void)nxinput_godot_caps_init(&caps, g_keys, NXINPUT_GODOT_KEY_BITS, g_abs,
                                NXINPUT_GODOT_ABS_BITS);
  (void)nxinput_godot_caps_set_absinfo(&caps, g_absinfo,
                                       NXINPUT_GODOT_ABS_BITS);

  /* 1. NO DECLARED ORIGIN => BLOCK. The audit removed the inference that
   * used to guess a domain from the binding names, so with nothing declared
   * there is nothing to act on. */
  (void)nxinput_godot_origin_init(&origin);
  CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps, BASE,
                            served, sizeof served, &evidence) ==
                NXINPUT_GODOT_ORIGIN_UNDECLARED &&
            served[0] == '\0',
        "an undeclared origin blocks; the domain is never inferred");
  CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, 0, &caps, BASE, served,
                            sizeof served, &evidence) ==
            NXINPUT_GODOT_ORIGIN_UNDECLARED,
        "a NULL origin blocks too");
  CHECK(nxinput_godot_origin_declare(&origin, NXINPUT_GODOT_DOMAIN_GODOT, "",
                                     "receipt") == -1 &&
            nxinput_godot_origin_declare(&origin, NXINPUT_GODOT_DOMAIN_GODOT,
                                         "prov", "") == -1,
        "an origin without provider or receipt cannot be declared");

  /* 2. A NON-IDENTITY LAYOUT IS PRESERVED. `a:b1,b:b0` is a legitimate
   * mapping, not an error to be corrected -- a real Godot 3 and Godot 4 in
   * godot_real_gate.py confirm the engines honour exactly this. */
  (void)nxinput_godot_origin_declare(&origin, NXINPUT_GODOT_DOMAIN_GODOT,
                                     "portmaster-gui", "receipt-fixture");
  for (engine = 0; engine < (int)NXINPUT_GODOT_ENGINE_COUNT; engine++) {
    CHECK(nxinput_godot_serve((nxinput_godot_engine)engine, &origin, &caps,
                              BASE, served, sizeof served, &evidence) ==
                  NXINPUT_GODOT_BYTE_INTACT &&
              strcmp(served, BASE) == 0 && evidence.rewritten_bindings == 0u,
          engine == 0 ? "godot3: a:b1,b:b0 is served byte-intact"
                      : "godot4: a:b1,b:b0 is served byte-intact");
  }

  /* 3. A declared SDL2 origin on a pad that ranks everything identically is
   * still byte-intact: the result reports what actually happened. */
  (void)nxinput_godot_origin_declare(&origin,
                                     NXINPUT_GODOT_DOMAIN_SDL2_EVDEV,
                                     "portmaster-gui", "receipt-fixture");
  CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps, BASE,
                            served, sizeof served, &evidence) ==
                NXINPUT_GODOT_BYTE_INTACT &&
            strcmp(served, BASE) == 0,
        "declared sdl2 origin on an identical-ranking pad stays intact");

  /* 4. Keys below BTN_MISC do NOT shift the gamepad ordinals: both domains
   * scan the high range first. Measured, not assumed. */
  build_pad(1, 0);
  (void)nxinput_godot_caps_init(&caps, g_keys, NXINPUT_GODOT_KEY_BITS, g_abs,
                                NXINPUT_GODOT_ABS_BITS);
  (void)nxinput_godot_caps_set_absinfo(&caps, g_absinfo,
                                       NXINPUT_GODOT_ABS_BITS);
  CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps, BASE,
                            served, sizeof served, &evidence) ==
                NXINPUT_GODOT_BYTE_INTACT &&
            evidence.ignored_low == 3u && evidence.keys == 18u,
        "three keys below BTN_MISC do not move any gamepad ordinal");

  /* 5. A pad that also advertises [BTN_MISC, BTN_JOYSTICK) DOES diverge, and
   * the conversion happens exactly once. */
  build_pad(1, 1);
  (void)nxinput_godot_caps_init(&caps, g_keys, NXINPUT_GODOT_KEY_BITS, g_abs,
                                NXINPUT_GODOT_ABS_BITS);
  (void)nxinput_godot_caps_set_absinfo(&caps, g_absinfo,
                                       NXINPUT_GODOT_ABS_BITS);
  {
    char shifted[NXINPUT_GODOT_LINE_MAX];
    char again[NXINPUT_GODOT_LINE_MAX];
    int sdl_ordinal = nxinput_godot_button_ordinal(
        NXINPUT_GODOT_DOMAIN_SDL2_EVDEV, &caps, 0x101);
    int godot_ordinal = nxinput_godot_button_ordinal(
        NXINPUT_GODOT_DOMAIN_GODOT, &caps, 0x101);

    CHECK(sdl_ordinal >= 0 && godot_ordinal >= 0 &&
              sdl_ordinal != godot_ordinal,
          "the two domains really do rank a BTN_MISC-range key differently");
    (void)snprintf(shifted, sizeof shifted,
                   "0300000009120000a1c5000010010000,NXC5 Pad,a:b1,b:b0,"
                   "misc1:b%d,leftx:a0,platform:Linux,", sdl_ordinal);
    CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_4, &origin, &caps, shifted,
                              served, sizeof served, &evidence) ==
                  NXINPUT_GODOT_CONVERTED &&
              evidence.rewritten_bindings == 1u &&
              evidence.internal_consistency == 1u,
          "a genuinely divergent ordinal is converted exactly once");
    {
      nxinput_godot_origin godot_origin;

      (void)nxinput_godot_origin_declare(&godot_origin,
                                         NXINPUT_GODOT_DOMAIN_GODOT,
                                         "portmaster-gui", "receipt-fixture");
      CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_4, &godot_origin, &caps,
                                served, again, sizeof again, &evidence) ==
                    NXINPUT_GODOT_BYTE_INTACT &&
                strcmp(again, served) == 0,
            "serving the converted line again is a byte-intact no-op");
    }
  }

  /* 6. Duplicates fail closed, identical ones included. */
  build_pad(0, 0);
  (void)nxinput_godot_caps_init(&caps, g_keys, NXINPUT_GODOT_KEY_BITS, g_abs,
                                NXINPUT_GODOT_ABS_BITS);
  (void)nxinput_godot_caps_set_absinfo(&caps, g_absinfo,
                                       NXINPUT_GODOT_ABS_BITS);
  (void)nxinput_godot_origin_declare(&origin, NXINPUT_GODOT_DOMAIN_GODOT,
                                     "portmaster-gui", "receipt-fixture");
  {
    static const char dup_same[] =
        "0300000009120000a1c5000010010000,NXC5 Pad,a:b0,a:b0,b:b1,"
        "platform:Linux,";
    static const char dup_diff[] =
        "0300000009120000a1c5000010010000,NXC5 Pad,a:b0,a:b1,b:b1,"
        "platform:Linux,";

    CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps,
                              dup_same, served, sizeof served, &evidence) ==
                  NXINPUT_GODOT_DUPLICATE &&
              served[0] == '\0',
          "an IDENTICAL duplicate a:b0,a:b0 fails closed");
    CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps,
                              dup_diff, served, sizeof served, &evidence) ==
              NXINPUT_GODOT_DUPLICATE,
          "a conflicting duplicate a:b0,a:b1 fails closed");
  }

  /* 7. Axes are covered, not just buttons: an axis ordinal the pad does not
   * have blocks instead of silently pointing elsewhere. */
  {
    static const char bad_axis[] =
        "0300000009120000a1c5000010010000,NXC5 Pad,a:b0,leftx:a9,"
        "platform:Linux,";

    CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps,
                              bad_axis, served, sizeof served, &evidence) ==
                  NXINPUT_GODOT_UNREACHABLE &&
              served[0] == '\0' && evidence.axis_bindings == 1u,
          "an axis ordinal the pad lacks blocks, and axes are counted");
  }

  /* 8. A tiny output buffer truncates nothing: it refuses. */
  {
    char tiny[24];

    CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps, BASE,
                              tiny, sizeof tiny, &evidence) ==
                  NXINPUT_GODOT_INVALID &&
              tiny[0] == '\0',
          "an undersized output buffer refuses instead of truncating");
  }

  /* 9. The receipt names the declared origin and its provenance. */
  CHECK(nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps, BASE,
                            served, sizeof served, &evidence) ==
                NXINPUT_GODOT_BYTE_INTACT &&
            nxinput_godot_evidence_line(&evidence, line, sizeof line) > 0 &&
            strstr(line, "declared_origin=godot") != 0 &&
            strstr(line, "provider=portmaster-gui") != 0 &&
            strstr(line, "internal_consistency=1") != 0,
        "the receipt records the DECLARED origin and its provider");
  printf("    receipt: %.180s\n", line);

  if (g_failures != 0) {
    printf("test_godot_mapping: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_godot_mapping: ALL PASS (FIXTURE only; engines proved "
         "separately)\n");
  return 0;
}
