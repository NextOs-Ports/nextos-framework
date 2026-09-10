/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C5B (audit 116B): the parser's closed grammar.
 *
 * CLAIM CLASS: FIXTURE_HOST. Nothing here executes an engine. What it proves
 * is that every field the 116A parser used to CLASSIFY AS "other" and then
 * ignore now blocks, and that hats and axes are checked against the measured
 * capability and the measured absinfo instead of being counted and waved
 * through.
 *
 * Each case states the exact result it demands; "it did not crash" is not a
 * pass here. */
#include "nxinput_godot.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define BPL (8u * (unsigned int)sizeof(unsigned long))
#define KEY_WORDS ((NXINPUT_GODOT_KEY_BITS + BPL - 1u) / BPL)
#define ABS_WORDS ((NXINPUT_GODOT_ABS_BITS + BPL - 1u) / BPL)

static unsigned long g_keys[KEY_WORDS];
static unsigned long g_abs[ABS_WORDS];
static nxinput_godot_absinfo g_absinfo[NXINPUT_GODOT_ABS_BITS];

static void set_bit(unsigned long *b, unsigned int c) {
  b[c / BPL] |= 1ul << (c % BPL);
}

/* One hat (ABS_HAT0X/Y), two centred sticks, two one-sided triggers: the
 * shape a kernel reports for an ordinary pad. Hat 1 is deliberately absent
 * so a binding that names it has something real to fail against. */
static void build_pad(void) {
  static const unsigned int gamepad[] = {0x130, 0x131, 0x133, 0x134, 0x136,
                                         0x137, 0x13a, 0x13b, 0x13c, 0x13d,
                                         0x13e};
  unsigned int i;

  memset(g_keys, 0, sizeof g_keys);
  memset(g_abs, 0, sizeof g_abs);
  memset(g_absinfo, 0, sizeof g_absinfo);
  for (i = 0u; i < sizeof gamepad / sizeof gamepad[0]; i++) {
    set_bit(g_keys, gamepad[i]);
  }
  for (i = 0u; i < 6u; i++) {
    set_bit(g_abs, i);
  }
  set_bit(g_abs, NXINPUT_GODOT_ABS_HAT0X);
  set_bit(g_abs, NXINPUT_GODOT_ABS_HAT0X + 1u);
  for (i = 0u; i < 6u; i++) {
    g_absinfo[i].present = 1u;
    if (i == 2u || i == 5u) { /* triggers: 0..255, no negative half */
      g_absinfo[i].minimum = 0;
      g_absinfo[i].maximum = 255;
    } else {
      g_absinfo[i].minimum = -32768;
      g_absinfo[i].maximum = 32767;
      g_absinfo[i].flat = 128;
    }
  }
  /* ABS 4 is advertised in the bitmap but the kernel reports no usable
   * range for it -- exactly the case a bitmap-only check cannot see. */
  g_absinfo[4].present = 0u;
  g_absinfo[NXINPUT_GODOT_ABS_HAT0X].present = 1u;
  g_absinfo[NXINPUT_GODOT_ABS_HAT0X].minimum = -1;
  g_absinfo[NXINPUT_GODOT_ABS_HAT0X].maximum = 1;
  g_absinfo[NXINPUT_GODOT_ABS_HAT0X + 1u] =
      g_absinfo[NXINPUT_GODOT_ABS_HAT0X];
}

#define GUID_AND_NAME "0300000009120000a1c5000010010000,NXC5 Pad,"

static nxinput_godot_result run(const char *tail, int with_absinfo) {
  nxinput_godot_origin origin;
  nxinput_godot_caps caps;
  nxinput_godot_evidence evidence;
  char mapping[NXINPUT_GODOT_LINE_MAX];
  char served[NXINPUT_GODOT_LINE_MAX];

  (void)snprintf(mapping, sizeof mapping, "%s%s", GUID_AND_NAME, tail);
  (void)nxinput_godot_origin_declare(&origin, NXINPUT_GODOT_DOMAIN_GODOT,
                                     "portmaster-gui", "receipt-fixture");
  (void)nxinput_godot_caps_init(&caps, g_keys, NXINPUT_GODOT_KEY_BITS, g_abs,
                                NXINPUT_GODOT_ABS_BITS);
  if (with_absinfo) {
    (void)nxinput_godot_caps_set_absinfo(&caps, g_absinfo,
                                         NXINPUT_GODOT_ABS_BITS);
  }
  return nxinput_godot_serve(NXINPUT_GODOT_ENGINE_3, &origin, &caps, mapping,
                             served, sizeof served, &evidence);
}

static void expect(const char *what, const char *tail, int with_absinfo,
                   nxinput_godot_result want) {
  nxinput_godot_result got = run(tail, with_absinfo);

  g_checks++;
  if (got == want) {
    printf("ok   %-52s -> %s\n", what, nxinput_godot_result_name(got));
  } else {
    printf("FAIL %-52s -> %s (wanted %s)\n", what,
           nxinput_godot_result_name(got),
           nxinput_godot_result_name(want));
    g_failures++;
  }
}

