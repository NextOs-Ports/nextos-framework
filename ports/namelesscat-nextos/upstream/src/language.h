/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MOS_LANGUAGE_H
#define MOS_LANGUAGE_H

/* V3 audit (blocker 6): ONE canonical language snapshot for this port. Every
 * locale-facing site (Java Locale, Bionic system properties, the Netflix SDK
 * preferred-language) reads this SAME snapshot. An explicit
 * NEXTOSSETTINGS.txt value has priority; otherwise the snapshot inherits the
 * owner's valid in-game language choice before using the title's English
 * default. No site hardcodes a locale on its own any more. */
typedef struct mos_language {
  char tag[24];        /* canonical BCP-47, e.g. "pt-BR" or "en-US" */
  char language[8];    /* lowercase primary subtag, e.g. "pt" / "en" */
  char country[8];     /* uppercase region, e.g. "BR" / "US" */
  char underscore[24]; /* Java toString form, e.g. "pt_BR" / "en_US" */
  char requested[40];  /* selected raw setting, saved key or default */
  int  from_settings;  /* 1 if resolved from an explicit setting */
} mos_language;

/* Returns the process-wide snapshot, resolving it once on first call from an
 * explicit st_gamedir/NEXTOSSETTINGS.txt value, the saved game choice, or the
 * title default (in that order). Never returns NULL. */
const mos_language *mos_language_get(void);

#endif /* MOS_LANGUAGE_H */
