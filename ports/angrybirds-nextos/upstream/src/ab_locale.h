/* ab_locale.h — language selected by the launcher (NXPORT_LANGUAGE).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_LOCALE_H
#define AB_LOCALE_H

/* Resolve once from NXPORT_LANGUAGE ("auto" or a code from the launcher's
 * GAME_LANGUAGE list). "auto" reads the system LANG/LC_ALL/LC_MESSAGES as they
 * were BEFORE the port forces its own C locale, and falls back to English.
 * Japanese is never chosen automatically. Logs the decision. */
void ab_locale_init(void);
const char *ab_locale_language(void);      /* "pt" */
const char *ab_locale_country(void);       /* "BR" */
const char *ab_locale_tag(void);           /* "pt-BR" */
const char *ab_locale_underscore(void);    /* "pt_BR" */
const char *ab_locale_iso3_language(void); /* "por" */
const char *ab_locale_iso3_country(void);  /* "BRA" */

#endif
