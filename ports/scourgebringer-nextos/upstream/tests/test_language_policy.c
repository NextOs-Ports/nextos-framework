#include "language_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct expected_profile {
    const char *nx;
    const char *runtime;
    const char *android_language;
    const char *country;
    const char *content;
};

static int failures;

static void check_profile(const char *input, const struct expected_profile *want) {
    const struct sb_language_profile *got = sb_language_for_code(input);
    if (strcmp(got->nx_code, want->nx) ||
        strcmp(got->runtime_locale, want->runtime) ||
        strcmp(got->android_language, want->android_language) ||
        strcmp(got->android_country, want->country) ||
        strcmp(got->content_code, want->content)) {
        fprintf(stderr,
                "FAIL input=%s got=%s/%s/%s/%s/%s want=%s/%s/%s/%s/%s\n",
                input ? input : "(null)", got->nx_code, got->runtime_locale,
                got->android_language, got->android_country, got->content_code,
                want->nx, want->runtime, want->android_language, want->country,
                want->content);
        ++failures;
    }
}

int main(void) {
    static const struct expected_profile cases[] = {
        {"en", "en", "en", "US", "EN"},
        {"fr", "fr", "fr", "FR", "FR"},
        {"it", "it", "it", "IT", "IT"},
        {"de", "de", "de", "DE", "DE"},
        {"es", "es", "es", "ES", "SP"},
        {"ru", "ru", "ru", "RU", "RU"},
        {"pt-br", "pt-BR", "pt", "BR", "PB"},
        {"zh-cn", "zh-CN", "zh", "CN", "CN"},
        {"ja", "ja", "ja", "JP", "JP"},
        {"ko", "ko", "ko", "KR", "KR"},
        {"pl", "pl", "pl", "PL", "PL"},
        {"zh-tw", "zh-TW", "zh", "TW", "CHT"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        check_profile(cases[i].nx, &cases[i]);

    check_profile("unsupported", &cases[0]);
    setenv("LC_ALL", "pt_BR.UTF-8", 1);
    check_profile("auto", &cases[6]);
    setenv("LC_ALL", "zh_HK.UTF-8", 1);
    check_profile("auto", &cases[11]);
    setenv("LC_ALL", "zh_CN.UTF-8", 1);
    check_profile("auto", &cases[7]);
    setenv("LC_ALL", "es_MX.UTF-8", 1);
    check_profile("auto", &cases[4]);
    setenv("LC_ALL", "C", 1);
    check_profile("auto", &cases[0]);

    if (failures)
        return 1;
    puts("language policy: 12 explicit profiles + auto/fallback OK");
    return 0;
}
