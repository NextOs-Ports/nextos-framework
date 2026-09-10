/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_AXIS_CALIB_H
#define NXINPUT_AXIS_CALIB_H
/*
 * nxinput_axis_calib -- V5 (0.11.0, mission 5.5): ONE normalization of an
 * evdev axis from its EVIOCGABS calibration to [-1,+1] with an EXACT 0.0 at
 * rest, and the radial deadzone / digital-direction derivation of 6.5.
 *
 * - Sticks: centre from the descriptor (pinned) or the RANGE MIDPOINT, never
 *   the first sample (the user may be holding the stick during open). The
 *   two half-ranges are divided separately by (max-centre) and (centre-min),
 *   saturated, and a neutral band of flat+fuzz forces exactly 0.0.
 * - Triggers: unilateral, baseline = min, output [0,1]; never centred.
 * - Hats: -1/0/+1 exact.
 * - raw, calibrated and normalized are all kept for the receipt so a double
 *   normalization is detectable.
 *
 * Pure: no I/O. The caller fills nxinput_axis_absinfo from EVIOCGABS.
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct nxinput_axis_absinfo {
  int32_t value;      /* current at open: recorded, NOT used as centre */
  int32_t minimum, maximum, fuzz, flat, resolution;
} nxinput_axis_absinfo;

typedef enum nxinput_axis_kind {
  NXINPUT_AXIS_STICK = 0,   /* centred, [-1,1] */
  NXINPUT_AXIS_TRIGGER,     /* unilateral, [0,1] */
  NXINPUT_AXIS_HAT          /* -1/0/+1 */
} nxinput_axis_kind;

typedef struct nxinput_axis_calib {
  uint8_t kind;             /* nxinput_axis_kind */
  uint8_t inverted;         /* mapping '~' */
  uint8_t centre_source;    /* 0 midpoint, 1 pinned descriptor */
  uint8_t quarantined;      /* no neutral window exists: do not use */
  int32_t minimum, maximum, centre, flat, fuzz;
  float neutral_band;       /* normalized half-width of the exact-zero band */
} nxinput_axis_calib;

/* Build the calibration. `pinned_centre` may be NULL (=> midpoint). Returns
 * -1 on a degenerate range (min >= max), 0 otherwise. A stick whose neutral
 * band would exceed 0.95 of the range is quarantined. */
int nxinput_axis_calib_init(nxinput_axis_calib *c, nxinput_axis_kind kind,
                            const nxinput_axis_absinfo *info,
                            const int32_t *pinned_centre, int inverted);

/* raw -> normalized. Exact 0.0f inside the neutral band; saturated to
 * [-1,1] (stick) or [0,1] (trigger). */
float nxinput_axis_normalize(const nxinput_axis_calib *c, int32_t raw);

/* 6.5 radial deadzone on an already-normalized (x,y): r<=d => (0,0) exactly,
 * else rescaled (r-d)/(1-d) along the same direction, saturated. `d` must be
 * in [0,0.95). Returns -1 for an invalid d. */
int nxinput_axis_radial(float x, float y, float d, float *ox, float *oy);

/* 0.11.8 axial gate on an already-normalized (x,y): the MINOR component (the
 * one with the smaller magnitude) is zeroed while it stays below `floor`;
 * the dominant component always passes; a true diagonal (both >= floor)
 * passes whole. Field (Nameless Cat 1.2.8, an ADC stick, 04/09): a full push
 * LEFT reads y = +0.28 on that stick and the engine treats "axis 2 > ~0.25"
 * as crouch, so the cat stopped "out of nowhere" and only to the left; the
 * D-pad never had a minor component. `floor` must be in [0,1). Returns -1 for
 * an invalid floor. */
int nxinput_axis_axial(float x, float y, float floor_, float *ox, float *oy);

/* Digital direction machine (per stick), 6.5. */
typedef struct nxinput_stick_digital {
  float enter, exit;        /* Schmitt thresholds, 0 <= exit < enter <= 1 */
  uint8_t eight_way;        /* 1 = 8way (independent axes), 0 = 4way-dominant */
  uint8_t tie_horizontal;   /* 4way tie-break from neutral */
  uint8_t up, down, left, right;   /* current state */
  uint8_t dominant;         /* 0 none, 1 horizontal, 2 vertical (4way) */
} nxinput_stick_digital;

int nxinput_stick_digital_init(nxinput_stick_digital *s, float enter,
                               float exit, int eight_way, int tie_horizontal);
/* Feed one normalized (post-radial) vector. Emits release BEFORE press on
 * reversal; opposites never coexist. Callback order is deterministic. */
typedef void (*nxinput_stick_edge_fn)(void *user, int direction /*0 up 1 down 2 left 3 right*/, int pressed);
void nxinput_stick_digital_update(nxinput_stick_digital *s, float x, float y,
                                  nxinput_stick_edge_fn edge, void *user);
void nxinput_stick_digital_release_all(nxinput_stick_digital *s,
                                       nxinput_stick_edge_fn edge, void *user);

/* Trigger digital edge with hysteresis (6.5 triggers). */
typedef struct nxinput_trigger_digital { float enter, exit; uint8_t down; } nxinput_trigger_digital;
int nxinput_trigger_digital_init(nxinput_trigger_digital *t, float enter, float exit);
/* Returns +1 on press edge, -1 on release edge, 0 no change. */
int nxinput_trigger_digital_update(nxinput_trigger_digital *t, float v01);
#ifdef __cplusplus
}
#endif
#endif
