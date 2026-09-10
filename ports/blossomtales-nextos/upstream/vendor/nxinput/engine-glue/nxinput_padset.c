/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_padset -- see nxinput_padset.h. No SDL header: every call goes
 * through the caller's vtable, so this file compiles and is tested anywhere. */
/* clock_gettime/CLOCK_MONOTONIC need the POSIX feature test under -std=c99;
 * the file declares it itself so it really does compile anywhere, instead of
 * depending on the flags of whichever harness happens to build it. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxinput_padset.h"
#include "nxinput_axis_calib.h"

#include <string.h>
#include <time.h>

static uint64_t padset_monotonic_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0u;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int slot_of_instance(const nxinput_padset *set, int32_t instance);
static void elect_primary(nxinput_padset *set);

/* pre-router sinks: a forwarded edge reaches the union; a retained one is masked */
static void padset_pre_forward(void *user, int instance_id, uint32_t generation,
                               nxinput_prerouter_edge edge, int pressed, uint64_t edge_id)
{
  nxinput_padset *set = user;
  int slot = slot_of_instance(set, instance_id);
  (void)generation; (void)edge_id;
  if (slot < 0)
    return;
  if (edge == NXINPUT_PREROUTER_SELECT)
    set->fwd_select[slot] = (uint8_t)(pressed ? 1u : 0u);
  else
    set->fwd_start[slot] = (uint8_t)(pressed ? 1u : 0u);
}

static void padset_pre_exit(void *user, int instance_id, uint32_t generation, uint64_t edge_id)
{
  nxinput_padset *set = user;
  (void)generation; (void)edge_id;
  set->exit_pending = 1;
  set->exit_instance = instance_id;
  set->exits++;
}

static const char padset_marker[] = NXINPUT_PADSET_MARKER;

const char *nxinput_padset_marker(void)
{
  return padset_marker;
}

int nxinput_padset_init(nxinput_padset *set, const nxinput_padset_sdl *sdl,
                        nxinput_padset_log_fn log, void *log_user)
{
  if (!set || !sdl || !sdl->num_joysticks || !sdl->instance_for_index ||
      !sdl->is_game_controller || !sdl->open || !sdl->close ||
      !sdl->get_joystick || !sdl->joystick_instance || !sdl->update ||
      !sdl->get_button || !sdl->get_axis)
    return -1;
  memset(set, 0, sizeof *set);
  set->sdl = sdl;
  set->log = log;
  set->log_user = log_user;
  set->clock = padset_monotonic_ns;
  set->next_generation = 1u;
  {
    nxinput_prerouter_ops ops;
    ops.user = set;
    ops.forward = padset_pre_forward;
    ops.system_exit = padset_pre_exit;
    if (nxinput_prerouter_init(&set->pre, &ops, 0u) != 0)
      return -1;
  }
  return 0;
}

void nxinput_padset_set_clock(nxinput_padset *set, nxinput_padset_clock_fn clock)
{
  if (set)
    set->clock = clock ? clock : padset_monotonic_ns;
}

int nxinput_padset_exit_requested(const nxinput_padset *set)
{
  return set ? set->exit_pending : 0;
}

unsigned nxinput_padset_instance_races(const nxinput_padset *set)
{
  return set ? set->instance_races : 0u;
}

static int slot_of_instance(const nxinput_padset *set, int32_t instance)
{
  for (unsigned p = 0; p < set->count; p++)
    if (set->instances[p] == instance)
      return (int)p;
  return -1;
}

