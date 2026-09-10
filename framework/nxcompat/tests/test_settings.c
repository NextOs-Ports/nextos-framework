/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for nxcompat_settings_parse (V3-SETTINGS-01). */
#include "nxcompat_settings.h"

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

/* V5 0.5.1: the recovery cases below assert a CONTRACT, so the failure has to
 * name it -- the condition alone would not say why it matters. */
#define CHECK_MSG(cond, message)                                           \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++failures;                                                          \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (message));  \
    }                                                                      \
  } while (0)

static int parse_str(const char *text, nxcompat_settings *out,
                     nxcompat_settings_unknown_key_fn cb, void *user) {
  return nxcompat_settings_parse(text, strlen(text), out, cb, user);
}

static void count_unknown(const char *key, size_t key_len, void *user) {
  (void)key;
  (void)key_len;
  *(int *)user += 1;
}

static void check_defaults(const nxcompat_settings *s) {
  CHECK(strcmp(s->language, "auto") == 0);
  CHECK(strcmp(s->quality, "auto") == 0);
  CHECK(s->unknown_key_count == 0u);
}

static void test_valid(void) {
  nxcompat_settings s;
  CHECK(parse_str("# NEXTOS_SETTINGS/1\n"
                  "# a comment\n"
                  "\n"
                  "language=pt-BR\n"
                  "quality=high\n",
                  &s, NULL, NULL) == 0);
  CHECK(strcmp(s.language, "pt-BR") == 0);
  CHECK(strcmp(s.quality, "high") == 0);
  CHECK(s.unknown_key_count == 0u);
  CHECK(s.api_version == NXCOMPAT_SETTINGS_API_VERSION);

  /* blank lines before the magic, CRLF, missing keys stay at defaults */
  CHECK(parse_str("\n\r\n# NEXTOS_SETTINGS/1\r\nquality=low\r\n", &s,
                  NULL, NULL) == 0);
  CHECK(strcmp(s.language, "auto") == 0);
  CHECK(strcmp(s.quality, "low") == 0);

  /* magic alone is a valid (all defaults) file */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\n", &s, NULL, NULL) == 0);
  check_defaults(&s);
}

static void test_unknown_key_rejected_fail_closed(void) {
  /* V3-SETTINGS-01 (spec): unknown keys are REJECTED fail-closed; the
   * callback still fires for diagnostics before the parse fails, and the
   * output is reset to safe defaults (last valid config preserved). */
  nxcompat_settings s;
  int seen = 0;
  CHECK(parse_str("# NEXTOS_SETTINGS/1\n"
                  "language=en\n"
                  "future_key=whatever\n",
                  &s, count_unknown, &seen) != 0);
  CHECK(seen == 1);                       /* diagnostic callback fired */
  CHECK(strcmp(s.language, "auto") == 0); /* reset to safe defaults */
  CHECK(strcmp(s.quality, "auto") == 0);
  /* an unknown key even after a valid known key still fails closed */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nfk=1\n", &s, NULL, NULL) != 0);
  CHECK(strcmp(s.language, "auto") == 0);
}

static void test_duplicate_known_key_fatal(void) {
  nxcompat_settings s;
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nlanguage=en\nlanguage=es\n", &s,
                  NULL, NULL) != 0);
  check_defaults(&s);
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nquality=low\nquality=low\n", &s,
                  NULL, NULL) != 0);
  check_defaults(&s);
}

static void test_nul_fatal(void) {
  nxcompat_settings s;
  static const char body[] = "# NEXTOS_SETTINGS/1\nlanguage=e\0n\n";
  CHECK(nxcompat_settings_parse(body, sizeof body - 1u, &s, NULL, NULL) !=
        0);
  check_defaults(&s);
}

static void test_oversize_fatal(void) {
  nxcompat_settings s;
  char *big = malloc(NXCOMPAT_SETTINGS_MAX_BYTES + 2u);
  CHECK(big != NULL);
  if (big != NULL) {
    memset(big, '#', NXCOMPAT_SETTINGS_MAX_BYTES + 1u);
    big[NXCOMPAT_SETTINGS_MAX_BYTES + 1u] = '\0';
    CHECK(nxcompat_settings_parse(big, NXCOMPAT_SETTINGS_MAX_BYTES + 1u,
                                  &s, NULL, NULL) != 0);
    check_defaults(&s);
    free(big);
  }
}

