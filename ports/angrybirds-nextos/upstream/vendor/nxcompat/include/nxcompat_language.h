/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_language.h -- fonte unica da regra de idioma dos ports (onda v2).
 *
 * Por que existe: cada port reescrevia a ponte "ler NXPORT_LANGUAGE /
 * GAME_LANGUAGE, normalizar, validar contra a lista suportada, cair no
 * default, NUNCA japones" -- skate3 tinha sk3_language_tag(), os FF4 tem
 * ff_language.h, cada ETD o seu. A regra e' UMA e agora mora aqui, testada
 * uma vez. O que continua no adapter, de proposito: o mapa codigo->indice/
 * asset da engine (FF usa indice+.lproj, Unity usa Locale, ...).
 *
 * Contrato:
 *   - entrada: lista `supported` de codigos BCP-47 minusculos com '-'
 *     (ex.: "en", "pt-br", "zh-cn") e um `fallback` que pertence a lista;
 *   - le NXPORT_LANGUAGE (o que o launcher valida) e GAME_LANGUAGE de
 *     reforco; normaliza (minusculas, '_' -> '-');
 *   - casa exato primeiro; depois por subtag primaria nas duas direcoes
 *     ("pt-br" pedido acha "pt" suportado e vice-versa);
 *   - REGRA #5 DA CASA: japones ("ja" e "ja-*") NUNCA e' selecionavel --
 *     nem por pedido, nem por fallback. Pedido "ja" cai no fallback;
 *     fallback "ja" e' erro de programacao e cai em "en".
 *   - devolve um ponteiro de DENTRO de `supported` (ou "en"); nunca NULL.
 *
 * Header-only e sem dependencia da biblioteca nxcompat, como o
 * nxcompat_system_font.h: um port usa sem linkar nada a mais.
 */
#ifndef NXCOMPAT_LANGUAGE_H
#define NXCOMPAT_LANGUAGE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void nxcompat_language_normalize_(const char *in, char *out,
                                                size_t cap) {
  size_t i = 0;
  out[0] = '\0';
  if (in == NULL)
    return;
  for (; in[i] != '\0' && i + 1 < cap; ++i) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c + 32);
    if (c == '_')
      c = '-';
    out[i] = c;
  }
  out[i] = '\0';
}

static inline int nxcompat_language_is_japanese_(const char *code) {
  return code != NULL && code[0] == 'j' && code[1] == 'a' &&
         (code[2] == '\0' || code[2] == '-');
}

static inline int nxcompat_language_primary_matches_(const char *left,
                                                     const char *right) {
  size_t i = 0;
  for (; left[i] != '\0' && left[i] != '-'; ++i) {
    if (right[i] != left[i])
      return 0;
  }
  return right[i] == '\0' || right[i] == '-';
}

/* Seleciona o idioma efetivo. `receipt` (opcional) ganha uma linha curta
 * dizendo de onde a escolha veio -- para o log do port. */
static inline const char *nxcompat_language_select(
    const char *const *supported, size_t supported_count,
    const char *fallback_code, char *receipt, size_t receipt_cap) {
  char wanted[24];
  const char *chosen = NULL;
  const char *source = "fallback";
  const char *request = getenv("NXPORT_LANGUAGE");
  size_t i;

  if (request == NULL || request[0] == '\0' ||
      strcmp(request, "auto") == 0)
    request = getenv("GAME_LANGUAGE");
  nxcompat_language_normalize_(request, wanted, sizeof wanted);

  if (wanted[0] != '\0' && strcmp(wanted, "auto") != 0 &&
      !nxcompat_language_is_japanese_(wanted) && supported != NULL) {
    for (i = 0; i < supported_count && chosen == NULL; ++i)
      if (supported[i] != NULL && strcmp(supported[i], wanted) == 0) {
        chosen = supported[i];
        source = "exact";
      }
    for (i = 0; i < supported_count && chosen == NULL; ++i)
      if (supported[i] != NULL &&
          !nxcompat_language_is_japanese_(supported[i]) &&
          nxcompat_language_primary_matches_(wanted, supported[i])) {
        chosen = supported[i];
        source = "primary-subtag";
      }
  }
  if (chosen == NULL) {
    chosen = fallback_code;
    if (nxcompat_language_is_japanese_(chosen) || chosen == NULL ||
        chosen[0] == '\0') {
      chosen = "en"; /* fallback "ja" e' erro de programacao; regra #5 */
      source = "fallback-invalido-en";
    }
  }
  if (receipt != NULL && receipt_cap > 0) {
    size_t used = 0;
    const char *parts[5] = {"language: ", chosen, " (", source, ")"};
    receipt[0] = '\0';
    for (i = 0; i < 5; ++i) {
      size_t len = strlen(parts[i]);
      if (used + len + 1 > receipt_cap)
        break;
      memcpy(receipt + used, parts[i], len);
      used += len;
      receipt[used] = '\0';
    }
  }
  return chosen;
}

#ifdef __cplusplus
}
#endif

#endif /* NXCOMPAT_LANGUAGE_H */