unsigned nxinput_padset_open_all(nxinput_padset *set,
                                 nxinput_padset_admit_fn admit,
                                 nxinput_padset_opened_fn opened, void *user)
{
  unsigned added = 0;
  if (!set || !set->sdl)
    return 0;
  int n = set->sdl->num_joysticks();
  for (int i = 0; i < n; i++) {
    int32_t instance = set->sdl->instance_for_index(i);
    if (slot_of_instance(set, instance) >= 0)
      continue; /* already open */
    if (admit && !admit(i, user))
      continue;
    if (!set->sdl->is_game_controller(i))
      continue;
    if (set->count >= NXINPUT_PADSET_MAX) {
      /* V5 (D3): a visible refusal, never a silent truncation. */
      set->overflowed++;
      if (!set->overflow_logged && set->log) {
        set->log("padset full: additional pad refused (limit reached); see overflowed count", set->log_user);
        set->overflow_logged = 1;
      }
      continue;
    }
    void *controller = set->sdl->open(i);
    if (!controller)
      continue;
    void *joy = set->sdl->get_joystick(controller);
    int32_t opened_instance = joy ? set->sdl->joystick_instance(joy) : instance;
    if (opened_instance != instance) {
      /* 0.11.1 (C5, review finding 11): the index was re-used by another
       * device between admission and open. The admission decided about
       * `instance`; this pad is NOT it. Close it, count the race, and let
       * the next open_all admit whatever is really there. */
      set->sdl->close(controller);
      set->instance_races++;
      if (set->log)
        set->log("instance race: opened pad is not the admitted instance; closed, re-admission required", set->log_user);
      continue;
    }
    set->pads[set->count] = controller;
    set->instances[set->count] = opened_instance;
    set->generations[set->count] = set->next_generation++;
    set->raw_select[set->count] = set->raw_start[set->count] = 0u;
    set->fwd_select[set->count] = set->fwd_start[set->count] = 0u;
    set->count++;
    added++;
    if (opened)
      opened(i, set->count - 1, controller, user);
  }
  return added;
}

int nxinput_padset_remove_instance(nxinput_padset *set, int32_t instance)
{
  if (!set)
    return 0;
  int slot = slot_of_instance(set, instance);
  if (slot < 0)
    return 0;
  set->sdl->close(set->pads[slot]);
  nxinput_prerouter_unplug(&set->pre, instance, set->clock());
  if (set->exit_pending && set->exit_instance == instance)
    set->exit_pending = 0;
  if (set->primary_explicit && set->primary_instance == instance)
    set->primary_explicit = 0; /* V5: the next primary is the first admitted */
  for (unsigned p = (unsigned)slot; p + 1 < set->count; p++) {
    set->pads[p] = set->pads[p + 1];
    set->instances[p] = set->instances[p + 1];
    set->generations[p] = set->generations[p + 1];
    set->raw_select[p] = set->raw_select[p + 1];
    set->raw_start[p] = set->raw_start[p + 1];
    set->fwd_select[p] = set->fwd_select[p + 1];
    set->fwd_start[p] = set->fwd_start[p + 1];
  }
  set->count--;
  set->pads[set->count] = NULL;
  set->instances[set->count] = 0;
  set->generations[set->count] = 0u;
  set->raw_select[set->count] = set->raw_start[set->count] = 0u;
  set->fwd_select[set->count] = set->fwd_start[set->count] = 0u;
  return 1;
}

void nxinput_padset_close_all(nxinput_padset *set)
{
  if (!set)
    return;
  for (unsigned p = 0; p < set->count; p++)
    if (set->pads[p])
      set->sdl->close(set->pads[p]);
  nxinput_prerouter_release_all(&set->pre, set->clock());
  memset(set->pads, 0, sizeof set->pads);
  memset(set->instances, 0, sizeof set->instances);
  memset(set->generations, 0, sizeof set->generations);
  memset(set->raw_select, 0, sizeof set->raw_select);
  memset(set->raw_start, 0, sizeof set->raw_start);
  memset(set->fwd_select, 0, sizeof set->fwd_select);
  memset(set->fwd_start, 0, sizeof set->fwd_start);
  set->exit_pending = 0;
  set->count = 0;
  memset(set->buttons, 0, sizeof set->buttons);
  set->chord_same_instance = 0;
  set->chord_cross_pad = 0;
}

void *nxinput_padset_first(const nxinput_padset *set)
{
  return (set && set->count) ? set->pads[0] : NULL;
}

