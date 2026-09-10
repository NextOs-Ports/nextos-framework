/* SPDX-License-Identifier: MIT */
#include "kotor_language.h"

#include <assert.h>
#include <stddef.h>

int main(void) {
  assert(kotor_language_id_from_code(NULL) == 0);
  assert(kotor_language_id_from_code("") == 0);
  assert(kotor_language_id_from_code("en") == 0);
  assert(kotor_language_id_from_code("fr") == 1);
  assert(kotor_language_id_from_code("it") == 2);
  assert(kotor_language_id_from_code("de") == 3);
  assert(kotor_language_id_from_code("es") == 4);
  assert(kotor_language_id_from_code("pl") == 5);
  assert(kotor_language_id_from_code("pt") == 0);
  assert(kotor_language_id_from_code("$(id)") == 0);
  return 0;
}
