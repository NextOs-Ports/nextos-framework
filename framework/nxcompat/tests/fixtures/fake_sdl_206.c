/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixture provider equivalent to SDL 2.0.6+: it exports the anchor plus both
 * optional joystick metadata symbols with distinctive values.  It is a test
 * double only and never ships. */
#include <stdint.h>

#define FAKE206_VENDOR 0x1209u
#define FAKE206_PRODUCT 0x4004u

typedef struct fake_sdl_version {
  uint8_t major;
  uint8_t minor;
  uint8_t patch;
} fake_sdl_version;

void SDL_GetVersion(fake_sdl_version *version);
uint16_t SDL_JoystickGetVendor(void *joystick);
uint16_t SDL_JoystickGetProduct(void *joystick);

void SDL_GetVersion(fake_sdl_version *version) {
  if (!version)
    return;
  version->major = 2;
  version->minor = 0;
  version->patch = 6;
}

uint16_t SDL_JoystickGetVendor(void *joystick) {
  return joystick ? FAKE206_VENDOR : 0;
}

uint16_t SDL_JoystickGetProduct(void *joystick) {
  return joystick ? FAKE206_PRODUCT : 0;
}