void nxinput_padset_sample(nxinput_padset *set)
{
  if (!set)
    return;
  memset(set->buttons, 0, sizeof set->buttons);
  set->chord_same_instance = 0;
  set->chord_cross_pad = 0;
  int any_select = 0, any_start = 0;
  uint64_t now = set->clock();
  if (set->count)
    set->sdl->update();
  elect_primary(set); /* 0.11.6 */
  for (unsigned p = 0; p < set->count; p++) {
    int sel = 0, start = 0;
    for (int b = 0; b < (int)NXINPUT_PADSET_BUTTON_MAX; b++) {
      int down = set->sdl->get_button(set->pads[p], b) ? 1 : 0;
      if (b == NXINPUT_PADSET_BUTTON_BACK) {
        sel = down;
        continue; /* decided by the pre-router below */
      }
      if (b == NXINPUT_PADSET_BUTTON_START) {
        start = down;
        continue;
      }
      if (down)
        set->buttons[b] = 1;
    }
    /* 0.11.1: edges of SELECT/START go through the sovereign pre-router
     * (same instance + same generation inside the window = ONE exit; a
     * lone press is forwarded exactly once after the window). */
    if ((sel != 0) != (set->raw_select[p] != 0u))
      nxinput_prerouter_event(&set->pre, set->instances[p], set->generations[p],
                              NXINPUT_PREROUTER_SELECT, sel ? 1 : 0, now);
    if ((start != 0) != (set->raw_start[p] != 0u))
      nxinput_prerouter_event(&set->pre, set->instances[p], set->generations[p],
                              NXINPUT_PREROUTER_START, start ? 1 : 0, now);
    set->raw_select[p] = (uint8_t)(sel ? 1u : 0u);
    set->raw_start[p] = (uint8_t)(start ? 1u : 0u);
    any_select |= sel;
    any_start |= start;
    if (set->exit_pending && set->exit_instance == set->instances[p]) {
      if (sel && start)
        set->chord_same_instance = 1; /* the chord instance still holds both */
      else
        set->exit_pending = 0; /* either released: the chord is over */
    }
  }
  nxinput_prerouter_tick(&set->pre, now);
  for (unsigned p = 0; p < set->count; p++) {
    if (set->fwd_select[p])
      set->buttons[NXINPUT_PADSET_BUTTON_BACK] = 1;
    if (set->fwd_start[p])
      set->buttons[NXINPUT_PADSET_BUTTON_START] = 1;
  }
  if (!set->chord_same_instance && any_select && any_start) {
    /* raw evidence: SELECT here and START there, no instance holding both */
    int same = 0;
    for (unsigned p = 0; p < set->count; p++)
      if (set->raw_select[p] && set->raw_start[p])
        same = 1;
    if (!same) {
      set->chord_cross_pad = 1;
      if (!set->cross_pad_logged) {
        if (set->log)
          set->log("chord denied: SELECT and START on different pads (cross-pad)",
                   set->log_user);
        set->cross_pad_logged = 1;
      }
    }
  } else if (!(any_select && any_start)) {
    set->cross_pad_logged = 0;
  }
}

int nxinput_padset_set_primary(nxinput_padset *set, int32_t instance)
{
  if (!set || slot_of_instance(set, instance) < 0)
    return -1;
  set->primary_instance = instance;
  set->primary_explicit = 1;
  return 0;
}

int32_t nxinput_padset_primary_instance(const nxinput_padset *set)
{
  if (!set || !set->count)
    return 0;
  if (set->primary_explicit && slot_of_instance(set, set->primary_instance) >= 0)
    return set->primary_instance;
  /* 0.11.6: the pad ELECTED by activity in nxinput_padset_sample (the one the
   * human is actually holding); first admitted until anyone moves. */
  if (set->elected_valid && slot_of_instance(set, set->elected_instance) >= 0)
    return set->elected_instance;
  return set->instances[0];
}

/* 0.11.6: is this pad being used right now? Any face/shoulder/dpad button
 * down or any stick/trigger axis beyond a quarter of the range. */
static int pad_active(const nxinput_padset *set, unsigned p)
{
  for (int b = 0; b < (int)NXINPUT_PADSET_BUTTON_MAX; b++)
    if (b != NXINPUT_PADSET_BUTTON_BACK && b != NXINPUT_PADSET_BUTTON_START &&
        set->sdl->get_button(set->pads[p], b))
      return 1;
  for (int a = 0; a < 6; a++) {
    int v = set->sdl->get_axis(set->pads[p], a);
    if (v > 8192 || v < -8192)
      return 1;
  }
  return 0;
}

/* 0.11.6: elect the primary by ACTIVITY. A pad with two nodes (adc-joystick
 * + gpio-keys), or a proof clone admitted after the real pad, is never slot
 * 0; "primary = first admitted" left its stick and triggers dead on the
 * device (FP2 03/09). The vector/trigger stay whole and from ONE instance
 * (D2); only WHICH instance follows the user's hand, with hysteresis: the
 * current primary keeps the seat while it is active, and hands it to the
 * first active pad only once it is at rest. An explicit primary never moves. */
