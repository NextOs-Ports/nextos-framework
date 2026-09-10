/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput 0.10.2 -- host gate for nxinput_padset with a fake SDL vtable.
 * Proves: admission callback is the only gate; union of buttons; largest
 * axis deflection; exit chord only on ONE instance; cross-pad denial logged
 * once per occurrence; hotplug removal compacts without losing other pads;
 * cap at NXINPUT_PADSET_MAX; init fails closed on a missing vtable entry. */
#include "nxinput_padset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_pad {
  int32_t instance;
  int is_gc;
  uint8_t buttons[NXINPUT_PADSET_BUTTON_MAX];
  int16_t axes[6];
  int open_count;
} fake_pad;

static fake_pad fakes[6];
static int fake_n;
static int update_calls;

static int f_num(void) { return fake_n; }
static int32_t f_inst(int i) { return fakes[i].instance; }
static int f_isgc(int i) { return fakes[i].is_gc; }
static void *f_open(int i) { fakes[i].open_count++; return &fakes[i]; }
static void f_close(void *c) { ((fake_pad *)c)->open_count--; }
static void *f_joy(void *c) { return c; }
static int32_t f_joyinst(void *j) { return ((fake_pad *)j)->instance; }
static void f_update(void) { update_calls++; }
static uint8_t f_btn(void *c, int b) { return ((fake_pad *)c)->buttons[b]; }
static int16_t f_axis(void *c, int a) { return ((fake_pad *)c)->axes[a]; }

static const nxinput_padset_sdl SDL = {
  f_num, f_inst, f_isgc, f_open, f_close, f_joy, f_joyinst, f_update, f_btn, f_axis
};

static int admit_calls, refuse_index = -1;
static int admit(int idx, void *u) { (void)u; admit_calls++; return idx != refuse_index; }
static unsigned opened_slots[8]; static int opened_n;
static void opened(int idx, unsigned slot, void *c, void *u) { (void)idx; (void)c; (void)u; opened_slots[opened_n++] = slot; }
static uint64_t fake_now = 1000000000ull; static uint64_t fake_clock(void) { return fake_now; }
static char last_log[256]; static int log_calls;
static void logf_(const char *line, void *u) { (void)u; log_calls++; snprintf(last_log, sizeof last_log, "%s", line); }

static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", m); } } while (0)

