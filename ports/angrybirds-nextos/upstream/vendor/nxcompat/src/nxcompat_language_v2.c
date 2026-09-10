/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_language_v2.c -- pure BCP-47-ish resolver for V3-SETTINGS-01.
 * No globals, no environment access, no locale mutation.  C99.
 */
#include "nxcompat_language_v2.h"

#include <string.h>

/* ---------------------------------------------------------------- utils */

static char nxlang_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char nxlang_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int nxlang_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int nxlang_is_digit(char c) { return c >= '0' && c <= '9'; }

static void nxlang_copy(char *dst, size_t cap, const char *src) {
  size_t i = 0u;
  if (cap == 0u)
    return;
  if (src != NULL) {
    for (; src[i] != '\0' && i + 1u < cap; ++i)
      dst[i] = src[i];
  }
  dst[i] = '\0';
}

static void nxlang_append(char *dst, size_t cap, const char *src) {
  size_t used = strlen(dst);
  size_t i = 0u;
  for (; src[i] != '\0' && used + i + 1u < cap; ++i)
    dst[used + i] = src[i];
  dst[used + i] = '\0';
}

/* ------------------------------------------------------------- parsing */

typedef struct nxlang_tag {
  char language[9]; /* lowercase, 2-3 letters */
  char script[5];   /* Title case, 4 letters, or "" */
  char region[9];   /* UPPER 2 letters or 3 digits, or "" */
} nxlang_tag;

/*
 * Parse "pt_BR.UTF-8", "PT-br", "zh-Hant-TW", ... into canonical parts.
 * Case-insensitive; '_' and '-' both separate; anything from '.' or '@'
 * on (encoding/modifier) is stripped.  Returns 1 on a usable tag, 0 on
 * "no preference" (NULL, empty, "C", "POSIX", "auto") or invalid input.
 */
static int nxlang_parse(const char *in, nxlang_tag *tag) {
  char body[64];
  char part[16];
  size_t i = 0u;
  size_t n = 0u;
  size_t part_index = 0u;

  tag->language[0] = '\0';
  tag->script[0] = '\0';
  tag->region[0] = '\0';

  if (in == NULL || in[0] == '\0')
    return 0;
  for (; in[n] != '\0' && in[n] != '.' && in[n] != '@'; ++n) {
    if (n + 1u >= sizeof body)
      return 0;
    body[n] = in[n];
  }
  body[n] = '\0';
  if (body[0] == '\0')
    return 0;
  if (strcmp(body, "C") == 0 || strcmp(body, "POSIX") == 0)
    return 0;

  while (i <= n) {
    size_t len = 0u;
    while (i < n && body[i] != '-' && body[i] != '_') {
      if (len + 1u >= sizeof part)
        return 0;
      part[len++] = body[i++];
    }
    part[len] = '\0';
    if (i <= n && (i == n || body[i] == '-' || body[i] == '_'))
      ++i;
    if (len == 0u)
      return 0;

    if (part_index == 0u) {
      size_t k;
      if (len < 2u || len > 3u)
        return 0;
      for (k = 0u; k < len; ++k) {
        if (!nxlang_is_alpha(part[k]))
          return 0;
        tag->language[k] = nxlang_lower(part[k]);
      }
      tag->language[len] = '\0';
      if (strcmp(tag->language, "auto") == 0)
        return 0; /* unreachable by length, kept for clarity */
    } else if (len == 4u && nxlang_is_alpha(part[0]) &&
               tag->script[0] == '\0' && tag->region[0] == '\0') {
      size_t k;
      for (k = 0u; k < 4u; ++k) {
        if (!nxlang_is_alpha(part[k]))
          return 0;
        tag->script[k] =
            (k == 0u) ? nxlang_upper(part[k]) : nxlang_lower(part[k]);
      }
      tag->script[4] = '\0';
    } else if (tag->region[0] == '\0' &&
               ((len == 2u && nxlang_is_alpha(part[0]) &&
                 nxlang_is_alpha(part[1])) ||
                (len == 3u && nxlang_is_digit(part[0]) &&
                 nxlang_is_digit(part[1]) && nxlang_is_digit(part[2])))) {
      size_t k;
      for (k = 0u; k < len; ++k)
        tag->region[k] = nxlang_upper(part[k]);
      tag->region[len] = '\0';
    } else {
      /* extra/unknown subtag (variant, extension): ignore, keep parsed */
      break;
    }
    ++part_index;
    if (i > n)
      break;
  }
  return 1;
}

