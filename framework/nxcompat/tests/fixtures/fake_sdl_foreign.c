/* SPDX-License-Identifier: GPL-3.0-only */
/* Foreign library that also exports the two optional names but is NOT the
 * provider of the caller's anchor.  The resolver must never select it: a
 * poisoned value from here appearing in a result is a contract violation.
 * Test double only. */
#include <stdint.h>

#define FAKE_FOREIGN_POISON 0xdeadu

uint16_t SDL_JoystickGetVendor(void *joystick);
uint16_t SDL_JoystickGetProduct(void *joystick);
/* Referenced by the fixture executable so the library is really loaded even
 * under --as-needed linking. */
int fake_foreign_present(void);

int fake_foreign_present(void) { return 1; }

uint16_t SDL_JoystickGetVendor(void *joystick) {
  (void)joystick;
  return FAKE_FOREIGN_POISON;
}

uint16_t SDL_JoystickGetProduct(void *joystick) {
  (void)joystick;
  return FAKE_FOREIGN_POISON;
}
