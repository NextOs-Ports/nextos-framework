/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_android_input.h"
#include "nxinput_exit_chord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;

#define CHECK(condition)                                                       \
  do {                                                                         \
    checks++;                                                                  \
    if (!(condition)) {                                                        \
      fprintf(stderr, "unity-chord: CHECK failed at %s:%d: %s\n", __FILE__,  \
              __LINE__, #condition);                                           \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

_Static_assert((int)NXANDROID_ANDROID_START == (int)NXINPUT_GPTK_START,
               "START ordinal drifted between C7 and C8");
_Static_assert((int)NXANDROID_ANDROID_SELECT == (int)NXINPUT_GPTK_SELECT,
               "SELECT ordinal drifted between C7 and C8");
_Static_assert((int)NXANDROID_ANDROID_L2 == (int)NXINPUT_GPTK_L2,
               "L2 ordinal drifted between C7 and C8");
_Static_assert((int)NXANDROID_ANDROID_R2 == (int)NXINPUT_GPTK_R2,
               "R2 ordinal drifted between C7 and C8");

typedef struct chord_fixture {
  int state[2][NXANDROID_ANDROID_CONTROL_COUNT];
  int guide[2];
  unsigned int reads;
} chord_fixture;

static int read_state(void *userdata, size_t pad, int logical_control) {
  chord_fixture *fixture = (chord_fixture *)userdata;
  fixture->reads++;
  if (pad >= 2u || logical_control < 0 ||
      logical_control >= (int)NXANDROID_ANDROID_CONTROL_COUNT)
    return 0;
  return fixture->state[pad][logical_control];
}

static void poll_three(nxinput_exit_chord *chord, chord_fixture *fixture,
                       int expect_fire) {
  int fired = 0;
  fired += nxinput_exit_chord_poll(chord, read_state, fixture, 2u);
  fired += nxinput_exit_chord_poll(chord, read_state, fixture, 2u);
  fired += nxinput_exit_chord_poll(chord, read_state, fixture, 2u);
  CHECK(fired == expect_fire);
}

int main(void) {
  nxinput_exit_chord chord;
  chord_fixture fixture;
  memset(&fixture, 0, sizeof(fixture));

  nxinput_exit_chord_init(&chord, 0u);
  fixture.state[0][NXANDROID_ANDROID_SELECT] = 1;
  fixture.state[0][NXANDROID_ANDROID_START] = 1;
  poll_three(&chord, &fixture, 1);
  CHECK(nxinput_exit_chord_requested(&chord) == 1);
  CHECK(nxinput_exit_chord_consume(&chord) == 1);
  CHECK(nxinput_exit_chord_consume(&chord) == 0);

  memset(&fixture.state, 0, sizeof(fixture.state));
  nxinput_exit_chord_reset_hold(&chord);
  fixture.state[0][NXANDROID_ANDROID_L2] = 1;
  fixture.state[0][NXANDROID_ANDROID_R2] = 1;
  poll_three(&chord, &fixture, 0);
  CHECK(nxinput_exit_chord_requested(&chord) == 0);

  memset(&fixture.state, 0, sizeof(fixture.state));
  nxinput_exit_chord_reset_hold(&chord);
  fixture.guide[0] = 1;
  fixture.state[0][NXANDROID_ANDROID_START] = 1;
  poll_three(&chord, &fixture, 0);
  CHECK(nxinput_exit_chord_requested(&chord) == 0);

  memset(&fixture.state, 0, sizeof(fixture.state));
  nxinput_exit_chord_reset_hold(&chord);
  fixture.state[0][NXANDROID_ANDROID_SELECT] = 1;
  fixture.state[1][NXANDROID_ANDROID_START] = 1;
  poll_three(&chord, &fixture, 0);
  CHECK(nxinput_exit_chord_requested(&chord) == 0);
  CHECK(fixture.guide[0] == 1);
  CHECK(fixture.reads == 42u);

  printf("nxandroid unity C8 chord: PASS checks=%u same_pad=PASS "
         "l2_r2=NEGATIVE guide_start=NEGATIVE cross_pad=NEGATIVE "
         "unity_events=0 keyboard_events=0\n",
         checks);
  return 0;
}
