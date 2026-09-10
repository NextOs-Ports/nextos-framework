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

#define NXCOMPAT_SETTINGS_API_VERSION 2u
#define NXCOMPAT_SETTINGS_MAX_BYTES 4096u
#define NXCOMPAT_SETTINGS_MAGIC "# NEXTOS_SETTINGS/1"
#define NXCOMPAT_SETTINGS_MAGIC_2 "# NEXTOS_SETTINGS/2"
#define NXCOMPAT_SETTINGS_VALUE_CAP 33u /* 32 chars + NUL */

/*
 * NEXTOS_SETTINGS/2 (nxcompat 0.5.0, V5 mission 7A.3): SAME grammar as /1
 * (`key=value`, `[A-Za-z0-9._-]`, magic first, unknown key fatal, duplicate
 * fatal) plus the typed video namespace, decided by the owner of NextOS on
 * 2026-09-03 to live INSIDE this file (no NEXTOSVIDEO.cfg):
 *
 *   video.authority=nextos|engine|synchronized
 *   video.output_size=auto|display|640x480|1280x720|1920x1080|<WxH>
 *   video.aspect=auto|engine|preserve|stretch|crop|integer
 *   video.filter=engine|nearest|linear
 *   video.invalid_policy=fail_closed|last_known_good|package_default
 *
 * A /1 file never carries video keys (they are unknown there => fatal).
 * In a /2 file every video key is OPTIONAL; an absent key reads as "" and
 * the adapter applies the package default for it (never a hidden value).
 * `<WxH>` is validated: 1..8192 each, digits only.
 */
typedef struct nxcompat_settings {
  unsigned api_version;                       /* filled by the parser */
  char language[NXCOMPAT_SETTINGS_VALUE_CAP]; /* default "auto" */
  char quality[NXCOMPAT_SETTINGS_VALUE_CAP];  /* default "auto" */
  unsigned unknown_key_count;                 /* diagnostic count */
  /* --- API 2 (additive) --- */
  unsigned schema;                            /* 1 or 2; 0 on failure */
  char video_authority[NXCOMPAT_SETTINGS_VALUE_CAP];   /* "" = absent */
  char video_output_size[NXCOMPAT_SETTINGS_VALUE_CAP];
  char video_aspect[NXCOMPAT_SETTINGS_VALUE_CAP];
  char video_filter[NXCOMPAT_SETTINGS_VALUE_CAP];
  char video_invalid_policy[NXCOMPAT_SETTINGS_VALUE_CAP];
} nxcompat_settings;

/* Typed diagnostics (API 2). `line` is 1-based; 0 = whole file. */
enum {
  NXCOMPAT_SETTINGS_OK = 0,
  NXCOMPAT_SETTINGS_E_ARGS = 1,
  NXCOMPAT_SETTINGS_E_OVERSIZE = 2,
  NXCOMPAT_SETTINGS_E_ENCODING = 3,
  NXCOMPAT_SETTINGS_E_MAGIC = 4,
  NXCOMPAT_SETTINGS_E_SYNTAX = 5,
  NXCOMPAT_SETTINGS_E_DUPLICATE = 6,
  NXCOMPAT_SETTINGS_E_UNKNOWN_KEY = 7,
  NXCOMPAT_SETTINGS_E_VALUE = 8
};
typedef struct nxcompat_settings_error {
  int code; unsigned line; char what[96];
} nxcompat_settings_error;

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

/* API 2: same contract, with a typed error (line + reason) so the launcher
 * can print `NEXTOSSETTINGS.txt:<line>: <reason>` and keep the owner's
 * bytes untouched. `err` may be NULL. */
int nxcompat_settings_parse2(const char *buffer, size_t size,
                             nxcompat_settings *out,
                             nxcompat_settings_error *err);
/* Validate one video value for one video key (both NUL-terminated). 1 = ok. */
int nxcompat_settings_video_value_ok(const char *key, const char *value);

