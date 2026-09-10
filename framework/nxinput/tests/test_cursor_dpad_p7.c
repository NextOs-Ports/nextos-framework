/* SPDX-License-Identifier: GPL-3.0-only */
/* P7 (nxinput 0.10.0) -- CURSOR_DPAD_IF_NO_STICK, the pure half.
 *
 * WHY THE OPT-IN EXISTS (mission case 44, the composition proof): the two
 * pre-existing capabilities do NOT compose into a zero-stick cursor.
 * NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING acts only on the state
 * RETURNED by nxinput_get_pad_with_options(); the cursor reads the stored
 * slot axes directly, and CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING demands
 * a MEASURED left stick, which a zero-stick pad does not have. The gap is
 * real, so the additive opt-in below exists -- exactly as narrow as the
 * contract allows. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../src/nxinput_core.h"

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

int main(void) {
  const uint32_t zero_stick = NXINPUT_PAD_CAP_DPAD;
  const uint32_t with_left = NXINPUT_PAD_CAP_DPAD | NXINPUT_PAD_CAP_LEFT_STICK;
  const uint32_t with_right =
      NXINPUT_PAD_CAP_DPAD | NXINPUT_PAD_CAP_RIGHT_STICK;
  const uint32_t two_sticks = NXINPUT_PAD_CAP_DPAD |
                              NXINPUT_PAD_CAP_LEFT_STICK |
                              NXINPUT_PAD_CAP_RIGHT_STICK;
  float x;
  float y;

  /* 44: the documented gap -- LEFT_STICK_IF_RIGHT_MISSING alone requires a
   * measured left stick, so it can never serve a zero-stick pad. The new
   * opt-in is what engages there. */
  check(nxinput_core_cursor_dpad_engages(
            zero_stick, NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING,
            1) == 0,
        "44: the old composition alone never engages on zero sticks");
  check(nxinput_core_cursor_dpad_engages(
            zero_stick, NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK, 1) == 1,
        "44: the new opt-in engages on a measured zero-stick pad in menu");

  /* 45: opt-in OFF preserves the previous behavior bit for bit. */
  check(nxinput_core_cursor_dpad_engages(zero_stick,
                                         NXINPUT_CURSOR_OPTION_NONE, 1) == 0,
        "45: without the option nothing engages");

  /* 46: never with any stick present -- the RG40XX-H (two sticks) and every
   * one-stick pad keep their native cursor path. */
  check(nxinput_core_cursor_dpad_engages(
            with_left, NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK, 1) == 0,
        "46: a left stick disables the opt-in");
  check(nxinput_core_cursor_dpad_engages(
            with_right, NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK, 1) == 0,
        "46: a right stick disables the opt-in");
  check(nxinput_core_cursor_dpad_engages(
            two_sticks, NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK, 1) == 0,
        "46: two sticks disable the opt-in");

  /* 47: menu/cursor context only, never gameplay. */
  check(nxinput_core_cursor_dpad_engages(
            zero_stick, NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK, 0) == 0,
        "47: gameplay context never engages");

  /* A pad without a D-pad has nothing to derive from. */
  check(nxinput_core_cursor_dpad_engages(
            0u, NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK, 1) == 0,
        "no D-pad capability, no engagement");

  /* The derived vector: cardinal directions and normalized diagonals. */
  nxinput_core_cursor_axes_from_dpad(
      NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT), &x, &y);
  check(x == 1.0f && y == 0.0f, "derive: right is (1, 0)");
  nxinput_core_cursor_axes_from_dpad(
      NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_UP), &x, &y);
  check(x == 0.0f && y == -1.0f, "derive: up is (0, -1)");
  nxinput_core_cursor_axes_from_dpad(
      NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_DOWN) |
          NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT),
      &x, &y);
  check(fabsf(x + 0.70710678f) < 0.0001f && fabsf(y - 0.70710678f) < 0.0001f,
        "derive: a diagonal is normalized");
  nxinput_core_cursor_axes_from_dpad(0u, &x, &y);
  check(x == 0.0f && y == 0.0f, "derive: nothing pressed is neutral");
  nxinput_core_cursor_axes_from_dpad(
      NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT) |
          NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT),
      &x, &y);
  check(x == 0.0f && y == 0.0f, "derive: opposing directions cancel");

  printf("test_cursor_dpad_p7: %d checks, %d failures\n", checks, failures);
  if (failures != 0) {
    return 1;
  }
  puts("test_cursor_dpad_p7: ALL PASS");
  return 0;
}
