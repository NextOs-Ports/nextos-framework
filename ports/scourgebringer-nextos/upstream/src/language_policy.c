#include "language_policy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Keep this order aligned with ScourgeBringer.Language in the managed game. */
static const struct sb_language_profile sb_languages[] = {
    {"en",    "en",    "en", "US", "EN"},
    {"fr",    "fr",    "fr", "FR", "FR"},
    {"it",    "it",    "it", "IT", "IT"},
    {"de",    "de",    "de", "DE", "DE"},
    {"es",    "es",    "es", "ES", "SP"},
    {"ru",    "ru",    "ru", "RU", "RU"},
    {"pt-br", "pt-BR", "pt", "BR", "PB"},
    {"zh-cn", "zh-CN", "zh", "CN", "CN"},
    {"ja",    "ja",    "ja", "JP", "JP"},
    {"ko",    "ko",    "ko", "KR", "KR"},
    {"pl",    "pl",    "pl", "PL", "PL"},
    {"zh-tw", "zh-TW", "zh", "TW", "CHT"},
};

static const size_t sb_language_count =
    sizeof(sb_languages) / sizeof(sb_languages[0]);

static const struct sb_language_profile *sb_language_english(void) {
    return &sb_languages[0];
}

static void sb_normalize_locale(const char *source, char *out, size_t capacity) {
    size_t used = 0;

    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (!source)
        return;

    while (*source && *source != '.' && *source != '@' && used + 1 < capacity) {
        unsigned char byte = (unsigned char)*source++;
        out[used++] = byte == '_' ? '-' : (char)tolower(byte);
    }
    out[used] = '\0';
}

static const struct sb_language_profile *sb_language_from_host_locale(void) {
    const char *source = getenv("LC_ALL");
    char locale[32];

    if (!source || !source[0])
        source = getenv("LC_MESSAGES");
    if (!source || !source[0])
        source = getenv("LANG");
    sb_normalize_locale(source, locale, sizeof(locale));

    if (!locale[0] || strcmp(locale, "c") == 0 ||
        strcmp(locale, "posix") == 0)
        return sb_language_english();

    if (strncmp(locale, "zh", 2) == 0) {
        if (strstr(locale, "-tw") || strstr(locale, "-hk") ||
            strstr(locale, "-mo") || strstr(locale, "-hant"))
            return sb_language_for_code("zh-tw");
        return sb_language_for_code("zh-cn");
    }
    if (strncmp(locale, "pt", 2) == 0)
        return sb_language_for_code("pt-br");

    for (size_t i = 0; i < sb_language_count; ++i) {
        const char *code = sb_languages[i].nx_code;
        if (strlen(code) == 2 && strncmp(locale, code, 2) == 0 &&
            (locale[2] == '\0' || locale[2] == '-'))
            return &sb_languages[i];
    }
    return sb_language_english();
}

const struct sb_language_profile *sb_language_for_code(const char *code) {
    if (!code || !code[0] || strcmp(code, "auto") == 0)
        return sb_language_from_host_locale();

    for (size_t i = 0; i < sb_language_count; ++i) {
        if (strcmp(code, sb_languages[i].nx_code) == 0)
            return &sb_languages[i];
    }
    return sb_language_english();
}

const struct sb_language_profile *sb_language_current(void) {
    return sb_language_for_code(getenv("NXPORT_LANGUAGE"));
}
