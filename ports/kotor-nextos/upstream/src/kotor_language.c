/* SPDX-License-Identifier: MIT */
#include "kotor_language.h"

#include <stddef.h>
#include <string.h>

typedef struct kotor_language_entry {
  const char *code;
  int native_id;
} kotor_language_entry;

int kotor_language_id_from_code(const char *code) {
  static const kotor_language_entry languages[] = {
      {"en", 0}, {"fr", 1}, {"it", 2},
      {"de", 3}, {"es", 4}, {"pl", 5},
  };

  if (!code || !*code)
    return 0;
  for (size_t index = 0; index < sizeof(languages) / sizeof(languages[0]);
       ++index) {
    if (strcmp(code, languages[index].code) == 0)
      return languages[index].native_id;
  }
  return 0;
}