static void elect_primary(nxinput_padset *set)
{
  int cur;
  if (set->primary_explicit || !set->count)
    return;
  cur = set->elected_valid ? slot_of_instance(set, set->elected_instance) : 0;
  if (cur < 0) cur = 0;
  if (pad_active(set, (unsigned)cur))
    return;
  for (unsigned p = 0; p < set->count; p++) {
    if ((int)p != cur && pad_active(set, p)) {
      set->elected_instance = set->instances[p];
      set->elected_valid = 1;
      return;
    }
  }
}

int nxinput_padset_vector(const nxinput_padset *set, int axis_x, int axis_y,
                          int16_t *x, int16_t *y)
{
  int slot;
  if (x) *x = 0;
  if (y) *y = 0;
  if (!set || !set->count)
    return 0;
  slot = slot_of_instance(set, nxinput_padset_primary_instance(set));
  if (slot < 0)
    return 0;
  if (x) *x = set->sdl->get_axis(set->pads[slot], axis_x);
  if (y) *y = set->sdl->get_axis(set->pads[slot], axis_y);
  return 1;
}

int nxinput_padset_vector_norm(const nxinput_padset *set, int axis_x, int axis_y,
                               float deadzone, float *x, float *y)
{
  int16_t rx = 0, ry = 0;
  nxinput_axis_absinfo info;
  nxinput_axis_calib calib;
  float nx, ny;
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (!(deadzone >= 0.0f) || deadzone >= 0.95f)
    return -1;
  if (!nxinput_padset_vector(set, axis_x, axis_y, &rx, &ry))
    return 0;
  /* SDL's Sint16 axis domain: centred, symmetric around 0 (the exact-zero
   * band absorbs the +1 residual of an odd raw range). */
  memset(&info, 0, sizeof info);
  info.minimum = -32768; info.maximum = 32767; info.flat = 0; info.fuzz = 0;
  if (nxinput_axis_calib_init(&calib, NXINPUT_AXIS_STICK, &info, NULL, 0) != 0)
    return -1;
  nx = nxinput_axis_normalize(&calib, rx);
  ny = nxinput_axis_normalize(&calib, ry);
  if (deadzone > 0.0f) {
    float ox = 0.0f, oy = 0.0f;
    if (nxinput_axis_radial(nx, ny, deadzone, &ox, &oy) != 0)
      return -1;
    nx = ox; ny = oy;
  }
  if (x) *x = nx;
  if (y) *y = ny;
  return 1;
}

int nxinput_padset_trigger_norm(const nxinput_padset *set, int axis, float *v01)
{
  int slot;
  int16_t raw;
  nxinput_axis_absinfo info;
  nxinput_axis_calib calib;
  float v;
  if (v01) *v01 = 0.0f;
  if (!set || !v01)
    return -1;
  if (!set->count)
    return 0;
  slot = slot_of_instance(set, nxinput_padset_primary_instance(set));
  if (slot < 0)
    return 0;
  raw = set->sdl->get_axis(set->pads[slot], axis);
  memset(&info, 0, sizeof info);
  info.minimum = 0; info.maximum = 32767; info.flat = 0; info.fuzz = 0;
  if (nxinput_axis_calib_init(&calib, NXINPUT_AXIS_TRIGGER, &info, NULL, 0) != 0)
    return -1;
  v = raw <= 0 ? 0.0f : nxinput_axis_normalize(&calib, raw);
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  *v01 = v;
  return 1;
}

unsigned nxinput_padset_overflowed(const nxinput_padset *set)
{
  return set ? set->overflowed : 0u;
}

int16_t nxinput_padset_axis(const nxinput_padset *set, int axis)
{
  int16_t best = 0;
  if (!set)
    return 0;
  for (unsigned p = 0; p < set->count; p++) {
    int16_t v = set->sdl->get_axis(set->pads[p], axis);
    int mv = v < 0 ? -(int)v : (int)v;
    int mb = best < 0 ? -(int)best : (int)best;
    if (mv > mb)
      best = v;
  }
  return best;
}

void nxinput_padset_chord_inputs(const nxinput_padset *set, int *select_down,
                                 int *start_down)
{
  int on = set && set->chord_same_instance;
  if (select_down)
    *select_down = on;
  if (start_down)
    *start_down = on;
}
