/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / D1 + 6.5: calibration fixtures of the matrix. */
#include "../../include/nxinput_axis_calib.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static char trace[256]; static void edge(void *u, int d, int p) { (void)u; char b[8]; snprintf(b, sizeof b, "%c%c ", "UDLR"[d], p ? '+' : '-'); strcat(trace, b); }
int main(void) {
  nxinput_axis_calib c; nxinput_axis_absinfo a;
  /* signed -32768..32767, centre 0 */
  a = (nxinput_axis_absinfo){0, -32768, 32767, 0, 0, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, NULL, 0);
  CHECK(c.centre == 0 && nxinput_axis_normalize(&c, 0) == 0.0f, "signed: centre 0, rest = exact 0.0");
  CHECK(nxinput_axis_normalize(&c, 32767) == 1.0f && nxinput_axis_normalize(&c, -32768) == -1.0f, "signed: both ends saturate to +-1");
  /* unsigned 0..255 centre 128 (NextOS 0810:0001 case) */
  a = (nxinput_axis_absinfo){128, 0, 255, 0, 15, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, NULL, 0);
  CHECK(c.centre == 128 && nxinput_axis_normalize(&c, 128) == 0.0f, "unsigned 0..255: centre 128 is exact 0.0");
  CHECK(nxinput_axis_normalize(&c, 140) == 0.0f, "flat=15: 140 is still exact 0.0 (not movement)");
  CHECK(nxinput_axis_normalize(&c, 255) == 1.0f && nxinput_axis_normalize(&c, 0) == -1.0f, "unsigned: full deflection is +-1 (not < deadzone as V4 /32767 did)");
  /* first sample is NOT the centre */
  a = (nxinput_axis_absinfo){250, 0, 255, 0, 0, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, NULL, 0);
  CHECK(c.centre == 128, "stick held during open: centre stays the midpoint, not `current`");
  /* asymmetric -1800..1800 with fuzz/flat 32 (CubeXX DTS), inverted */
  a = (nxinput_axis_absinfo){3, -1800, 1800, 32, 32, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, NULL, 1);
  CHECK(nxinput_axis_normalize(&c, 3) == 0.0f && nxinput_axis_normalize(&c, 60) == 0.0f, "noisy centre inside flat+fuzz = exact 0.0");
  CHECK(nxinput_axis_normalize(&c, 1800) == -1.0f, "inverted: max reads -1");
  a = (nxinput_axis_absinfo){0, -100, 300, 0, 0, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, NULL, 0);
  CHECK(c.centre == 100 && nxinput_axis_normalize(&c, 300) == 1.0f && nxinput_axis_normalize(&c, -100) == -1.0f && nxinput_axis_normalize(&c, 200) == 0.5f, "asymmetric range: each side scaled separately");
  { int32_t pin = 0; nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, &pin, 0); CHECK(c.centre == 0 && c.centre_source == 1, "pinned centre from descriptor wins"); }
  /* trigger unilateral 0..255 */
  a = (nxinput_axis_absinfo){0, 0, 255, 0, 0, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_TRIGGER, &a, NULL, 0);
  CHECK(nxinput_axis_normalize(&c, 0) == 0.0f && nxinput_axis_normalize(&c, 255) == 1.0f && nxinput_axis_normalize(&c, 128) > 0.49f, "trigger: baseline min, [0,1], never centred");
  /* signed trigger -32768..32767 presented by some HID pads: baseline min */
  a = (nxinput_axis_absinfo){-32768, -32768, 32767, 0, 0, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_TRIGGER, &a, NULL, 0);
  CHECK(nxinput_axis_normalize(&c, -32768) == 0.0f && nxinput_axis_normalize(&c, 32767) == 1.0f, "signed trigger: rest at min = 0.0, not 0.5");
  /* hat */
  a = (nxinput_axis_absinfo){0, -1, 1, 0, 0, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_HAT, &a, NULL, 0);
  CHECK(nxinput_axis_normalize(&c, -1) == -1.0f && nxinput_axis_normalize(&c, 0) == 0.0f, "hat exact -1/0/+1");
  /* quarantine: flat covers the range */
  a = (nxinput_axis_absinfo){0, -10, 10, 0, 10, 0};
  nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &a, NULL, 0);
  CHECK(c.quarantined == 1 && nxinput_axis_normalize(&c, 10) == 0.0f, "no neutral window: quarantined, emits 0");
  CHECK(nxinput_axis_calib_init(&c, NXINPUT_AXIS_STICK, &(nxinput_axis_absinfo){0, 5, 5, 0, 0, 0}, NULL, 0) == -1, "degenerate range refused");
  /* radial deadzone */
  { float x, y; nxinput_axis_radial(0.1f, 0.1f, 0.2f, &x, &y); CHECK(x == 0.0f && y == 0.0f, "radial: inside d => exactly (0,0)");
    nxinput_axis_radial(1.0f, 0.0f, 0.2f, &x, &y); CHECK(x == 1.0f && y == 0.0f, "radial: full deflection stays 1");
    nxinput_axis_radial(0.6f, 0.0f, 0.2f, &x, &y); CHECK(x > 0.49f && x < 0.51f, "radial: (r-d)/(1-d) rescale");
    CHECK(nxinput_axis_radial(0, 0, 0.95f, &x, &y) == -1 && nxinput_axis_radial(0, 0, -0.1f, &x, &y) == -1, "radial: d outside [0,0.95) refused"); }
  /* digital directions: 8way */
  { nxinput_stick_digital s;
    CHECK(nxinput_stick_digital_init(&s, 0.40f, 0.55f, 1, 1) == -1, "inverted thresholds refused");
    nxinput_stick_digital_init(&s, 0.55f, 0.40f, 1, 1);
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.6f, 0.0f, edge, NULL); CHECK(strcmp(trace, "R+ ") == 0, "8way: right enters at 0.6");
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.45f, 0.0f, edge, NULL); CHECK(strcmp(trace, "") == 0, "8way: hysteresis holds at 0.45");
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.3f, 0.0f, edge, NULL); CHECK(strcmp(trace, "R- ") == 0, "8way: exits below 0.40");
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.7f, -0.7f, edge, NULL); CHECK(strcmp(trace, "R+ U+ ") == 0, "8way: diagonal = two directions");
    trace[0] = 0; nxinput_stick_digital_update(&s, -0.9f, -0.7f, edge, NULL); CHECK(strcmp(trace, "R- L+ ") == 0, "reversal: release old before press new; opposites never coexist");
    trace[0] = 0; nxinput_stick_digital_release_all(&s, edge, NULL); CHECK(strcmp(trace, "L- U- ") == 0, "release-all drops every held direction");
    /* 4way-dominant */
    nxinput_stick_digital_init(&s, 0.55f, 0.40f, 0, 1);
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.6f, 0.6f, edge, NULL); CHECK(strcmp(trace, "R+ ") == 0, "4way: perfect tie from neutral -> tie_break horizontal");
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.6f, 0.9f, edge, NULL); CHECK(strcmp(trace, "") == 0, "4way: active axis kept while above threshold (no flip)");
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.2f, 0.9f, edge, NULL); CHECK(strcmp(trace, "R- D+ ") == 0, "4way: horizontal exits, vertical takes over");
    nxinput_stick_digital_init(&s, 0.55f, 0.40f, 0, 0);
    trace[0] = 0; nxinput_stick_digital_update(&s, 0.6f, 0.6f, edge, NULL); CHECK(strcmp(trace, "D+ ") == 0, "4way: tie_break vertical"); }
  { nxinput_trigger_digital t; nxinput_trigger_digital_init(&t, 0.55f, 0.40f);
    CHECK(nxinput_trigger_digital_update(&t, 0.5f) == 0 && nxinput_trigger_digital_update(&t, 0.6f) == 1 && nxinput_trigger_digital_update(&t, 0.45f) == 0 && nxinput_trigger_digital_update(&t, 0.3f) == -1, "trigger digital edge with hysteresis");
    CHECK(nxinput_trigger_digital_init(&t, 1.2f, 0.4f) == -1, "trigger thresholds outside [0,1] refused"); }
  printf(fails ? "v5-axis-calib: FAIL\n" : "v5-axis-calib: OK\n");
  { float ox, oy; /* 0.11.8 axial gate (K36S: full LEFT reads y=+0.28 -> engine crouch) */
    CHECK(nxinput_axis_axial(-0.96f, 0.28f, 0.5f, &ox, &oy) == 0 && ox == -0.96f && oy == 0.0f, "axial: minor y below floor is zeroed, x whole");
    CHECK(nxinput_axis_axial(0.13f, 0.99f, 0.5f, &ox, &oy) == 0 && ox == 0.0f && oy == 0.99f, "axial: minor x below floor is zeroed, y whole");
    CHECK(nxinput_axis_axial(0.7f, -0.7f, 0.5f, &ox, &oy) == 0 && ox == 0.7f && oy == -0.7f, "axial: real diagonal (both >= floor) passes whole");
    CHECK(nxinput_axis_axial(0.3f, 0.3f, 0.5f, &ox, &oy) == 0 && ox == 0.3f && oy == 0.3f, "axial: equal magnitudes are not minor (no arbitrary zeroing)");
    CHECK(nxinput_axis_axial(0.0f, 0.0f, 0.5f, &ox, &oy) == 0 && ox == 0.0f && oy == 0.0f, "axial: rest stays exact 0,0");
    CHECK(nxinput_axis_axial(0.5f, 0.2f, 1.0f, &ox, &oy) == -1, "axial: floor outside [0,1) refused"); }
  return fails ? 1 : 0;
}
