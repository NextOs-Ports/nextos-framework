/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_axis_calib.h"
#include <math.h>
#include <string.h>

int nxinput_axis_calib_init(nxinput_axis_calib *c, nxinput_axis_kind kind,
                            const nxinput_axis_absinfo *info,
                            const int32_t *pinned_centre, int inverted) {
  if (c == NULL || info == NULL) return -1;
  memset(c, 0, sizeof *c);
  if (info->minimum >= info->maximum) return -1;
  c->kind = (uint8_t)kind;
  c->inverted = inverted ? 1u : 0u;
  c->minimum = info->minimum;
  c->maximum = info->maximum;
  c->flat = info->flat < 0 ? 0 : info->flat;
  c->fuzz = info->fuzz < 0 ? 0 : info->fuzz;
  if (kind == NXINPUT_AXIS_TRIGGER) {
    c->centre = info->minimum; /* baseline, not a centre */
    c->neutral_band = (float)(c->flat + c->fuzz) / (float)(c->maximum - c->minimum);
    return 0;
  }
  if (kind == NXINPUT_AXIS_HAT) {
    c->centre = 0;
    return 0;
  }
  if (pinned_centre != NULL && *pinned_centre > info->minimum && *pinned_centre < info->maximum) {
    c->centre = *pinned_centre;
    c->centre_source = 1u;
  } else {
    /* midpoint, rounded toward the kernel's convention (e.g. 0..255 -> 128,
     * -32768..32767 -> 0). */
    c->centre = (int32_t)(((int64_t)info->minimum + (int64_t)info->maximum + 1) / 2);
    c->centre_source = 0u;
  }
  {
    int32_t up = c->maximum - c->centre, down = c->centre - c->minimum;
    int32_t smaller = up < down ? up : down;
    float band = smaller > 0 ? (float)(c->flat + c->fuzz) / (float)smaller : 1.0f;
    c->neutral_band = band;
    if (band >= 0.95f) c->quarantined = 1u;
  }
  return 0;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float nxinput_axis_normalize(const nxinput_axis_calib *c, int32_t raw) {
  float v;
  if (c == NULL || c->quarantined) return 0.0f;
  if (c->kind == NXINPUT_AXIS_HAT) {
    v = raw < 0 ? -1.0f : raw > 0 ? 1.0f : 0.0f;
    return c->inverted ? -v : v;
  }
  if (c->kind == NXINPUT_AXIS_TRIGGER) {
    int32_t span = c->maximum - c->minimum;
    int32_t off = raw - c->minimum;
    if (off <= c->flat + c->fuzz) return 0.0f; /* exact rest */
    v = clampf((float)off / (float)span, 0.0f, 1.0f);
    return c->inverted ? 1.0f - v : v;
  }
  /* stick */
  {
    int32_t delta = raw - c->centre;
    int32_t band = c->flat + c->fuzz;
    if (delta >= -band && delta <= band) return 0.0f; /* EXACT zero */
    if (delta > 0) v = clampf((float)delta / (float)(c->maximum - c->centre), 0.0f, 1.0f);
    else v = -clampf((float)(-delta) / (float)(c->centre - c->minimum), 0.0f, 1.0f);
    return c->inverted ? -v : v;
  }
}

int nxinput_axis_radial(float x, float y, float d, float *ox, float *oy) {
  float r, k;
  if (ox == NULL || oy == NULL || !(d >= 0.0f && d < 0.95f)) return -1;
  x = clampf(x, -1.0f, 1.0f); y = clampf(y, -1.0f, 1.0f);
  r = sqrtf(x * x + y * y);
  if (r > 1.0f) r = 1.0f;
  if (r <= d) { *ox = 0.0f; *oy = 0.0f; return 0; }
  k = ((r - d) / (1.0f - d)) / sqrtf(x * x + y * y);
  *ox = clampf(x * k, -1.0f, 1.0f);
  *oy = clampf(y * k, -1.0f, 1.0f);
  return 0;
}

int nxinput_axis_axial(float x, float y, float floor_, float *ox, float *oy) {
  float ax, ay;
  if (ox == NULL || oy == NULL || !(floor_ >= 0.0f && floor_ < 1.0f)) return -1;
  x = clampf(x, -1.0f, 1.0f); y = clampf(y, -1.0f, 1.0f);
  ax = x < 0 ? -x : x; ay = y < 0 ? -y : y;
  if (ay < ax && ay < floor_) y = 0.0f;
  else if (ax < ay && ax < floor_) x = 0.0f;
  *ox = x; *oy = y;
  return 0;
}

int nxinput_stick_digital_init(nxinput_stick_digital *s, float enter, float exit, int eight_way, int tie_horizontal) {
  if (s == NULL || !(exit >= 0.0f && exit < enter && enter <= 1.0f)) return -1;
  memset(s, 0, sizeof *s);
  s->enter = enter; s->exit = exit; s->eight_way = eight_way ? 1u : 0u; s->tie_horizontal = tie_horizontal ? 1u : 0u;
  return 0;
}

/* Schmitt on one signed component: returns -1/0/+1 wanted state. */
static int schmitt(float v, int cur /* -1,0,1 */, float enter, float exit) {
  float a = v < 0 ? -v : v;
  if (cur == 0) return a >= enter ? (v > 0 ? 1 : -1) : 0;
  if ((cur > 0 && v < 0) || (cur < 0 && v > 0)) return a >= enter ? (v > 0 ? 1 : -1) : 0; /* reversal */
  return a > exit ? cur : 0;
}

static void apply(nxinput_stick_digital *s, int h, int v, nxinput_stick_edge_fn edge, void *user) {
  /* releases first, then presses (opposites never coexist). */
  int wl = h < 0, wr = h > 0, wu = v < 0, wd = v > 0;
  if (s->left && !wl) { s->left = 0; if (edge) edge(user, 2, 0); }
  if (s->right && !wr) { s->right = 0; if (edge) edge(user, 3, 0); }
  if (s->up && !wu) { s->up = 0; if (edge) edge(user, 0, 0); }
  if (s->down && !wd) { s->down = 0; if (edge) edge(user, 1, 0); }
  if (wl && !s->left) { s->left = 1; if (edge) edge(user, 2, 1); }
  if (wr && !s->right) { s->right = 1; if (edge) edge(user, 3, 1); }
  if (wu && !s->up) { s->up = 1; if (edge) edge(user, 0, 1); }
  if (wd && !s->down) { s->down = 1; if (edge) edge(user, 1, 1); }
}

void nxinput_stick_digital_update(nxinput_stick_digital *s, float x, float y, nxinput_stick_edge_fn edge, void *user) {
  int ch = s->right ? 1 : s->left ? -1 : 0;
  int cv = s->down ? 1 : s->up ? -1 : 0;
  int h = schmitt(x, ch, s->enter, s->exit);
  int v = schmitt(y, cv, s->enter, s->exit);
  if (!s->eight_way) {
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (h && v) {
      /* keep the already-active axis while above threshold */
      if (s->dominant == 1 && ch) v = 0;
      else if (s->dominant == 2 && cv) h = 0;
      else if (ax > ay) v = 0;
      else if (ay > ax) h = 0;
      else if (s->tie_horizontal) v = 0; else h = 0;
    }
    s->dominant = h ? 1 : v ? 2 : 0;
  }
  apply(s, h, v, edge, user);
}

void nxinput_stick_digital_release_all(nxinput_stick_digital *s, nxinput_stick_edge_fn edge, void *user) {
  if (s == NULL) return;
  apply(s, 0, 0, edge, user);
  s->dominant = 0;
}

int nxinput_trigger_digital_init(nxinput_trigger_digital *t, float enter, float exit) {
  if (t == NULL || !(exit >= 0.0f && exit < enter && enter <= 1.0f)) return -1;
  t->enter = enter; t->exit = exit; t->down = 0; return 0;
}
int nxinput_trigger_digital_update(nxinput_trigger_digital *t, float v) {
  if (!t->down && v >= t->enter) { t->down = 1; return 1; }
  if (t->down && v < t->exit) { t->down = 0; return -1; }
  return 0;
}
