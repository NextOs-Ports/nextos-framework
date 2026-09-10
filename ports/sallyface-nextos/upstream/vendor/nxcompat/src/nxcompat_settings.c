/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_settings.c -- strict NEXTOSSETTINGS.txt parser.
 * Buffer in, struct out.  No filesystem, no eval, no globals.  C99.
 */
#include "nxcompat_settings.h"

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

static int nxset_quality_allowed(const char *value) {
  return strcmp(value, "auto") == 0 || strcmp(value, "low") == 0 ||
         strcmp(value, "medium") == 0 || strcmp(value, "high") == 0;
}

/* ---------------------------------------------------------------- parse */

int nxcompat_settings_parse(const char *buffer, size_t size,
                            nxcompat_settings *out,
                            nxcompat_settings_unknown_key_fn on_unknown_key,
                            void *user_data) {
  nxcompat_settings tmp;
  size_t pos = 0u;
  int magic_seen = 0;
  int have_language = 0;
  int have_quality = 0;

  if (out == NULL)
    return 1;
  nxset_defaults(out);
  nxset_defaults(&tmp);
  if (buffer == NULL && size != 0u)
    return 1;
  if (size > NXCOMPAT_SETTINGS_MAX_BYTES)
    return 1; /* oversize is fatal */
  if (size != 0u && !nxset_utf8_valid((const unsigned char *)buffer, size))
    return 1; /* embedded NUL or malformed UTF-8 is fatal */

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
    if (len > 0u && line[len - 1u] == '\r')
      --len; /* tolerate CRLF */

    if (nxset_line_blank(line, len))
      continue;

    if (!magic_seen) {
      /* the first non-blank line must be exactly the magic */
      size_t magic_len = sizeof NXCOMPAT_SETTINGS_MAGIC - 1u;
      if (len != magic_len ||
          memcmp(line, NXCOMPAT_SETTINGS_MAGIC, magic_len) != 0)
        return 1;
      magic_seen = 1;
      continue;
    }

    if (line[0] == '#')
      continue; /* comment */

    /* key=value, no surrounding whitespace */
    eq = 0u;
    while (eq < len && line[eq] != '=')
      ++eq;
    if (eq == 0u || eq >= len)
      return 1; /* missing key or missing '=' */
    key_len = eq;
    value = line + eq + 1u;
    value_len = len - eq - 1u;

    for (i = 0u; i < key_len; ++i)
      if (!nxset_value_char_ok((unsigned char)line[i]))
        return 1; /* malformed key */
    if (key_len > 32u)
      return 1;
    if (value_len < 1u || value_len > 32u)
      return 1;
    for (i = 0u; i < value_len; ++i)
      if (!nxset_value_char_ok((unsigned char)value[i]))
        return 1; /* value outside [A-Za-z0-9._-] */

    if (key_len == 8u && memcmp(line, "language", 8u) == 0) {
      if (have_language)
        return 1; /* duplicate known key is fatal */
      have_language = 1;
      memcpy(tmp.language, value, value_len);
      tmp.language[value_len] = '\0';
    } else if (key_len == 7u && memcmp(line, "quality", 7u) == 0) {
      char quality[NXCOMPAT_SETTINGS_VALUE_CAP];
      if (have_quality)
        return 1; /* duplicate known key is fatal */
      have_quality = 1;
      memcpy(quality, value, value_len);
      quality[value_len] = '\0';
      if (!nxset_quality_allowed(quality))
        return 1; /* allowlist: auto|low|medium|high */
      memcpy(tmp.quality, quality, value_len + 1u);
    } else {
      /* V3-SETTINGS-01 (spec): "campos desconhecidos rejeitados com ultimo
       * valor valido / fallback seguro". A parser of this schema version
       * FAILS CLOSED on any key it does not know; `out` stays at the safe
       * defaults. The callback fires first, purely as diagnostics, never to
       * signal "ignored". */
      tmp.unknown_key_count += 1u;
      if (on_unknown_key != NULL)
        on_unknown_key(line, key_len, user_data);
      return 1; /* unknown key is fatal */
    }
  }

  if (!magic_seen)
    return 1; /* empty file / no magic */
  *out = tmp;
  return 0;
}
