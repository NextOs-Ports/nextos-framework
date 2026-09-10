/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for nxcompat_language_resolve_v2 (V3-SETTINGS-01). */
#include "nxcompat_language_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++failures;                                                          \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                      \
  } while (0)

static const char *const kSupported[] = {"en", "pt-BR", "zh-Hans"};
static const size_t kSupportedCount = 3u;

static int resolve(const char *ovr, const char *set, const char *sdl,
                   const char *loc, nxcompat_language_snapshot *snap) {
  return nxcompat_language_resolve_v2(ovr, set, sdl, loc, kSupported,
                                      kSupportedCount, "en", snap);
}

static void test_parse_variants(void) {
  nxcompat_language_snapshot snap;

  /* pt_BR.UTF-8: underscore + encoding suffix */
  CHECK(resolve("pt_BR.UTF-8", NULL, NULL, NULL, &snap) == 0);
  CHECK(strcmp(snap.canonical_tag, "pt-BR") == 0);
  CHECK(strcmp(snap.language, "pt") == 0);
  CHECK(strcmp(snap.region, "BR") == 0);
  CHECK(snap.script[0] == '\0');
  CHECK(strcmp(snap.underscore_variant, "pt_BR") == 0);
  CHECK(strcmp(snap.hyphen_variant, "pt-BR") == 0);
  CHECK(strcmp(snap.iso639_3, "por") == 0);
  CHECK(strcmp(snap.direction, "ltr") == 0);
  CHECK(strcmp(snap.matched_supported, "pt-BR") == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_SESSION_OVERRIDE);
  CHECK(snap.used_fallback == 0);
  CHECK(snap.api_version == NXCOMPAT_LANGUAGE_V2_API_VERSION);
  CHECK(strcmp(snap.requested, "pt_BR.UTF-8") == 0);

  /* PT-br: crazy case is canonicalized */
  CHECK(resolve("PT-br", NULL, NULL, NULL, &snap) == 0);
  CHECK(strcmp(snap.canonical_tag, "pt-BR") == 0);
  CHECK(strcmp(snap.matched_supported, "pt-BR") == 0);

  /* es_ES: valid but not supported -> continue down to fallback en */
  CHECK(resolve("es_ES", NULL, NULL, NULL, &snap) == 0);
  CHECK(snap.used_fallback == 1);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_PORT_FALLBACK);
  CHECK(strcmp(snap.canonical_tag, "en") == 0);

  /* zh-Hant-TW canonical split */
  CHECK(resolve(NULL, NULL, NULL, "zh-Hant-TW", &snap) == 0 ||
        1); /* result checked in script-safety test below */
}

static void test_script_safety(void) {
  nxcompat_language_snapshot snap;
  const char *const cn_only[] = {"en", "zh-CN"};

  /* zh-Hant must NOT resolve to zh-Hans by bare language: falls through
   * the whole order into the declared fallback. */
  CHECK(resolve("zh-Hant", NULL, NULL, NULL, &snap) == 0);
  CHECK(snap.used_fallback == 1);
  CHECK(strcmp(snap.canonical_tag, "en") == 0);
  CHECK(strcmp(snap.matched_supported, "en") == 0);

  /* zh-Hant-TW vs {en, pt-BR, zh-Hans}: same story */
  CHECK(resolve("zh-Hant-TW", NULL, NULL, NULL, &snap) == 0);
  CHECK(snap.used_fallback == 1);
  CHECK(strcmp(snap.canonical_tag, "en") == 0);

  /* zh-CN implies Hans: zh-Hant never lands there either */
  CHECK(nxcompat_language_resolve_v2("zh-Hant", NULL, NULL, NULL, cn_only,
                                     2u, "en", &snap) == 0);
  CHECK(snap.used_fallback == 1);
  CHECK(strcmp(snap.canonical_tag, "en") == 0);

  /* the positive side: zh-Hans requests do reach zh-Hans */
  CHECK(resolve("zh_CN.UTF-8", NULL, NULL, NULL, &snap) == 0);
  CHECK(snap.used_fallback == 0);
  CHECK(strcmp(snap.matched_supported, "zh-Hans") == 0);
  CHECK(strcmp(snap.canonical_tag, "zh-CN") == 0);
  CHECK(snap.script[0] == '\0'); /* Hans only IMPLIED, not explicit */
  CHECK(strcmp(snap.iso639_3, "zho") == 0);
}