static void test_bad_magic_fatal(void) {
  nxcompat_settings s;
  CHECK(parse_str("language=en\n", &s, NULL, NULL) != 0);
  check_defaults(&s);
  /* 0.5.0: /2 is a known schema now (see test_schema2); /3 is not */
  CHECK(parse_str("# NEXTOS_SETTINGS/3\nlanguage=en\n", &s, NULL, NULL) !=
        0);
  check_defaults(&s);
  CHECK(parse_str("# a comment first\n# NEXTOS_SETTINGS/1\n", &s, NULL,
                  NULL) != 0);
  CHECK(parse_str("", &s, NULL, NULL) != 0);
  check_defaults(&s);
}

static void test_quality_allowlist(void) {
  nxcompat_settings s;
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nquality=medium\n", &s, NULL,
                  NULL) == 0);
  CHECK(strcmp(s.quality, "medium") == 0);
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nquality=ultra\n", &s, NULL,
                  NULL) != 0);
  check_defaults(&s);
}

static void test_charset_and_syntax(void) {
  nxcompat_settings s;
  /* bad value charset */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nlanguage=pt BR\n", &s, NULL,
                  NULL) != 0);
  check_defaults(&s);
  /* empty value */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nlanguage=\n", &s, NULL, NULL) !=
        0);
  /* value longer than 32 */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\n"
                  "language=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",
                  &s, NULL, NULL) != 0);
  /* line without '=' */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\nlanguage\n", &s, NULL, NULL) !=
        0);
  /* malformed UTF-8 */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\n# \xC3\x28\n", &s, NULL, NULL) !=
        0);
  check_defaults(&s);
  /* valid multibyte UTF-8 in a comment is fine */
  CHECK(parse_str("# NEXTOS_SETTINGS/1\n# coment\xC3\xA1rio\n", &s, NULL,
                  NULL) == 0);
}

static void test_schema2(void) {
  nxcompat_settings s; nxcompat_settings_error e;
  CHECK(parse_str("# NEXTOS_SETTINGS/2\nlanguage=pt-BR\nvideo.authority=nextos\nvideo.output_size=display\nvideo.aspect=preserve\nvideo.filter=engine\nvideo.invalid_policy=fail_closed\n", &s, NULL, NULL) == 0);
  CHECK(s.schema == 2u && strcmp(s.language, "pt-BR") == 0 && strcmp(s.video_authority, "nextos") == 0 && strcmp(s.video_aspect, "preserve") == 0 && strcmp(s.video_invalid_policy, "fail_closed") == 0);
  /* /2 with the video keys absent: "" (adapter applies the package default, never a hidden value) */
  CHECK(parse_str("# NEXTOS_SETTINGS/2\nquality=high\n", &s, NULL, NULL) == 0 && s.schema == 2u && s.video_aspect[0] == '\0' && strcmp(s.quality, "high") == 0);
  /* /1 keeps rejecting video keys (schema 1 has no video namespace) */
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/1\nvideo.aspect=preserve\n", 41, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_UNKNOWN_KEY && e.line == 2u && strstr(e.what, "NEXTOS_SETTINGS/2") != NULL);
  check_defaults(&s);
  /* enum enforcement, duplicate, unknown video key, WxH validation, typed errors with line */
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nvideo.aspect=keep\n", 36, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_VALUE && e.line == 2u);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nvideo.aspect=stretch\nvideo.aspect=preserve\n", 63, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_DUPLICATE && e.line == 3u);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nvideo.gamma=1\n", 34, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_UNKNOWN_KEY && e.line == 2u);
  CHECK(parse_str("# NEXTOS_SETTINGS/2\nvideo.output_size=800x600\n", &s, NULL, NULL) == 0 && strcmp(s.video_output_size, "800x600") == 0);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nvideo.output_size=0x600\n", 43, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_VALUE);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nvideo.output_size=9000x600\n", 46, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_VALUE);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nvideo.output_size=800x\n", 40, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_VALUE);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/3\n", 20, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_MAGIC && e.line == 1u);
  CHECK(nxcompat_settings_parse2("# NEXTOS_SETTINGS/2\nnovalue\n", 28, &s, &e) != 0 && e.code == NXCOMPAT_SETTINGS_E_SYNTAX && e.line == 2u);
  /* the API-1 entry point on a /2 file: identical result */
  CHECK(parse_str("# NEXTOS_SETTINGS/2\nvideo.aspect=stretch\n", &s, NULL, NULL) == 0 && strcmp(s.video_aspect, "stretch") == 0);
  CHECK(nxcompat_settings_video_value_ok("video.authority", "synchronized") == 1 && nxcompat_settings_video_value_ok("video.authority", "owner") == 0 && nxcompat_settings_video_value_ok("video.filter", "nearest") == 1 && nxcompat_settings_video_value_ok("video.other", "x") == 0);
}

