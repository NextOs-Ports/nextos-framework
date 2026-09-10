#ifndef SCOURGEBRINGER_LANGUAGE_POLICY_H
#define SCOURGEBRINGER_LANGUAGE_POLICY_H

/*
 * Canonical launcher language translated to the three formats consumed by
 * the original Android runtime.  Content codes are the names shipped in the
 * APK under assets/Content/Localizations.
 */
struct sb_language_profile {
    const char *nx_code;
    const char *runtime_locale;
    const char *android_language;
    const char *android_country;
    const char *content_code;
};

const struct sb_language_profile *sb_language_for_code(const char *code);
const struct sb_language_profile *sb_language_current(void);

#endif
