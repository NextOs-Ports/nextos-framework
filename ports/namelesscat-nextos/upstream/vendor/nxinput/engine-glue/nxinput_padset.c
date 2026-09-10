/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_padset -- see nxinput_padset.h. No SDL header: every call goes
 * through the caller's vtable, so this file compiles and is tested anywhere. */
#include "nxinput_padset.h"

#include <string.h>

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
  return 0;
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
  for (int i = 0; i < n && set->count < NXINPUT_PADSET_MAX; i++) {
    int32_t instance = set->sdl->instance_for_index(i);
    if (slot_of_instance(set, instance) >= 0)
      continue; /* already open */
    if (admit && !admit(i, user))
      continue;
    if (!set->sdl->is_game_controller(i))
      continue;
    void *controller = set->sdl->open(i);
    if (!controller)
      continue;
    void *joy = set->sdl->get_joystick(controller);
    set->pads[set->count] = controller;
    set->instances[set->count] = joy ? set->sdl->joystick_instance(joy) : instance;
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
  for (unsigned p = (unsigned)slot; p + 1 < set->count; p++) {
    set->pads[p] = set->pads[p + 1];
    set->instances[p] = set->instances[p + 1];
  }
  set->count--;
  set->pads[set->count] = NULL;
  set->instances[set->count] = 0;
  return 1;
}

void nxinput_padset_close_all(nxinput_padset *set)
{
  if (!set)
    return;
  for (unsigned p = 0; p < set->count; p++)
    if (set->pads[p])
      set->sdl->close(set->pads[p]);
  memset(set->pads, 0, sizeof set->pads);
  memset(set->instances, 0, sizeof set->instances);
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
  if (set->count)
    set->sdl->update();
  for (unsigned p = 0; p < set->count; p++) {
    int sel = 0, start = 0;
    for (int b = 0; b < (int)NXINPUT_PADSET_BUTTON_MAX; b++) {
      int down = set->sdl->get_button(set->pads[p], b) ? 1 : 0;
      if (down)
        set->buttons[b] = 1;
      if (b == NXINPUT_PADSET_BUTTON_BACK)
        sel = down;
      if (b == NXINPUT_PADSET_BUTTON_START)
        start = down;
    }
    if (sel && start)
      set->chord_same_instance = 1;
    any_select |= sel;
    any_start |= start;
  }
  if (!set->chord_same_instance && any_select && any_start) {
    set->chord_cross_pad = 1;
    if (!set->cross_pad_logged) {
      if (set->log)
        set->log("chord denied: SELECT and START on different pads (cross-pad)",
                 set->log_user);
      set->cross_pad_logged = 1;
    }
  } else if (!(any_select && any_start)) {
    set->cross_pad_logged = 0;
  }
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