int main(void) {
  build_pad();

  /* ---- the control line: this one MUST be admitted, or every negative
   * below would pass for the wrong reason. */
  expect("a real line with buttons, axes and a hat",
         "a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,lefttrigger:a2,"
         "righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
         "platform:Linux,",
         1, NXINPUT_GODOT_BYTE_INTACT);

  /* ---- buttons: syntax, range, garbage, suffix */
  expect("`bN` with no ordinal", "a:b,", 1, NXINPUT_GODOT_SYNTAX);
  expect("`bN` with a non-digit ordinal", "a:bx,", 1, NXINPUT_GODOT_SYNTAX);
  expect("`bN` with trailing garbage", "a:b3x,", 1, NXINPUT_GODOT_SYNTAX);
  expect("`bN` with an axis inversion suffix", "a:b3~,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("`bN` with a half-range sign", "a:+b3,", 1, NXINPUT_GODOT_SYNTAX);
  expect("`bN` ordinal overflowing the digit budget", "a:b99999,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("`bN` naming a button this pad has not", "a:b40,", 1,
         NXINPUT_GODOT_UNREACHABLE);

  /* ---- axes */
  expect("`aN` with no ordinal", "leftx:a,", 1, NXINPUT_GODOT_SYNTAX);
  expect("`aN` with trailing garbage", "leftx:a1x,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("`~` written as a prefix instead of a suffix", "leftx:~a1,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("`aN` naming an axis this pad has not", "leftx:a20,", 1,
         NXINPUT_GODOT_UNREACHABLE);
  expect("an inverted axis is legal", "leftx:a0~,", 1,
         NXINPUT_GODOT_BYTE_INTACT);

  /* ---- hats: the mask the 116A parser never read */
  expect("`hN` with no dot", "dpup:h0,", 1, NXINPUT_GODOT_SYNTAX);
  expect("`hN.` with an empty mask", "dpup:h0.,", 1, NXINPUT_GODOT_SYNTAX);
  expect("a hat mask that is not one direction", "dpup:h0.3,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("a hat mask of zero", "dpup:h0.0,", 1, NXINPUT_GODOT_SYNTAX);
  expect("a hat mask past left", "dpup:h0.16,", 1, NXINPUT_GODOT_SYNTAX);
  expect("a hat ordinal past ABS_HAT3Y", "dpup:h9.1,", 1,
         NXINPUT_GODOT_HAT);
  expect("a hat this pad does not physically have", "dpup:h1.1,", 1,
         NXINPUT_GODOT_HAT);
  expect("a hat with a half-range sign", "dpup:+h0.1,", 1,
         NXINPUT_GODOT_SYNTAX);

  /* ---- absinfo: SDL's range decision must be reproducible */
  expect("an axis bound with no measured absinfo at all", "leftx:a0,", 0,
         NXINPUT_GODOT_ABSINFO);
  expect("the negative half of a 0..255 trigger", "lefttrigger:-a2,", 1,
         NXINPUT_GODOT_ABSINFO);
  expect("the positive half of a 0..255 trigger is fine",
         "lefttrigger:+a2,", 1, NXINPUT_GODOT_BYTE_INTACT);
  expect("an axis the bitmap advertises but EVIOCGABS does not", "leftx:a4,",
         1, NXINPUT_GODOT_ABSINFO);

  /* ---- fields and keys */
  expect("a field with no colon", "a:b0,justatoken,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("an empty field between two commas", "a:b0,,b:b1,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("a control name this contract does not define", "nosuchbutton:b0,",
         1, NXINPUT_GODOT_SYNTAX);
  expect("a known key with an unknown value class", "a:z9,", 1,
         NXINPUT_GODOT_SYNTAX);
  expect("a known key with an empty value", "a:,", 1, NXINPUT_GODOT_SYNTAX);
  expect("declared metadata is carried, not parsed as a binding",
         "a:b0,crc:1234,type:gamepad,platform:Linux,", 1,
         NXINPUT_GODOT_BYTE_INTACT);

  /* ---- duplicates */
  expect("an identical duplicate", "a:b0,a:b0,", 1, NXINPUT_GODOT_DUPLICATE);
  expect("a conflicting duplicate", "a:b0,a:b1,", 1,
         NXINPUT_GODOT_DUPLICATE);

  /* ---- the line itself */
  expect("a mapping with no binding at all", "platform:Linux,", 1,
         NXINPUT_GODOT_BYTE_INTACT);

  printf("test_godot_parser_negatives: %d checks, %d failures -- %s\n",
         g_checks, g_failures,
         g_failures == 0 ? "ALL PASS (FIXTURE_HOST)" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
