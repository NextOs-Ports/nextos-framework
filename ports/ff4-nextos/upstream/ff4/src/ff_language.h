/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Seleção de idioma da engine Final Fantasy (Matsuura/SQEX), compartilhada por
 * FF4 3D e FF4 The After Years: as duas usam a MESMA ordem canônica de idioma
 * e o mesmo layout `<prefixo>.lproj/`. Le NXPORT_LANGUAGE (o GAME_LANGUAGE que
 * o launcher gera e o nxbootstrap valida), com GAME_LANGUAGE de reforço e
 * FF*_LANG legado por índice para depuração.
 *
 * Índice canônico da engine (getLanguage): ja=0 en=1 fr=2 de=3 it=4 es=5
 * zh_CN=6 zh_TW=7 ko=8 pt=9 ru=10 th=11.  Japonês (0) JAMAIS é selecionável
 * (regra da casa #5); um pedido inválido/desconhecido/"auto"/"ja" cai em en. */
#ifndef FF_LANGUAGE_H
#define FF_LANGUAGE_H

#include <stdlib.h>
#include <string.h>

typedef struct {
  int index;         /* índice da engine para getLanguage() */
  const char *lproj; /* prefixo do diretório <prefixo>.lproj/ no OBB */
} ff_language;

/* Prefixos lproj REAIS medidos no OBB. pt: FF4a usa "pt", FF4 usa "pt_BR" --
 * cada port passa a sua tabela; o índice é o mesmo. */
static inline ff_language ff_language_select(const char *pt_lproj) {
  static const struct {
    const char *code;
    int index;
    const char *lproj;
  } table[] = {
      {"en", 1, "en"},   {"fr", 2, "fr"},      {"de", 3, "de"},
      {"it", 4, "it"},   {"es", 5, "es"},      {"zh", 6, "zh_CN"},
      {"zh-cn", 6, "zh_CN"}, {"zh-tw", 7, "zh_TW"}, {"ko", 8, "ko"},
      {"pt", 9, NULL},   {"pt-br", 9, NULL},   {"ru", 10, "ru"},
      {"th", 11, "th"},
  };
  const char *want = getenv("NXPORT_LANGUAGE");
  if (!want || !*want)
    want = getenv("GAME_LANGUAGE");
  ff_language result = {1, "en"}; /* default en, nunca ja */
  if (want && *want) {
    char lower[16];
    size_t i = 0;
    for (; want[i] && i < sizeof lower - 1; i++) {
      char c = want[i];
      lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[i] = 0;
    for (size_t t = 0; t < sizeof table / sizeof table[0]; t++) {
      if (!strcmp(lower, table[t].code)) {
        result.index = table[t].index;
        result.lproj = table[t].lproj ? table[t].lproj
                                      : (pt_lproj ? pt_lproj : "pt");
        break;
      }
    }
  }
  /* FF*_LANG legado (índice cru) só para depuração; nunca 0 (ja). */
  const char *legacy = getenv("FF4_LANG");
  if (!legacy) legacy = getenv("FF4A_LANG");
  if (legacy && *legacy) {
    int idx = atoi(legacy);
    if (idx >= 1 && idx <= 11)
      result.index = idx;
  }
  return result;
}

#endif /* FF_LANGUAGE_H */
