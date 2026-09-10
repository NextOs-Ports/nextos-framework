/* SPDX-License-Identifier: GPL-3.0-only */
#include "chrono_language.h"

#include <stdlib.h>
#include <string.h>

static int chrono_japanese_selected(void) {
  const char *language = getenv("NXPORT_LANGUAGE");
  return language != NULL && strcmp(language, "ja") == 0;
}

int chrono_forced_lang(void) {
  return chrono_japanese_selected() ? 0 : 1;
}

int chrono_location_code(void) {
  return chrono_japanese_selected() ? 0 : 1;
}