static void nxlang_canonical(const nxlang_tag *tag, char *out, size_t cap,
                             char sep) {
  char sepbuf[2];
  sepbuf[0] = sep;
  sepbuf[1] = '\0';
  nxlang_copy(out, cap, tag->language);
  if (tag->script[0] != '\0') {
    nxlang_append(out, cap, sepbuf);
    nxlang_append(out, cap, tag->script);
  }
  if (tag->region[0] != '\0') {
    nxlang_append(out, cap, sepbuf);
    nxlang_append(out, cap, tag->region);
  }
}

/* Effective script: explicit script, else the zh implication table
 * (CN/SG => Hans, TW/HK/MO => Hant), else "". */
static const char *nxlang_effective_script(const nxlang_tag *tag) {
  if (tag->script[0] != '\0')
    return tag->script;
  if (strcmp(tag->language, "zh") == 0) {
    if (strcmp(tag->region, "CN") == 0 || strcmp(tag->region, "SG") == 0)
      return "Hans";
    if (strcmp(tag->region, "TW") == 0 || strcmp(tag->region, "HK") == 0 ||
        strcmp(tag->region, "MO") == 0)
      return "Hant";
  }
  return "";
}

/* Never cross scripts: compatible when equal, or when either side has no
 * effective script at all. */
static int nxlang_scripts_compatible(const nxlang_tag *a,
                                     const nxlang_tag *b) {
  const char *sa = nxlang_effective_script(a);
  const char *sb = nxlang_effective_script(b);
  if (sa[0] == '\0' || sb[0] == '\0')
    return 1;
  return strcmp(sa, sb) == 0;
}

/* ------------------------------------------------------------- tables */

/* PARTIAL ISO-639-1 -> ISO-639-3 table.  Deliberately small: only the
 * languages the ports catalogue actually ships today.  Anything else is
 * reported as "unknown" -- extend here when a port needs more. */
static const char *nxlang_iso639_3(const char *language) {
  static const char *const table[][2] = {
      {"en", "eng"}, {"pt", "por"}, {"es", "spa"}, {"fr", "fra"},
      {"de", "deu"}, {"it", "ita"}, {"ja", "jpn"}, {"zh", "zho"},
      {"ru", "rus"}, {"ko", "kor"},
  };
  size_t i;
  for (i = 0u; i < sizeof table / sizeof table[0]; ++i)
    if (strcmp(table[i][0], language) == 0)
      return table[i][1];
  return "unknown";
}

static const char *nxlang_direction(const char *language) {
  static const char *const rtl[] = {"ar", "he", "fa", "ur"};
  size_t i;
  for (i = 0u; i < sizeof rtl / sizeof rtl[0]; ++i)
    if (strcmp(rtl[i], language) == 0)
      return "rtl";
  return "ltr";
}

/* ------------------------------------------------------------ matching */

/* exact > language+script > language+region > bare language;
 * returns the index into supported[] or (size_t)-1. */
static size_t nxlang_match(const nxlang_tag *want, char canonical_cap36[36],
                           const char *const *supported,
                           size_t supported_count) {
  size_t best = (size_t)-1;
  int best_rank = 0; /* 4 exact, 3 lang+script, 2 lang+region, 1 bare */
  size_t i;

  for (i = 0u; i < supported_count; ++i) {
    nxlang_tag have;
    char have_canon[36];
    int rank = 0;
    if (!nxlang_parse(supported[i], &have))
      continue;
    if (strcmp(want->language, have.language) != 0)
      continue;
    nxlang_canonical(&have, have_canon, sizeof have_canon, '-');
    if (strcmp(canonical_cap36, have_canon) == 0) {
      rank = 4;
    } else if (want->script[0] != '\0' &&
               strcmp(want->script, have.script) == 0) {
      rank = 3;
    } else if (want->region[0] != '\0' &&
               strcmp(want->region, have.region) == 0 &&
               nxlang_scripts_compatible(want, &have)) {
      rank = 2;
    } else if (nxlang_scripts_compatible(want, &have)) {
      rank = 1;
    } else {
      continue; /* script mismatch: never cross scripts */
    }
    if (rank > best_rank) {
      best_rank = rank;
      best = i;
    }
  }
  return best;
}

/* ------------------------------------------------------------- fill-in */

