/* ab_locale.c — language selected by the launcher (NXPORT_LANGUAGE).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Angry Birds Classic 8.0.3 ships 13 localizations in
 * assets/data/localization (en, fr, it, de, es, zh-TW, zh-CN, ja, ru, ar,
 * pt-PT, pt-BR, pl) and picks one from the Android locale it reads through
 * JNI (Localization.deviceLocale / java.util.Locale). The launcher's visible
 * GAME_LANGUAGE line becomes NXPORT_LANGUAGE; this maps it to the answers
 * the JNI shim gives the engine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ab_locale.h"
#include "ab_port.h"

typedef struct {
  const char *code;   /* launcher code (lower-case) */
  const char *lang;   /* ISO 639-1 */
  const char *ctry;   /* ISO 3166-1 */
  const char *lang3;  /* ISO 639-2 */
  const char *ctry3;  /* ISO 3166-1 alpha-3 */
} ab_locale_entry;

/* Only languages the game actually ships. Japanese is intentionally absent:
 * it is never a default and the house rule keeps it out of the list. */
static const ab_locale_entry TABLE[] = {
    {"en", "en", "US", "eng", "USA"},
    {"pt-br", "pt", "BR", "por", "BRA"},
    {"pt", "pt", "PT", "por", "PRT"},
    {"es", "es", "ES", "spa", "ESP"},
    {"fr", "fr", "FR", "fra", "FRA"},
    {"de", "de", "DE", "deu", "DEU"},
    {"it", "it", "IT", "ita", "ITA"},
    {"ru", "ru", "RU", "rus", "RUS"},
    {"pl", "pl", "PL", "pol", "POL"},
    {"ar", "ar", "SA", "ara", "SAU"},
    {"zh-cn", "zh", "CN", "zho", "CHN"},
    {"zh-tw", "zh", "TW", "zho", "TWN"},
};

static const ab_locale_entry *g_sel = &TABLE[0];
static char g_tag[16] = "en-US";
static char g_underscore[16] = "en_US";
static int g_done;

static const ab_locale_entry *find_code(const char *code) {
  for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++)
    if (strcasecmp(TABLE[i].code, code) == 0)
      return &TABLE[i];
  return NULL;
}

/* "pt_BR.UTF-8" / "pt-BR" / "pt" -> table entry or NULL */
static const ab_locale_entry *from_system(const char *value) {
  char lang[8] = "", ctry[8] = "";
  size_t i = 0, j = 0;
  if (!value || !*value || strcasecmp(value, "C") == 0 ||
      strncasecmp(value, "POSIX", 5) == 0)
    return NULL;
  while (value[i] && value[i] != '_' && value[i] != '-' && value[i] != '.' &&
         value[i] != '@' && j < 7)
    lang[j++] = (char)(value[i++] | 0x20);
  lang[j] = 0;
  if (value[i] == '_' || value[i] == '-') {
    i++; j = 0;
    while (value[i] && value[i] != '.' && value[i] != '@' && j < 7)
      ctry[j++] = (char)(value[i++] & ~0x20);
    ctry[j] = 0;
  }
  if (strcmp(lang, "pt") == 0)
    return find_code(strcmp(ctry, "PT") == 0 ? "pt" : "pt-br");
  if (strcmp(lang, "zh") == 0)
    return find_code((strcmp(ctry, "TW") == 0 || strcmp(ctry, "HK") == 0) ?
                     "zh-tw" : "zh-cn");
  return find_code(lang); /* en/es/fr/de/it/ru/pl/ar; ja -> NULL -> en */
}

void ab_locale_init(void) {
  const char *req = getenv("NXPORT_LANGUAGE");
  const char *how = "default";
  const ab_locale_entry *e = NULL;
  if (g_done)
    return;
  g_done = 1;
  if (req && *req && strcasecmp(req, "auto") != 0) {
    e = find_code(req);
    how = e ? "GAME_LANGUAGE" : "GAME_LANGUAGE (unknown code, using en)";
  }
  if (!e && (!req || !*req || strcasecmp(req, "auto") == 0)) {
    const char *sys = getenv("LC_ALL");
    if (!sys || !*sys) sys = getenv("LC_MESSAGES");
    if (!sys || !*sys) sys = getenv("LANG");
    e = from_system(sys);
    how = e ? "auto (system LANG)" : "auto (system LANG unusable, using en)";
  }
  if (!e)
    e = &TABLE[0];
  g_sel = e;
  snprintf(g_tag, sizeof g_tag, "%s-%s", e->lang, e->ctry);
  snprintf(g_underscore, sizeof g_underscore, "%s_%s", e->lang, e->ctry);
  ab_log("[locale] %s -> %s (%s)", req && *req ? req : "(unset)", g_tag, how);
}

const char *ab_locale_language(void) { ab_locale_init(); return g_sel->lang; }
const char *ab_locale_country(void) { ab_locale_init(); return g_sel->ctry; }
const char *ab_locale_tag(void) { ab_locale_init(); return g_tag; }
const char *ab_locale_underscore(void) { ab_locale_init(); return g_underscore; }
const char *ab_locale_iso3_language(void) { ab_locale_init(); return g_sel->lang3; }
const char *ab_locale_iso3_country(void) { ab_locale_init(); return g_sel->ctry3; }
