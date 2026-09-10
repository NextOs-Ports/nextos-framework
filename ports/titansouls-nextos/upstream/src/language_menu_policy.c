/* SPDX-License-Identifier: GPL-3.0-only */
#include "language_menu_policy.h"

#include <ctype.h>
#include <string.h>

/* Valores observados em AgLocale::getCurrentSystemLanguage e consumidos pelo
 * switch de Language::ChangeLanguage deste guest Android 1.0.3. */
enum {
  TS_AG_LOCALE_FRENCH = 3,
  TS_AG_LOCALE_ENGLISH = 4,
  TS_AG_LOCALE_GERMAN = 5,
  TS_AG_LOCALE_SPANISH = 6,
  TS_AG_LOCALE_PORTUGUESE = 9,
};

/* Os dois extremos preservam as escolhas originais do Android. As cinco
 * entradas centrais apenas tornam selecionaveis as colunas ja existentes no
 * gametext.xml; nenhuma traducao e inventada pelo port. */
static const ts_language_choice g_choices[] = {
    {"systemlang", NULL, TS_AG_LOCALE_ENGLISH},
    {"english", "ENGLISH", TS_AG_LOCALE_ENGLISH},
    {"french", "FRANÇAIS", TS_AG_LOCALE_FRENCH},
    {"german", "DEUTSCH", TS_AG_LOCALE_GERMAN},
    {"portuguese", "PORTUGUÊS", TS_AG_LOCALE_PORTUGUESE},
    {"spanish", "ESPAÑOL", TS_AG_LOCALE_SPANISH},
    {"titan", NULL, TS_AG_LOCALE_ENGLISH},
};

size_t ts_language_choice_count(void) {
  return sizeof(g_choices) / sizeof(g_choices[0]);
}

const ts_language_choice *ts_language_choice_at(size_t index) {
  return index < ts_language_choice_count() ? &g_choices[index] : NULL;
}

int ts_language_choice_index(const char *engine_key) {
  size_t index;
  if (!engine_key || !engine_key[0])
    return -1;
  for (index = 0; index < ts_language_choice_count(); ++index) {
    if (strcmp(engine_key, g_choices[index].engine_key) == 0)
      return (int)index;
  }
  return -1;
}

int ts_language_system_locale(const char *locale_name) {
  char first;
  char second;
  if (!locale_name || !locale_name[0] || !locale_name[1])
    return TS_AG_LOCALE_ENGLISH;
  first = (char)tolower((unsigned char)locale_name[0]);
  second = (char)tolower((unsigned char)locale_name[1]);
  if (first == 'f' && second == 'r')
    return TS_AG_LOCALE_FRENCH;
  if (first == 'd' && second == 'e')
    return TS_AG_LOCALE_GERMAN;
  if (first == 'e' && second == 's')
    return TS_AG_LOCALE_SPANISH;
  if (first == 'p' && second == 't')
    return TS_AG_LOCALE_PORTUGUESE;
  return TS_AG_LOCALE_ENGLISH;
}
