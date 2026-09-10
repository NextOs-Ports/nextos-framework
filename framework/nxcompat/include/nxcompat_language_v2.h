/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_language_v2.h -- additive V2 language resolution (V3-SETTINGS-01).
 *
 * The legacy nxcompat_language_select() in nxcompat_language.h stays exactly
 * as it is (env-driven, header-only, house rule #5 enforced there).  This V2
 * API is a PURE, passive resolver: the caller hands every input explicitly,
 * nothing is read from the environment and nothing global is ever mutated
 * (no setenv, no setlocale).  The result is an immutable snapshot the
 * adapter registers into its own sinks at its own lifecycle point.
 *
 * Resolution order (each rung is skipped when empty/"C"/"POSIX"/invalid):
 *   1. session override        (validated launcher/session request)
 *   2. settings file           (NEXTOSSETTINGS.txt "language" key)
 *   3. SDL / firmware locale preference
 *   4. POSIX locale            (READ ONLY -- caller passes the string)
 *   5. declared port fallback  (MUST be a member of `supported`)
 *
 * Script safety: a request that carries a script subtag (explicit, or
 * implied for zh by region: CN/SG => Hans, TW/HK/MO => Hant) never matches
 * a supported tag of a different script, not even by bare language.
 * `zh-Hant` therefore does NOT match a supported list of {zh-CN, zh-Hans};
 * resolution simply continues down the order.
 *
 * Fail-closed: when the declared fallback itself is not a member of the
 * supported list, resolution returns nonzero instead of inventing a tag.
 */
#ifndef NXCOMPAT_LANGUAGE_V2_H
#define NXCOMPAT_LANGUAGE_V2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXCOMPAT_LANGUAGE_V2_API_VERSION 1u

typedef enum nxcompat_language_origin {
  NXCOMPAT_LANGUAGE_ORIGIN_SESSION_OVERRIDE = 0,
  NXCOMPAT_LANGUAGE_ORIGIN_SETTINGS_FILE = 1,
  NXCOMPAT_LANGUAGE_ORIGIN_SDL_FIRMWARE_PREFERENCE = 2,
  NXCOMPAT_LANGUAGE_ORIGIN_POSIX_LOCALE_READONLY = 3,
  NXCOMPAT_LANGUAGE_ORIGIN_PORT_FALLBACK = 4
} nxcompat_language_origin;

/* Immutable resolved snapshot.  All strings are NUL-terminated. */
typedef struct nxcompat_language_snapshot {
  unsigned api_version;           /* NXCOMPAT_LANGUAGE_V2_API_VERSION */
  char requested[36];             /* raw value of the winning rung ("" for
                                   * the fallback rung) */
  nxcompat_language_origin origin;
  char canonical_tag[36];         /* canonical BCP-47, e.g. "pt-BR" */
  char language[9];               /* lowercase primary subtag, "pt" */
  char script[5];                 /* Title-case script or "", "Hant" */
  char region[9];                 /* UPPERCASE region or "", "BR" */
  char underscore_variant[36];    /* "pt_BR" / "zh_Hant_TW" */
  char hyphen_variant[36];        /* "pt-BR" / "zh-Hant-TW" */
  char iso639_3[8];               /* from a small PARTIAL built-in table
                                   * (en pt es fr de it ja zh ru ko);
                                   * "unknown" otherwise */
  char direction[4];              /* "ltr", or "rtl" for ar/he/fa/ur */
  char matched_supported[36];     /* the supported[] entry that matched,
                                   * verbatim as the adapter declared it */
  int used_fallback;              /* 1 when the fallback rung decided */
  char reason[96];                /* short human-readable receipt */
} nxcompat_language_snapshot;

/*
 * Resolve the effective language.  Pure function: no globals, no
 * environment access, no locale.h state mutation.  `posix_locale` is the
 * caller-supplied read-only copy of whatever LANG/LC_ALL held -- this
 * function never calls setenv()/setlocale().
 *
 * Returns 0 and fills `out` on success; nonzero (fail closed, `out` left
 * at safe zeroed defaults with a reason) when no rung can produce a tag
 * that is a member of `supported` -- including a declared fallback that is
 * absent from `supported`.
 */
int nxcompat_language_resolve_v2(const char *session_override,
                                 const char *settings_value,
                                 const char *sdl_preference,
                                 const char *posix_locale,
                                 const char *const *supported,
                                 size_t supported_count,
                                 const char *declared_fallback,
                                 nxcompat_language_snapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* NXCOMPAT_LANGUAGE_V2_H */
