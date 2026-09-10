/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>

#include "language_menu_policy.h"

static int check_choice(size_t index, const char *key, int locale) {
  const ts_language_choice *choice = ts_language_choice_at(index);
  if (!choice || strcmp(choice->engine_key, key) != 0 ||
      choice->ag_locale != locale ||
      ts_language_choice_index(key) != (int)index) {
    fprintf(stderr, "choice %lu divergiu\n", (unsigned long)index);
    return 1;
  }
  return 0;
}

int main(void) {
  if (ts_language_choice_count() != 7u ||
      check_choice(0u, "systemlang", 4) ||
      check_choice(1u, "english", 4) ||
      check_choice(2u, "french", 3) ||
      check_choice(3u, "german", 5) ||
      check_choice(4u, "portuguese", 9) ||
      check_choice(5u, "spanish", 6) ||
      check_choice(6u, "titan", 4) ||
      ts_language_choice_index("italian") != -1 ||
      ts_language_system_locale("pt_BR.UTF-8") != 9 ||
      ts_language_system_locale("es_ES") != 6 ||
      ts_language_system_locale("de_DE") != 5 ||
      ts_language_system_locale("fr_FR") != 3 ||
      ts_language_system_locale("C") != 4) {
    fprintf(stderr, "language menu policy: FAIL\n");
    return 1;
  }
  puts("language menu policy: PASS");
  return 0;
}
