/* SPDX-License-Identifier: GPL-3.0-only */
#include "chrono_language.h"

#include <assert.h>
#include <stdlib.h>

int main(void) {
  unsetenv("NXPORT_LANGUAGE");
  assert(chrono_forced_lang() == 1);
  assert(chrono_location_code() == 1);

  setenv("NXPORT_LANGUAGE", "en", 1);
  assert(chrono_forced_lang() == 1);
  assert(chrono_location_code() == 1);

  setenv("NXPORT_LANGUAGE", "ja", 1);
  assert(chrono_forced_lang() == 0);
  assert(chrono_location_code() == 0);

  setenv("NXPORT_LANGUAGE", "$(id)", 1);
  assert(chrono_forced_lang() == 1);
  assert(chrono_location_code() == 1);
  return 0;
}