static void nxlang_fill(nxcompat_language_snapshot *out,
                        const nxlang_tag *tag, const char *requested,
                        nxcompat_language_origin origin,
                        const char *matched_supported, int used_fallback,
                        const char *reason) {
  memset(out, 0, sizeof *out);
  out->api_version = NXCOMPAT_LANGUAGE_V2_API_VERSION;
  nxlang_copy(out->requested, sizeof out->requested,
              requested != NULL ? requested : "");
  out->origin = origin;
  nxlang_canonical(tag, out->canonical_tag, sizeof out->canonical_tag, '-');
  nxlang_copy(out->language, sizeof out->language, tag->language);
  nxlang_copy(out->script, sizeof out->script, tag->script);
  nxlang_copy(out->region, sizeof out->region, tag->region);
  nxlang_canonical(tag, out->underscore_variant,
                   sizeof out->underscore_variant, '_');
  nxlang_canonical(tag, out->hyphen_variant, sizeof out->hyphen_variant,
                   '-');
  nxlang_copy(out->iso639_3, sizeof out->iso639_3,
              nxlang_iso639_3(tag->language));
  nxlang_copy(out->direction, sizeof out->direction,
              nxlang_direction(tag->language));
  nxlang_copy(out->matched_supported, sizeof out->matched_supported,
              matched_supported);
  out->used_fallback = used_fallback;
  nxlang_copy(out->reason, sizeof out->reason, reason);
}

/* ------------------------------------------------------------- resolve */

int nxcompat_language_resolve_v2(const char *session_override,
                                 const char *settings_value,
                                 const char *sdl_preference,
                                 const char *posix_locale,
                                 const char *const *supported,
                                 size_t supported_count,
                                 const char *declared_fallback,
                                 nxcompat_language_snapshot *out) {
  const char *rung_value[4];
  const nxcompat_language_origin rung_origin[4] = {
      NXCOMPAT_LANGUAGE_ORIGIN_SESSION_OVERRIDE,
      NXCOMPAT_LANGUAGE_ORIGIN_SETTINGS_FILE,
      NXCOMPAT_LANGUAGE_ORIGIN_SDL_FIRMWARE_PREFERENCE,
      NXCOMPAT_LANGUAGE_ORIGIN_POSIX_LOCALE_READONLY};
  static const char *const rung_name[4] = {
      "session-override", "settings-file", "sdl-firmware-preference",
      "posix-locale"};
  size_t r;
  nxlang_tag fb_tag;
  char fb_canon[36];
  size_t fb_index;

  if (out == NULL)
    return 1;
  memset(out, 0, sizeof *out);
  out->api_version = NXCOMPAT_LANGUAGE_V2_API_VERSION;
  nxlang_copy(out->reason, sizeof out->reason, "fail-closed: bad inputs");
  if (supported == NULL || supported_count == 0u)
    return 1;

  rung_value[0] = session_override;
  rung_value[1] = settings_value;
  rung_value[2] = sdl_preference;
  rung_value[3] = posix_locale;

  for (r = 0u; r < 4u; ++r) {
    nxlang_tag want;
    char canon[36];
    size_t hit;
    if (rung_value[r] != NULL &&
        strcmp(rung_value[r], "auto") == 0)
      continue; /* explicit "no preference" */
    if (!nxlang_parse(rung_value[r], &want))
      continue; /* empty / C / POSIX / invalid: skip this rung */
    nxlang_canonical(&want, canon, sizeof canon, '-');
    hit = nxlang_match(&want, canon, supported, supported_count);
    if (hit == (size_t)-1)
      continue; /* valid preference, nothing compatible: keep walking */
    {
      char reason[96];
      nxlang_copy(reason, sizeof reason, rung_name[r]);
      nxlang_append(reason, sizeof reason, ": matched supported tag");
      nxlang_fill(out, &want, rung_value[r], rung_origin[r],
                  supported[hit], 0, reason);
    }
    return 0;
  }

  /* Rung 5: declared fallback.  It MUST be a member of supported --
   * matched by exact canonical tag -- otherwise fail closed. */
  if (!nxlang_parse(declared_fallback, &fb_tag)) {
    nxlang_copy(out->reason, sizeof out->reason,
                "fail-closed: declared fallback unparseable");
    return 1;
  }
  nxlang_canonical(&fb_tag, fb_canon, sizeof fb_canon, '-');
  fb_index = (size_t)-1;
  for (r = 0u; r < supported_count; ++r) {
    nxlang_tag have;
    char have_canon[36];
    if (!nxlang_parse(supported[r], &have))
      continue;
    nxlang_canonical(&have, have_canon, sizeof have_canon, '-');
    if (strcmp(fb_canon, have_canon) == 0) {
      fb_index = r;
      break;
    }
  }
  if (fb_index == (size_t)-1) {
    nxlang_copy(out->reason, sizeof out->reason,
                "fail-closed: declared fallback not in supported list");
    return 1;
  }
  nxlang_fill(out, &fb_tag, "", NXCOMPAT_LANGUAGE_ORIGIN_PORT_FALLBACK,
              supported[fb_index], 1,
              "port-fallback: no earlier rung matched");
  return 0;
}
