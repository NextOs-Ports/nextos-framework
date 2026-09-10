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

/* Split a game language key or BCP-47-ish setting into the title's primary
 * language key and optional region.  PlayerPrefs stores keys such as "pt";
 * NEXTOSSETTINGS may use either "pt" or "pt-BR". */
static int parse_language(const char *requested, char *lang, size_t lang_cap,
                          char *region, size_t region_cap) {
  size_t i;
  size_t j;

  if (requested == NULL || requested[0] == '\0' || lang_cap == 0u ||
      region_cap == 0u)
    return 0;

  lang[0] = '\0';
  region[0] = '\0';
  for (i = 0; requested[i] != '\0' && requested[i] != '-' &&
              requested[i] != '_' && i < lang_cap - 1u; i++)
    lang[i] = (char)tolower((unsigned char)requested[i]);
  lang[i] = '\0';
  if (!lang_is_supported(lang))
    return 0;

  if (requested[i] == '-' || requested[i] == '_') {
    i++;
    for (j = 0; requested[i] != '\0' && j < region_cap - 1u; i++, j++)
      region[j] = (char)toupper((unsigned char)requested[i]);
    region[j] = '\0';
  }
  return 1;
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
  char saved[40];
  char lang[16];
  char region[16];
  const char *source;
  int explicit_setting;

  if (ready)
    return &snap;

  requested[0] = '\0';
  saved[0] = '\0';
  lang[0] = '\0';
  region[0] = '\0';

  explicit_setting = read_setting_language(requested, sizeof requested) &&
                     requested[0] != '\0' &&
                     strcmp(requested, "auto") != 0;
  source = "NEXTOSSETTINGS";

  if (!explicit_setting) {
    /* Nameless Cat writes the detected Android system language back to the
     * PlayerPrefs key "language" on every boot without reading it first.  Use
     * the owner's last valid in-game selection as the simulated system locale
     * so that native startup writes the same value instead of resetting it to
     * English.  An explicit NEXTOSSETTINGS choice still has priority. */
    if (st_prefs_get_string("language", saved, sizeof saved) &&
        parse_language(saved, lang, sizeof lang, region, sizeof region)) {
      snprintf(requested, sizeof requested, "%s", saved);
      source = "saved-game-choice";
    } else {
      snprintf(requested, sizeof requested, "%s", "en-US");
      source = "game-default";
    }
  }

  if (!parse_language(requested, lang, sizeof lang, region, sizeof region)) {
    set_snapshot(&snap, "en", "US", 0);
    source = "game-default-invalid-request";
  } else {
    if (region[0] == '\0') {
      const char *dr = default_region_for(lang);
      set_snapshot(&snap, lang, dr, explicit_setting);
    } else {
      set_snapshot(&snap, lang, region, explicit_setting);
    }
  }
  snprintf(snap.requested, sizeof snap.requested, "%s", requested);
  fprintf(stderr, "[st/language] locale %s (source=%s, requested=%s)\n",
          snap.tag, source, snap.requested);
  ready = 1;
  return &snap;
}
