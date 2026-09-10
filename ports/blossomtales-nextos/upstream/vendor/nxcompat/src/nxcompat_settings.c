/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_settings.c -- strict NEXTOSSETTINGS.txt parser.
 * Buffer in, struct out.  No filesystem, no eval, no globals.  C99.
 */
#include "nxcompat_settings.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- utils */

static void nxset_defaults(nxcompat_settings *out) {
  memset(out, 0, sizeof *out);
  out->api_version = NXCOMPAT_SETTINGS_API_VERSION;
  memcpy(out->language, "auto", 5u);
  memcpy(out->quality, "auto", 5u);
  out->unknown_key_count = 0u;
}

static int nxset_value_char_ok(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

/* Strict UTF-8: rejects NUL, overlong forms, surrogates, > U+10FFFF. */
static int nxset_utf8_valid(const unsigned char *p, size_t n) {
  size_t i = 0u;
  while (i < n) {
    unsigned char c = p[i];
    if (c == 0u)
      return 0;
    if (c < 0x80u) {
      ++i;
    } else if ((c & 0xE0u) == 0xC0u) {
      if (i + 1u >= n || (p[i + 1u] & 0xC0u) != 0x80u || c < 0xC2u)
        return 0;
      i += 2u;
    } else if ((c & 0xF0u) == 0xE0u) {
      if (i + 2u >= n || (p[i + 1u] & 0xC0u) != 0x80u ||
          (p[i + 2u] & 0xC0u) != 0x80u)
        return 0;
      if (c == 0xE0u && p[i + 1u] < 0xA0u)
        return 0; /* overlong */
      if (c == 0xEDu && p[i + 1u] >= 0xA0u)
        return 0; /* surrogate */
      i += 3u;
    } else if ((c & 0xF8u) == 0xF0u) {
      if (i + 3u >= n || (p[i + 1u] & 0xC0u) != 0x80u ||
          (p[i + 2u] & 0xC0u) != 0x80u || (p[i + 3u] & 0xC0u) != 0x80u)
        return 0;
      if (c == 0xF0u && p[i + 1u] < 0x90u)
        return 0; /* overlong */
      if (c > 0xF4u || (c == 0xF4u && p[i + 1u] >= 0x90u))
        return 0; /* > U+10FFFF */
      i += 4u;
    } else {
      return 0;
    }
  }
  return 1;
}

/* Line is blank (only spaces/tabs)? */
static int nxset_line_blank(const char *line, size_t len) {
  size_t i;
  for (i = 0u; i < len; ++i)
    if (line[i] != ' ' && line[i] != '\t')
      return 0;
  return 1;
}

static int nxset_in(const char *value, const char *const *allowed) {
  size_t i;
  for (i = 0u; allowed[i] != NULL; ++i)
    if (strcmp(value, allowed[i]) == 0)
      return 1;
  return 0;
}

static int nxset_wxh_ok(const char *v) {
  unsigned long w = 0ul, h = 0ul; size_t i = 0u, n = strlen(v);
  if (n < 3u || n > 9u) return 0;
  while (i < n && v[i] >= '0' && v[i] <= '9') { w = w * 10ul + (unsigned long)(v[i] - '0'); ++i; }
  if (i == 0u || i >= n || v[i] != 'x') return 0;
  ++i;
  if (i >= n) return 0;
  while (i < n && v[i] >= '0' && v[i] <= '9') { h = h * 10ul + (unsigned long)(v[i] - '0'); ++i; }
  if (i != n) return 0;
  return w >= 1ul && w <= 8192ul && h >= 1ul && h <= 8192ul;
}

int nxcompat_settings_video_value_ok(const char *key, const char *value) {
  static const char *const authority[] = {"nextos", "engine", "synchronized", NULL};
  static const char *const output[] = {"auto", "display", "640x480", "1280x720", "1920x1080", NULL};
  static const char *const aspect[] = {"auto", "engine", "preserve", "stretch", "crop", "integer", NULL};
  static const char *const filter[] = {"engine", "nearest", "linear", NULL};
  static const char *const invalid[] = {"fail_closed", "last_known_good", "package_default", NULL};
  if (key == NULL || value == NULL) return 0;
  if (strcmp(key, "video.authority") == 0) return nxset_in(value, authority);
  if (strcmp(key, "video.output_size") == 0) return nxset_in(value, output) || nxset_wxh_ok(value);
  if (strcmp(key, "video.aspect") == 0) return nxset_in(value, aspect);
  if (strcmp(key, "video.filter") == 0) return nxset_in(value, filter);
  if (strcmp(key, "video.invalid_policy") == 0) return nxset_in(value, invalid);
  return 0;
}

static char *nxset_video_slot(nxcompat_settings *s, const char *key) {
  if (strcmp(key, "video.authority") == 0) return s->video_authority;
  if (strcmp(key, "video.output_size") == 0) return s->video_output_size;
  if (strcmp(key, "video.aspect") == 0) return s->video_aspect;
  if (strcmp(key, "video.filter") == 0) return s->video_filter;
  if (strcmp(key, "video.invalid_policy") == 0) return s->video_invalid_policy;
  return NULL;
}

static void nxset_err(nxcompat_settings_error *e, int code, unsigned line, const char *what) {
  if (e == NULL) return;
  e->code = code; e->line = line;
  snprintf(e->what, sizeof e->what, "%s", what);
}

static int nxset_quality_allowed(const char *value) {
  return strcmp(value, "auto") == 0 || strcmp(value, "low") == 0 ||
         strcmp(value, "medium") == 0 || strcmp(value, "high") == 0;
}

/* ---------------------------------------------------------------- parse */

static int nxset_parse(const char *buffer, size_t size,
                       nxcompat_settings *out,
                       nxcompat_settings_unknown_key_fn on_unknown_key,
                       void *user_data, nxcompat_settings_error *err) {
  nxcompat_settings tmp;
  size_t pos = 0u;
  unsigned lineno = 0u;
  int magic_seen = 0;
  int have_language = 0;
  int have_quality = 0;

  nxset_err(err, NXCOMPAT_SETTINGS_OK, 0u, "");
  if (out == NULL) {
    nxset_err(err, NXCOMPAT_SETTINGS_E_ARGS, 0u, "no output");
    return 1;
  }
  nxset_defaults(out);
  nxset_defaults(&tmp);
  if (buffer == NULL && size != 0u) {
    nxset_err(err, NXCOMPAT_SETTINGS_E_ARGS, 0u, "no buffer");
    return 1;
  }
  if (size > NXCOMPAT_SETTINGS_MAX_BYTES) {
    nxset_err(err, NXCOMPAT_SETTINGS_E_OVERSIZE, 0u, "file larger than 4096 bytes");
    return 1; /* oversize is fatal */
  }
  if (size != 0u && !nxset_utf8_valid((const unsigned char *)buffer, size)) {
    nxset_err(err, NXCOMPAT_SETTINGS_E_ENCODING, 0u, "embedded NUL or malformed UTF-8");
    return 1; /* embedded NUL or malformed UTF-8 is fatal */
  }

  while (pos < size) {
    const char *line = buffer + pos;
    size_t len = 0u;
    size_t eq;
    size_t key_len;
    const char *value;
    size_t value_len;
    size_t i;

    while (pos + len < size && buffer[pos + len] != '\n')
      ++len;
    pos += len;
    if (pos < size)
      ++pos; /* skip '\n' */
    ++lineno;
    if (len > 0u && line[len - 1u] == '\r')
      --len; /* tolerate CRLF */

    if (nxset_line_blank(line, len))
      continue;

    if (!magic_seen) {
      /* the first non-blank line must be exactly one of the magics */
      size_t magic_len = sizeof NXCOMPAT_SETTINGS_MAGIC - 1u;
      if (len == magic_len && memcmp(line, NXCOMPAT_SETTINGS_MAGIC, magic_len) == 0)
        tmp.schema = 1u;
      else if (len == magic_len && memcmp(line, NXCOMPAT_SETTINGS_MAGIC_2, magic_len) == 0)
        tmp.schema = 2u;
      else {
        nxset_err(err, NXCOMPAT_SETTINGS_E_MAGIC, lineno, "first line must be `# NEXTOS_SETTINGS/1` or `# NEXTOS_SETTINGS/2`");
        return 1;
      }
      magic_seen = 1;
      continue;
    }

    if (line[0] == '#')
      continue; /* comment */

    /* key=value, no surrounding whitespace */
    eq = 0u;
    while (eq < len && line[eq] != '=')
      ++eq;
    if (eq == 0u || eq >= len) {
      nxset_err(err, NXCOMPAT_SETTINGS_E_SYNTAX, lineno, "expected key=value");
      return 1; /* missing key or missing '=' */
    }
    key_len = eq;
    value = line + eq + 1u;
    value_len = len - eq - 1u;

    for (i = 0u; i < key_len; ++i)
      if (!nxset_value_char_ok((unsigned char)line[i])) {
        nxset_err(err, NXCOMPAT_SETTINGS_E_SYNTAX, lineno, "key outside [A-Za-z0-9._-]");
        return 1; /* malformed key */
      }
    if (key_len > 32u) {
      nxset_err(err, NXCOMPAT_SETTINGS_E_SYNTAX, lineno, "key longer than 32");
      return 1;
    }
    if (value_len < 1u || value_len > 32u) {
      nxset_err(err, NXCOMPAT_SETTINGS_E_SYNTAX, lineno, "value must be 1..32 characters");
      return 1;
    }
    for (i = 0u; i < value_len; ++i)
      if (!nxset_value_char_ok((unsigned char)value[i])) {
        nxset_err(err, NXCOMPAT_SETTINGS_E_SYNTAX, lineno, "value outside [A-Za-z0-9._-]");
        return 1; /* value outside [A-Za-z0-9._-] */
      }

    if (key_len == 8u && memcmp(line, "language", 8u) == 0) {
      if (have_language) {
        nxset_err(err, NXCOMPAT_SETTINGS_E_DUPLICATE, lineno, "duplicate key: language");
        return 1; /* duplicate known key is fatal */
      }
      have_language = 1;
      memcpy(tmp.language, value, value_len);
      tmp.language[value_len] = '\0';
    } else if (key_len == 7u && memcmp(line, "quality", 7u) == 0) {
      char quality[NXCOMPAT_SETTINGS_VALUE_CAP];
      if (have_quality) {
        nxset_err(err, NXCOMPAT_SETTINGS_E_DUPLICATE, lineno, "duplicate key: quality");
        return 1; /* duplicate known key is fatal */
      }
      have_quality = 1;
      memcpy(quality, value, value_len);
      quality[value_len] = '\0';
      if (!nxset_quality_allowed(quality)) {
        nxset_err(err, NXCOMPAT_SETTINGS_E_VALUE, lineno, "quality must be auto|low|medium|high");
        return 1; /* allowlist: auto|low|medium|high */
      }
      memcpy(tmp.quality, quality, value_len + 1u);
    } else if (tmp.schema == 2u && key_len > 6u && key_len < 32u &&
               memcmp(line, "video.", 6u) == 0) {
      char key[33]; char val[NXCOMPAT_SETTINGS_VALUE_CAP]; char *slot;
      memcpy(key, line, key_len); key[key_len] = '\0';
      memcpy(val, value, value_len); val[value_len] = '\0';
      slot = nxset_video_slot(&tmp, key);
      if (slot == NULL) {
        tmp.unknown_key_count += 1u;
        if (on_unknown_key != NULL)
          on_unknown_key(line, key_len, user_data);
        nxset_err(err, NXCOMPAT_SETTINGS_E_UNKNOWN_KEY, lineno, "unknown video key");
        return 1;
      }
      if (slot[0] != '\0') {
        nxset_err(err, NXCOMPAT_SETTINGS_E_DUPLICATE, lineno, "duplicate video key");
        return 1;
      }
      if (!nxcompat_settings_video_value_ok(key, val)) {
        nxset_err(err, NXCOMPAT_SETTINGS_E_VALUE, lineno, "video value outside the schema /2 enum");
        return 1;
      }
      memcpy(slot, val, value_len + 1u);
    } else {
      /* V3-SETTINGS-01 (spec): "campos desconhecidos rejeitados com ultimo
       * valor valido / fallback seguro". A parser of this schema version
       * FAILS CLOSED on any key it does not know; `out` stays at the safe
       * defaults. The callback fires first, purely as diagnostics, never to
       * signal "ignored". */
      tmp.unknown_key_count += 1u;
      if (on_unknown_key != NULL)
        on_unknown_key(line, key_len, user_data);
      nxset_err(err, NXCOMPAT_SETTINGS_E_UNKNOWN_KEY, lineno,
                tmp.schema == 1u && key_len > 6u && memcmp(line, "video.", 6u) == 0
                    ? "video keys need `# NEXTOS_SETTINGS/2`" : "unknown key");
      return 1; /* unknown key is fatal */
    }
  }

  if (!magic_seen) {
    nxset_err(err, NXCOMPAT_SETTINGS_E_MAGIC, 0u, "empty file / no magic");
    return 1; /* empty file / no magic */
  }
  *out = tmp;
  return 0;
}

int nxcompat_settings_parse(const char *buffer, size_t size,
                            nxcompat_settings *out,
                            nxcompat_settings_unknown_key_fn on_unknown_key,
                            void *user_data) {
  return nxset_parse(buffer, size, out, on_unknown_key, user_data, NULL);
}

int nxcompat_settings_parse2(const char *buffer, size_t size,
                             nxcompat_settings *out,
                             nxcompat_settings_error *err) {
  return nxset_parse(buffer, size, out, NULL, NULL, err);
}

/* ===================================================================== *
 * nxcompat 0.5.1 / V5 7A.2: execução da `video.invalid_policy`.
 * Pura: uma política declarada e um erro de parse entram, uma ação sai.
 * Nenhum caminho toca em arquivo -- e nenhum reescreve os bytes do dono.
 * ===================================================================== */

int nxcompat_settings_invalid_policy_from_string(
    const char *token, nxcompat_settings_invalid_policy *out) {
  if (token == NULL || out == NULL) return -1;
  if (strcmp(token, "fail_closed") == 0)
    *out = NXCOMPAT_SETTINGS_INVALID_FAIL_CLOSED;
  else if (strcmp(token, "last_known_good") == 0)
    *out = NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD;
  else if (strcmp(token, "package_default") == 0)
    *out = NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT;
  else
    return -1;
  return 0;
}

const char *nxcompat_settings_invalid_policy_name(
    nxcompat_settings_invalid_policy policy) {
  switch (policy) {
    case NXCOMPAT_SETTINGS_INVALID_FAIL_CLOSED: return "fail_closed";
    case NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD: return "last_known_good";
    case NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT: return "package_default";
    default: return "?";
  }
}

const char *nxcompat_settings_recovery_action_name(
    nxcompat_settings_recovery_action action) {
  switch (action) {
    case NXCOMPAT_SETTINGS_RECOVER_REFUSE: return "refuse";
    case NXCOMPAT_SETTINGS_RECOVER_LAST_KNOWN_GOOD: return "last-known-good";
    case NXCOMPAT_SETTINGS_RECOVER_PACKAGE_DEFAULT: return "package-default";
    default: return "?";
  }
}

int nxcompat_settings_recover(nxcompat_settings_invalid_policy policy,
                              const nxcompat_settings_error *err,
                              int last_known_good_available,
                              int last_known_good_same_contract,
                              nxcompat_settings_recovery *out) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof *out);
  out->api_version = NXCOMPAT_SETTINGS_RECOVERY_API_VERSION;
  out->policy = policy;
  out->owner_bytes_rewritten = 0;   /* invariant of every branch below */
  /* The error policy is never a route to discard a VALID configuration. */
  if (err == NULL || err->code == NXCOMPAT_SETTINGS_OK) return -1;
  out->error_code = err->code;
  out->error_line = err->line;
  switch (policy) {
    case NXCOMPAT_SETTINGS_INVALID_FAIL_CLOSED:
      out->action = NXCOMPAT_SETTINGS_RECOVER_REFUSE;
      snprintf(out->reason, sizeof out->reason,
               "fail_closed: refuse the launch, owner bytes preserved");
      break;
    case NXCOMPAT_SETTINGS_INVALID_LAST_KNOWN_GOOD:
      if (last_known_good_available && last_known_good_same_contract) {
        out->action = NXCOMPAT_SETTINGS_RECOVER_LAST_KNOWN_GOOD;
        snprintf(out->reason, sizeof out->reason,
                 "last_known_good: reapply the state already applied");
      } else {
        out->action = NXCOMPAT_SETTINGS_RECOVER_REFUSE;
        out->fell_back = 1;
        snprintf(out->reason, sizeof out->reason,
                 last_known_good_available
                     ? "last_known_good under another contract: fail closed"
                     : "no last known good exists: fail closed");
      }
      break;
    case NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT:
      out->action = NXCOMPAT_SETTINGS_RECOVER_PACKAGE_DEFAULT;
      snprintf(out->reason, sizeof out->reason,
               "package_default: apply the immutable seed, never copy it over "
               "the owner");
      break;
    default:
      out->action = NXCOMPAT_SETTINGS_RECOVER_REFUSE;
      out->fell_back = 1;
      snprintf(out->reason, sizeof out->reason, "unknown policy: fail closed");
      break;
  }
  return 0;
}

int nxcompat_settings_recovery_receipt(const nxcompat_settings_recovery *d,
                                       char *out, size_t cap) {
  int n;
  if (d == NULL || out == NULL || cap == 0u) return -1;
  n = snprintf(out, cap,
               "NX-SETTINGS-RECOVERY/1 policy=%s action=%s fell_back=%d "
               "error=%d line=%u owner_bytes_rewritten=%d reason=%s",
               nxcompat_settings_invalid_policy_name(d->policy),
               nxcompat_settings_recovery_action_name(d->action),
               d->fell_back, d->error_code, d->error_line,
               d->owner_bytes_rewritten, d->reason);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}
