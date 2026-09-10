/* SELECT+START exit chord read from raw evdev, as a fallback for pads whose
 * SDL mapping omits BACK/START or when SDL loses focus.
 *
 * Since 1.1.3 this is the framework's nxinput_evdev_chord.h: the physical
 * SELECT/START key codes are derived from the SDL_GameController BACK/START
 * binds of the pad nxinput opened, and only without a bind do the raw
 * heuristics (TRIGGER_HAPPY1/2, BTN_SELECT/START, BTN_BASE3/4) apply. Reading
 * BTN_SELECT/BTN_START literally closed the game on L2+R2 on pad drivers that
 * publish those codes for the triggers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>

#include "ab_port.h"
#define NXINPUT_EVDEV_CHORD_LOG(...) ab_log(__VA_ARGS__)
#define NXINPUT_EVDEV_CHORD_IMPLEMENTATION
#include "nxinput_evdev_chord.h"
#include "nxinput.h"

#include "ab_evdev_exit.h"

#define AB_EVDEV_REBIND_MS 2000u

static int g_initialized;
static uint32_t g_next_rebind;
static SDL_GameController *g_bound_controller;

static void rebind(uint32_t now) {
  nxinput_context *input = ab_input_context();
  int slot = input ? nxinput_first_connected(input) : -1;
  SDL_GameController *controller =
      slot >= 0 ? nxinput_pad_sdl_controller(input, (unsigned int)slot) : NULL;
  if (controller && controller != g_bound_controller) {
    nx_evdev_chord_bind_sdl(controller);
    g_bound_controller = controller;
  } else if (!controller) {
    g_bound_controller = NULL;
  }
  g_next_rebind = now + AB_EVDEV_REBIND_MS;
}

void ab_evdev_exit_init(uint32_t now) {
  if (g_initialized)
    return;
  g_initialized = 1;
  nx_evdev_chord_open();
  rebind(now);
}

int ab_evdev_exit_poll(uint32_t now) {
  if (!g_initialized)
    ab_evdev_exit_init(now);
  if ((int32_t)(now - g_next_rebind) >= 0)
    rebind(now);
  return nx_evdev_chord_poll();
}

void ab_evdev_exit_shutdown(void) {
  if (!g_initialized)
    return;
  nx_evdev_chord_close();
  g_bound_controller = NULL;
  g_initialized = 0;
  g_next_rebind = 0;
}
