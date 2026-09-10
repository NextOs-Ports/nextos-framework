/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MOS_LANGUAGE_H
#define MOS_LANGUAGE_H

/* V3 audit (blocker 6): ONE canonical language snapshot for this port. Every
 * locale-facing site (Java Locale, Bionic system properties, the Netflix SDK
 * preferred-language) reads this SAME snapshot, resolved once from the owner's
 * NEXTOSSETTINGS.txt via the V3 settings contract. No site hardcodes a locale
 * on its own any more. */
typedef struct mos_language {
  char tag[24];        /* canonical BCP-47, e.g. "pt-BR" or "en-US" */
  char language[8];    /* lowercase primary subtag, e.g. "pt" / "en" */
  char country[8];     /* uppercase region, e.g. "BR" / "US" */
  char underscore[24]; /* Java toString form, e.g. "pt_BR" / "en_US" */
  char requested[40];  /* the raw value read from NEXTOSSETTINGS.txt */
  int  from_settings;  /* 1 if resolved from an explicit setting */
} mos_language;

/* Returns the process-wide snapshot, resolving it once on first call from
 * st_gamedir/NEXTOSSETTINGS.txt. Never returns NULL. */
const mos_language *mos_language_get(void);

#endif /* MOS_LANGUAGE_H */
