/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_settings.h -- strict NEXTOSSETTINGS.txt parser (V3-SETTINGS-01).
 *
 * Adapters read the settings file themselves (the framework never touches
 * the filesystem here) and hand the raw bytes to this parser.  Format:
 *
 *   # NEXTOS_SETTINGS/1          <- magic, first non-blank line
 *   # any later '#' line is a comment
 *   language=pt-BR
 *   quality=high
 *
 * Rules (strict on purpose -- this file is machine-written):
 *   - at most NXCOMPAT_SETTINGS_MAX_BYTES bytes, strict UTF-8, no NUL;
 *   - values match [A-Za-z0-9._-]{1,32};
 *   - known keys: "language" (any valid value) and "quality"
 *     (auto|low|medium|high);
 *   - an UNKNOWN key is REJECTED fail-closed (V3-SETTINGS-01): the callback
 *     fires once for diagnostics, then the parse fails and `out` is reset to
 *     the safe defaults (the caller keeps its last valid configuration);
 *   - a DUPLICATE occurrence of a known key is fatal;
 *   - any fatal error returns nonzero and leaves the output struct at the
 *     safe defaults: language "auto", quality "auto".
 *
 * The parser only ever reads the caller-provided buffer: no filesystem,
 * no source/eval, no environment.
 */
#ifndef NXCOMPAT_SETTINGS_H
#define NXCOMPAT_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXCOMPAT_SETTINGS_API_VERSION 1u
#define NXCOMPAT_SETTINGS_MAX_BYTES 4096u
#define NXCOMPAT_SETTINGS_MAGIC "# NEXTOS_SETTINGS/1"
#define NXCOMPAT_SETTINGS_VALUE_CAP 33u /* 32 chars + NUL */

typedef struct nxcompat_settings {
  unsigned api_version;                       /* filled by the parser */
  char language[NXCOMPAT_SETTINGS_VALUE_CAP]; /* default "auto" */
  char quality[NXCOMPAT_SETTINGS_VALUE_CAP];  /* default "auto" */
  unsigned unknown_key_count;                 /* diagnostic count */
} nxcompat_settings;

/* Called once per unknown key (key is NOT NUL-terminated; key_len given).
 * Purely diagnostic: an unknown key is REJECTED fail-closed (V3-SETTINGS-01),
 * so the callback fires once and the parse then fails -- never "ignored". */
typedef void (*nxcompat_settings_unknown_key_fn)(const char *key,
                                                 size_t key_len,
                                                 void *user_data);

/*
 * Parse `size` bytes at `buffer`.  Returns 0 on success with `out`
 * filled; nonzero on any fatal error with `out` reset to the safe
 * defaults (language "auto", quality "auto", unknown_key_count 0).
 * `on_unknown_key` may be NULL; an unknown key is fatal (fail-closed)
 * and the callback, when given, fires once before the failure.
 */
int nxcompat_settings_parse(const char *buffer, size_t size,
                            nxcompat_settings *out,
                            nxcompat_settings_unknown_key_fn on_unknown_key,
                            void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* NXCOMPAT_SETTINGS_H */
