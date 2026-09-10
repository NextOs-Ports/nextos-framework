/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixture provider exporting the anchor and only SDL_JoystickGetVendor, to
 * prove each optional symbol is independent.  Test double only. */
#include <stdint.h>

#define FAKE_PARTIAL_VENDOR 0x057eu

typedef struct fake_sdl_version {
  uint8_t major;
  uint8_t minor;
  uint8_t patch;
} fake_sdl_version;

void SDL_GetVersion(fake_sdl_version *version);
uint16_t SDL_JoystickGetVendor(void *joystick);

void SDL_GetVersion(fake_sdl_version *version) {
  if (!version)
    return;
  version->major = 2;
  version->minor = 0;
  version->patch = 6;
}

uint16_t SDL_JoystickGetVendor(void *joystick) {
  return joystick ? FAKE_PARTIAL_VENDOR : 0;
}
