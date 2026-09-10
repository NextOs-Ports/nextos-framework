/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixture provider equivalent to the universal SDL 2.0.4 floor: it exports a
 * baseline anchor symbol but neither SDL_JoystickGetVendor nor
 * SDL_JoystickGetProduct.  It is a test double only and never ships. */
#include <stdint.h>

typedef struct fake_sdl_version {
  uint8_t major;
  uint8_t minor;
  uint8_t patch;
} fake_sdl_version;

void SDL_GetVersion(fake_sdl_version *version);

void SDL_GetVersion(fake_sdl_version *version) {
  if (!version)
    return;
  version->major = 2;
  version->minor = 0;
  version->patch = 4;
}