/* ===================================================================== *
 * nxcompat 0.5.1 / V5 7A.2: `video.invalid_policy` EXECUTADA, no framework
 * ---------------------------------------------------------------------
 * A 0.5.0 tipou a chave e o parser passou a devolver linha/motivo, mas quem
 * DECIDIA o que fazer com um arquivo inválido era cada adapter -- ou seja,
 * cada jogo. Regra do NextOS: se vale para mais de um jogo, mora aqui.
 *
 * Contrato (7A.2), executável e sem I/O:
 *   fail_closed        recusa a abertura, preservando os bytes do dono;
 *   last_known_good    aplica o estado APLICADO anteriormente -- e SÓ é
 *                      válido se esse estado existir SOB O MESMO CONTRATO;
 *                      sem ele, cai para fail_closed e diz que caiu;
 *   package_default    aplica a semente imutável do pacote, SEM copiá-la por
 *                      cima do arquivo do dono.
 * Em nenhum caminho os bytes do dono são reescritos: a decisão carrega
 * `owner_bytes_rewritten` sempre 0 e o receipt o afirma.
 * Chamar a recuperação com um parse BEM-SUCEDIDO é recusado: a política de
 * erro nunca pode virar rota para descartar uma configuração válida.
 * ===================================================================== */

#define NXCOMPAT_SETTINGS_RECOVERY_API_VERSION 1u

typedef enum nxcompat_settings_invalid_policy {
  NXCOMPAT_SETTINGS_INVALID_FAIL_CLOSED = 0,
  NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD,
  NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT
} nxcompat_settings_invalid_policy;

typedef enum nxcompat_settings_recovery_action {
  NXCOMPAT_SETTINGS_RECOVER_REFUSE = 0,  /* do not launch */
  NXCOMPAT_SETTINGS_RECOVER_LAST_KNOWN_GOOD,
  NXCOMPAT_SETTINGS_RECOVER_PACKAGE_DEFAULT
} nxcompat_settings_recovery_action;

typedef struct nxcompat_settings_recovery {
  unsigned api_version;
  nxcompat_settings_invalid_policy policy;   /* what the port declared */
  nxcompat_settings_recovery_action action;  /* what must happen now */
  int owner_bytes_rewritten;                 /* always 0, stated on purpose */
  int fell_back;                             /* 1 = policy could not be honoured */
  int error_code;                            /* the parse error that got here */
  unsigned error_line;
  char reason[96];
} nxcompat_settings_recovery;

/* Parse the declared token. 0 on success, -1 on an unknown token
 * (an absent/empty token is NOT defaulted: the port must declare one). */
int nxcompat_settings_invalid_policy_from_string(
    const char *token, nxcompat_settings_invalid_policy *out);
const char *nxcompat_settings_invalid_policy_name(
    nxcompat_settings_invalid_policy policy);
const char *nxcompat_settings_recovery_action_name(
    nxcompat_settings_recovery_action action);

/* Decide what to do after a FAILED parse2. `err` must carry a nonzero code.
 * `last_known_good_available` is 1 when applied state exists;
 * `last_known_good_same_contract` is 1 when that state was applied under the
 * SAME contract/provider as now. Returns 0 when `out` holds a usable
 * decision (including a deliberate REFUSE), -1 when the arguments themselves
 * are wrong (no error to recover from, or a NULL out). */
int nxcompat_settings_recover(nxcompat_settings_invalid_policy policy,
                              const nxcompat_settings_error *err,
                              int last_known_good_available,
                              int last_known_good_same_contract,
                              nxcompat_settings_recovery *out);

/* `NX-SETTINGS-RECOVERY/1 policy=.. action=.. fell_back=.. error=<code>
 * line=<n> owner_bytes_rewritten=0 reason=..` */
int nxcompat_settings_recovery_receipt(const nxcompat_settings_recovery *d,
                                       char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NXCOMPAT_SETTINGS_H */
