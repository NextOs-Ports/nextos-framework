/* SPDX-License-Identifier: GPL-3.0-only */
/* Negative audit control: this executable DOES import
 * SDL_JoystickGetVendor directly (an SDL 2.0.6 symbol), which is exactly
 * what a universal ELF may never do.  run-sdl-optional-audit.sh requires the
 * nxabi sdl-floor gate to flag it; it is never executed and never ships. */
#include <stdint.h>

extern uint16_t SDL_JoystickGetVendor(void *joystick);

int main(void) {
  int cookie = 0;
  return SDL_JoystickGetVendor(&cookie) == 0xffffu;
}