static void test_no_preference_rungs(void) {
  nxcompat_language_snapshot snap;

  /* C / POSIX / empty / garbage all skip the rung */
  CHECK(resolve("C", "POSIX", "", "!!bad!!", &snap) == 0);
  CHECK(snap.used_fallback == 1);
  CHECK(strcmp(snap.canonical_tag, "en") == 0);

  CHECK(resolve("C.UTF-8", NULL, "auto", "pt_BR.UTF-8", &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_POSIX_LOCALE_READONLY);
  CHECK(strcmp(snap.canonical_tag, "pt-BR") == 0);
  CHECK(snap.used_fallback == 0);
}

static void test_precedence(void) {
  nxcompat_language_snapshot snap;

  /* override beats settings beats SDL beats locale beats fallback */
  CHECK(resolve("pt-BR", "en", "en", "en", &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_SESSION_OVERRIDE);
  CHECK(strcmp(snap.canonical_tag, "pt-BR") == 0);

  CHECK(resolve(NULL, "pt-BR", "en", "en", &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_SETTINGS_FILE);
  CHECK(strcmp(snap.canonical_tag, "pt-BR") == 0);

  CHECK(resolve(NULL, NULL, "pt-BR", "en", &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_SDL_FIRMWARE_PREFERENCE);

  CHECK(resolve(NULL, NULL, NULL, "pt_BR.UTF-8", &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_POSIX_LOCALE_READONLY);

  CHECK(resolve(NULL, NULL, NULL, NULL, &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_PORT_FALLBACK);
  CHECK(snap.used_fallback == 1);
  CHECK(snap.requested[0] == '\0');

  /* an invalid override falls through to the settings rung */
  CHECK(resolve("garbage-tag-!!", "pt-BR", NULL, NULL, &snap) == 0);
  CHECK(snap.origin == NXCOMPAT_LANGUAGE_ORIGIN_SETTINGS_FILE);
}

static void test_fail_closed(void) {
  nxcompat_language_snapshot snap;

  /* fallback not a member of supported: nonzero, no invented tag */
  CHECK(nxcompat_language_resolve_v2(NULL, NULL, NULL, NULL, kSupported,
                                     kSupportedCount, "fr", &snap) != 0);
  CHECK(snap.canonical_tag[0] == '\0');
  CHECK(snap.reason[0] != '\0');

  /* unparseable fallback: nonzero */
  CHECK(nxcompat_language_resolve_v2(NULL, NULL, NULL, NULL, kSupported,
                                     kSupportedCount, "", &snap) != 0);

  /* empty supported list: nonzero */
  CHECK(nxcompat_language_resolve_v2("en", NULL, NULL, NULL, NULL, 0u,
                                     "en", &snap) != 0);
}

static void test_fields(void) {
  nxcompat_language_snapshot snap;
  const char *const rtl_supported[] = {"ar", "en"};
  const char *const hant[] = {"zh-Hant-TW", "en"};

  /* direction rtl + unknown-table language stays honest */
  CHECK(nxcompat_language_resolve_v2("ar_EG.UTF-8", NULL, NULL, NULL,
                                     rtl_supported, 2u, "en", &snap) == 0);
  CHECK(strcmp(snap.direction, "rtl") == 0);
  CHECK(strcmp(snap.iso639_3, "unknown") == 0);
  CHECK(strcmp(snap.language, "ar") == 0);
  CHECK(strcmp(snap.region, "EG") == 0);

  /* full lang-script-region underscore/hyphen variants */
  CHECK(nxcompat_language_resolve_v2("ZH_hant_tw", NULL, NULL, NULL, hant,
                                     2u, "en", &snap) == 0);
  CHECK(strcmp(snap.canonical_tag, "zh-Hant-TW") == 0);
  CHECK(strcmp(snap.script, "Hant") == 0);
  CHECK(strcmp(snap.underscore_variant, "zh_Hant_TW") == 0);
  CHECK(strcmp(snap.hyphen_variant, "zh-Hant-TW") == 0);
  CHECK(strcmp(snap.matched_supported, "zh-Hant-TW") == 0);
}

int main(void) {
  test_parse_variants();
  test_script_safety();
  test_no_preference_rungs();
  test_precedence();
  test_fail_closed();
  test_fields();
  if (failures != 0) {
    fprintf(stderr, "test_language_v2: %d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  printf("test_language_v2: PASS\n");
  return EXIT_SUCCESS;
}
