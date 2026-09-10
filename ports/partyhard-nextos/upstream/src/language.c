/* SPDX-License-Identifier: GPL-3.0-only */
/* Single canonical language snapshot from the V5 settings parser. */
#include "language.h"
#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The languages this title actually ships (I2 Localization). "auto" and any
 * unsupported request fall back to en-US so the game always has real text. */
static const char *const k_supported_lang[] = {
  "en", "pt", "ru", "de", "es", "fr", "it", "pl", "zh", "ja", "ko", "tr",
  "uk", "cs", "nl",
};

static int lang_is_supported(const char *lang) {
  size_t i;
  for (i = 0; i < sizeof k_supported_lang / sizeof *k_supported_lang; i++)
    if (strcmp(lang, k_supported_lang[i]) == 0)
      return 1;
  return 0;
}

/* Default region for the languages that have a canonical storefront region in
 * this title; anything else keeps an empty region. */
static const char *default_region_for(const char *lang) {
  if (strcmp(lang, "en") == 0) return "US";
  if (strcmp(lang, "pt") == 0) return "BR";
  return "";
}

static void set_snapshot(mos_language *s, const char *lang, const char *region,
                         int from_settings) {
  size_t i;
  memset(s, 0, sizeof *s);
  for (i = 0; lang[i] != '\0' && i < sizeof s->language - 1u; i++)
    s->language[i] = (char)tolower((unsigned char)lang[i]);
  for (i = 0; region[i] != '\0' && i < sizeof s->country - 1u; i++)
    s->country[i] = (char)toupper((unsigned char)region[i]);
  if (s->country[0] != '\0') {
    snprintf(s->tag, sizeof s->tag, "%s-%s", s->language, s->country);
    snprintf(s->underscore, sizeof s->underscore, "%s_%s",
             s->language, s->country);
  } else {
    snprintf(s->tag, sizeof s->tag, "%s", s->language);
    snprintf(s->underscore, sizeof s->underscore, "%s", s->language);
  }
  s->from_settings = from_settings;
}

const mos_language *mos_language_get(void) {
  static mos_language snap;
  static int ready = 0;
  char requested[40];
  char lang[16];
  char region[16];
  size_t i;

  if (ready)
    return &snap;

  requested[0] = '\0';
  lang[0] = '\0';
  region[0] = '\0';

  const nxcompat_settings *settings = st_settings_get();
  snprintf(requested, sizeof requested, "%s", settings->language);
  if (requested[0] == '\0' ||
      strcmp(requested, "auto") == 0) {
    /* No explicit choice: the game's own default. */
    set_snapshot(&snap, "en", "US", 0);
    snprintf(snap.requested, sizeof snap.requested, "%s",
             requested[0] ? requested : "auto");
    ready = 1;
    return &snap;
  }

  /* Split "<lang>[-_<REGION>]". */
  for (i = 0; requested[i] != '\0' && requested[i] != '-' &&
              requested[i] != '_' && i < sizeof lang - 1u; i++)
    lang[i] = (char)tolower((unsigned char)requested[i]);
  lang[i] = '\0';
  if (requested[i] == '-' || requested[i] == '_') {
    size_t j = 0;
    i++;
    for (; requested[i] != '\0' && j < sizeof region - 1u; i++, j++)
      region[j] = (char)toupper((unsigned char)requested[i]);
    region[j] = '\0';
  }

  if (!lang_is_supported(lang)) {
    set_snapshot(&snap, "en", "US", 0);
  } else {
    if (region[0] == '\0') {
      const char *dr = default_region_for(lang);
      set_snapshot(&snap, lang, dr, 1);
    } else {
      set_snapshot(&snap, lang, region, 1);
    }
  }
  snprintf(snap.requested, sizeof snap.requested, "%s", requested);
  ready = 1;
  return &snap;
}
