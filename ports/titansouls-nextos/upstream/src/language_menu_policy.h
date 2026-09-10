/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TITANSOULS_LANGUAGE_MENU_POLICY_H
#define TITANSOULS_LANGUAGE_MENU_POLICY_H

#include <stddef.h>

typedef struct ts_language_choice {
  const char *engine_key;
  const char *menu_label;
  int ag_locale;
} ts_language_choice;

size_t ts_language_choice_count(void);
const ts_language_choice *ts_language_choice_at(size_t index);
int ts_language_choice_index(const char *engine_key);
int ts_language_system_locale(const char *locale_name);

#endif /* TITANSOULS_LANGUAGE_MENU_POLICY_H */