int main(void)
{
  nxinput_padset set;
  nxinput_padset_sdl broken = SDL; broken.get_axis = NULL;
  CHECK(nxinput_padset_init(&set, &broken, logf_, NULL) == -1, "init must fail closed on a missing vtable entry");
  CHECK(nxinput_padset_init(&set, &SDL, logf_, NULL) == 0, "init");
  nxinput_padset_set_clock(&set, fake_clock);
  CHECK(strcmp(nxinput_padset_marker(), "nxinput-padset/1") == 0, "marker");

  fake_n = 3;
  memset(fakes, 0, sizeof fakes);
  for (int i = 0; i < fake_n; i++) { fakes[i].instance = 10 + i; fakes[i].is_gc = 1; }
  fakes[2].is_gc = 0;              /* a joystick SDL does not know as a controller */
  refuse_index = 1;                /* the authority refuses index 1 */
  unsigned added = nxinput_padset_open_all(&set, admit, opened, NULL);
  CHECK(added == 1 && set.count == 1 && set.pads[0] == &fakes[0], "only the admitted game controller opens");
  CHECK(admit_calls == 3, "admission asked for every index");
  refuse_index = -1;
  added = nxinput_padset_open_all(&set, admit, opened, NULL);
  CHECK(added == 1 && set.count == 2 && set.pads[1] == &fakes[1] && fakes[0].open_count == 1, "re-scan opens the newly admitted pad only once");
  CHECK(opened_n == 2 && opened_slots[0] == 0 && opened_slots[1] == 1, "opened callback with slots");
  CHECK(nxinput_padset_first(&set) == &fakes[0], "first pad");

  /* union of buttons */
  fakes[0].buttons[0] = 1; fakes[1].buttons[3] = 1;
  nxinput_padset_sample(&set);
  CHECK(set.buttons[0] == 1 && set.buttons[3] == 1 && set.buttons[1] == 0 && update_calls == 1, "union of buttons, one update per sample");
  int sel, start;
  nxinput_padset_chord_inputs(&set, &sel, &start);
  CHECK(sel == 0 && start == 0, "no chord without SELECT+START");

  /* largest axis deflection */
  fakes[0].axes[0] = 1000; fakes[1].axes[0] = -20000;
  CHECK(nxinput_padset_axis(&set, 0) == -20000, "largest deflection wins regardless of sign");
  fakes[1].axes[0] = 0;
  CHECK(nxinput_padset_axis(&set, 0) == 1000, "a resting pad never cancels another");

  /* same-instance chord (0.11.1: through the sovereign pre-router -- both
   * pressed inside the window on ONE instance = one exit, both edges
   * consumed, START never reaches the union) */
  memset(fakes[0].buttons, 0, sizeof fakes[0].buttons); memset(fakes[1].buttons, 0, sizeof fakes[1].buttons);
  fakes[0].buttons[NXINPUT_PADSET_BUTTON_BACK] = 1; fakes[0].buttons[NXINPUT_PADSET_BUTTON_START] = 1;
  nxinput_padset_sample(&set);
  nxinput_padset_chord_inputs(&set, &sel, &start);
  CHECK(sel == 1 && start == 1 && set.chord_same_instance == 1 && set.chord_cross_pad == 0, "SELECT+START on one instance arms the chord");
  CHECK(nxinput_padset_exit_requested(&set) == 1 && set.buttons[NXINPUT_PADSET_BUTTON_START] == 0 && set.buttons[NXINPUT_PADSET_BUTTON_BACK] == 0, "the chord's own edges never reach the native union (exit requested once)");
  CHECK(log_calls == 0, "no denial logged for a legitimate chord");
  fakes[0].buttons[NXINPUT_PADSET_BUTTON_BACK] = 0; fakes[0].buttons[NXINPUT_PADSET_BUTTON_START] = 0;
  fake_now += 300000000ull; nxinput_padset_sample(&set);
  CHECK(nxinput_padset_exit_requested(&set) == 0 && set.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "released: request over, nothing leaked");

  /* cross-pad: SELECT on pad 0, START on pad 1 -- two individual presses,
   * forwarded after the pre-router window, never a chord */
  fakes[0].buttons[NXINPUT_PADSET_BUTTON_BACK] = 1; fakes[1].buttons[NXINPUT_PADSET_BUTTON_START] = 1;
  nxinput_padset_sample(&set);
  nxinput_padset_chord_inputs(&set, &sel, &start);
  CHECK(sel == 0 && start == 0 && set.chord_cross_pad == 1, "cross-pad SELECT+START never reaches the chord");
  CHECK(set.buttons[NXINPUT_PADSET_BUTTON_BACK] == 0 && set.buttons[NXINPUT_PADSET_BUTTON_START] == 0, "inside the window the plain edges are retained (START never pauses before the pre-router decides)");
  fake_now += 300000000ull; nxinput_padset_sample(&set);
  CHECK(set.buttons[NXINPUT_PADSET_BUTTON_BACK] == 1 && set.buttons[NXINPUT_PADSET_BUTTON_START] == 1 && nxinput_padset_exit_requested(&set) == 0, "after the window the plain buttons reach the native union, exactly once, no exit");
  CHECK(log_calls == 1 && strstr(last_log, "cross-pad") != NULL, "denial logged once");
  nxinput_padset_sample(&set);
  CHECK(log_calls == 1, "held cross-pad pair does not spam the log");
  fakes[1].buttons[NXINPUT_PADSET_BUTTON_START] = 0;
  nxinput_padset_sample(&set);
  fakes[1].buttons[NXINPUT_PADSET_BUTTON_START] = 1;
  nxinput_padset_sample(&set);
  CHECK(log_calls == 2, "a new occurrence logs again");
  fakes[0].buttons[NXINPUT_PADSET_BUTTON_BACK] = 0; fakes[1].buttons[NXINPUT_PADSET_BUTTON_START] = 0;
  fake_now += 300000000ull; nxinput_padset_sample(&set);

  /* hotplug removal compacts and keeps the other pad */
  CHECK(nxinput_padset_remove_instance(&set, 10) == 1 && set.count == 1 && set.pads[0] == &fakes[1] && fakes[0].open_count == 0, "removal closes only that instance and compacts");
  CHECK(nxinput_padset_remove_instance(&set, 99) == 0, "unknown instance is a no-op");
  CHECK(nxinput_padset_first(&set) == &fakes[1], "first pad after compaction");

  /* cap */
  fake_n = 6;
  for (int i = 0; i < fake_n; i++) { fakes[i].instance = 10 + i; fakes[i].is_gc = 1; }
  nxinput_padset_open_all(&set, admit, opened, NULL);
  CHECK(set.count == NXINPUT_PADSET_MAX, "never more than NXINPUT_PADSET_MAX pads");

  nxinput_padset_close_all(&set);
  CHECK(set.count == 0 && nxinput_padset_first(&set) == NULL, "close_all empties the set");
  for (int i = 0; i < fake_n; i++) CHECK(fakes[i].open_count == 0, "every opened pad was closed exactly once");

  if (fails) { fprintf(stderr, "nxinput_padset=FAIL (%d)\n", fails); return 1; }
  printf("nxinput_padset=PASS union, max-axis, same-instance chord, cross-pad denial, compaction, cap, fail-closed init\n");
  return 0;
}
