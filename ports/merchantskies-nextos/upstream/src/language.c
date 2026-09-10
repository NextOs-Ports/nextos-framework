/* SPDX-License-Identifier: GPL-3.0-only */
/* V3 audit (blocker 6): single canonical language snapshot. */
#include "language.h"
#include "gb.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

/* Parse "language=<value>" from a NEXTOS_SETTINGS/1 file. Returns 1 and fills
 * value on success. Strict enough for the pilot: one language line, bounded. */
static int read_setting_language(char *value, size_t cap) {
  char path[1100];
  FILE *f;
  char line[256];
  int magic = 0;
  struct stat st;

  snprintf(path, sizeof path, "%s/NEXTOSSETTINGS.txt", st_gamedir);
  if (lstat(path, &st) != 0 || (st.st_mode & S_IFMT) == S_IFLNK)
    return 0;
  f = fopen(path, "r");
  if (f == NULL)
    return 0;
  while (fgets(line, sizeof line, f) != NULL) {
    char *p = line;
    size_t n = strlen(line);
    while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r'))
      line[--n] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') continue;
    if (!magic) {
      if (strcmp(p, "# NEXTOS_SETTINGS/1") == 0) magic = 1;
      else if (*p == '#') continue;
      else break; /* first non-comment line is not the magic: reject */
      continue;
    }
    if (*p == '#') continue;
    if (strncmp(p, "language", 8u) == 0) {
      char *eq = strchr(p, '=');
      if (eq != NULL) {
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '\0' && strlen(v) < cap) {
          strncpy(value, v, cap - 1u);
          value[cap - 1u] = '\0';
          fclose(f);
          return 1;
        }
      }
    }
  }
  fclose(f);
  return 0;
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

  if (!read_setting_language(requested, sizeof requested) ||
      requested[0] == '\0' ||
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