int main(void) {
  test_valid();
  test_unknown_key_rejected_fail_closed();
  test_duplicate_known_key_fatal();
  test_nul_fatal();
  test_oversize_fatal();
  test_bad_magic_fatal();
  test_quality_allowlist();
  test_charset_and_syntax();
  test_schema2();
  /* ---- V5 7A.2 (0.5.1): a `video.invalid_policy` EXECUTADA ---------------
   * A 0.5.0 tipou a chave; quem decidia o que fazer com um arquivo inválido
   * era cada adapter, isto é, cada jogo. Aqui a decisão é do framework. */
  {
    nxcompat_settings_invalid_policy policy;
    nxcompat_settings_error bad;
    nxcompat_settings_recovery got;
    char line[256];
    memset(&bad, 0, sizeof bad);
    bad.code = NXCOMPAT_SETTINGS_E_VALUE;
    bad.line = 4u;

    CHECK_MSG(nxcompat_settings_invalid_policy_from_string("fail_closed", &policy) == 0 &&
              policy == NXCOMPAT_SETTINGS_INVALID_FAIL_CLOSED &&
              nxcompat_settings_invalid_policy_from_string("", &policy) == -1 &&
              nxcompat_settings_invalid_policy_from_string("retry", &policy) == -1,
          "invalid_policy: only the three declared tokens; absent is NOT defaulted");

    CHECK_MSG(nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_FAIL_CLOSED,
                                    &bad, 1, 1, &got) == 0 &&
              got.action == NXCOMPAT_SETTINGS_RECOVER_REFUSE &&
              !got.fell_back && got.owner_bytes_rewritten == 0,
          "fail_closed refuses the launch even with a usable last known good");

    CHECK_MSG(nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD,
                                    &bad, 1, 1, &got) == 0 &&
              got.action == NXCOMPAT_SETTINGS_RECOVER_LAST_KNOWN_GOOD &&
              !got.fell_back,
          "last_known_good reapplies state already applied under the same contract");

    CHECK_MSG(nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD,
                                    &bad, 0, 0, &got) == 0 &&
              got.action == NXCOMPAT_SETTINGS_RECOVER_REFUSE && got.fell_back &&
              strstr(got.reason, "no last known good") != NULL,
          "MUTANT killed: last_known_good with NO stored state falls back to fail closed, and says so");

    CHECK_MSG(nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD,
                                    &bad, 1, 0, &got) == 0 &&
              got.action == NXCOMPAT_SETTINGS_RECOVER_REFUSE && got.fell_back,
          "MUTANT killed: state applied under ANOTHER contract is not a last known good");

    CHECK_MSG(nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT,
                                    &bad, 0, 0, &got) == 0 &&
              got.action == NXCOMPAT_SETTINGS_RECOVER_PACKAGE_DEFAULT &&
              got.owner_bytes_rewritten == 0 &&
              strstr(got.reason, "never copy it over the owner") != NULL,
          "package_default applies the immutable seed WITHOUT copying it over the owner");

    memset(&bad, 0, sizeof bad);
    CHECK_MSG(nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT,
                                    &bad, 1, 1, &got) == -1,
          "MUTANT killed: the error policy is refused on a SUCCESSFUL parse (never a route to drop a valid config)");

    bad.code = NXCOMPAT_SETTINGS_E_SYNTAX;
    bad.line = 9u;
    nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD,
                              &bad, 0, 0, &got);
    CHECK_MSG(nxcompat_settings_recovery_receipt(&got, line, sizeof line) > 0 &&
              strstr(line, "NX-SETTINGS-RECOVERY/1 policy=last_known_good "
                           "action=refuse fell_back=1 error=5 line=9 "
                           "owner_bytes_rewritten=0") != NULL,
          "receipt names the declared policy, the action taken, the fallback and the untouched owner bytes");
  }

  if (failures != 0) {
    fprintf(stderr, "test_settings: %d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  printf("test_settings: PASS\n");
  return EXIT_SUCCESS;
}
