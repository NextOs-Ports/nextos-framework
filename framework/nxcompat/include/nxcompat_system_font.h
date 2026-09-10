/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_system_font.h -- fonte unica da busca por uma TTF/OTF do sistema.
 *
 * Por que existe: um port que substitui as fontes proprias do jogo por
 * stb_truetype passa a depender de uma fonte do SISTEMA em tempo de execucao.
 * E cada CFW guarda TTF num lugar diferente.
 *
 * O modo como isso falha e' o problema. Quando a lista de caminhos do port nao
 * casa com nenhum arquivo, `stbtt_InitFont` nunca roda, `drawFont` desenha zero
 * glifo e o jogo abre INTEIRO e MUDO: dialogos, menus e descricoes vazios, com
 * o resto -- imagem, audio, input, save -- funcionando. Nao ha erro no log, o
 * processo sai 0, e o launcher nao tem sinal nenhum de que algo deu errado.
 * Medido em campo: o ff4a tinha tres caminhos fixos e nenhum existia no dArkOS.
 *
 * A busca aqui vai do mais explicito ao mais generico e termina numa varredura
 * de diretorio, para que um layout de fontes que ninguem previu ainda funcione:
 *
 *   1. `preferred` -- a escolha explicita de quem chama;
 *   2. a variavel de ambiente `env_name`, se houver;
 *   3. a fonte que o PROPRIO PortMaster carrega consigo, por
 *      NXCOMPAT_PORTMASTER_DIR ou CONTROLFOLDER. Esse degrau e' o que resolve
 *      na pratica: existe em todo aparelho que tem PortMaster, seja qual for a
 *      distro por tras;
 *   4. `extra_roots` de quem chama, varridas um nivel de subpasta;
 *   5. os caminhos absolutos conhecidos das distros usadas pelas CFW, e por
 *      fim as raizes padrao de fontes -- um nivel de subpasta, que e' como
 *      /usr/share/fonts se organiza.
 *
 * Um candidato so' vale se abrir E tiver assinatura de fonte no inicio do
 * arquivo. Sem essa checagem um `.ttf` de zero byte, um LFS pointer ou um HTML
 * de erro baixado com nome de fonte seriam aceitos, e o jogo voltaria a abrir
 * mudo -- exatamente a falha que este arquivo existe para impedir.
 *
 * Este modulo nao conhece nome de aparelho nem de firmware, nao le' capability
 * nenhuma e nao depende da biblioteca nxcompat: e' header-only de proposito,
 * para um port poder usa'-lo sem linkar nada a mais.
 *
 * USO:
 *
 *   #include "nxcompat_system_font.h"
 *   char path[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
 *   if (nxcompat_system_font_find(NULL, "FF4_FONT", NULL, 0,
 *                                 path, sizeof path)) {
 *       ... abre `path` e passa para stbtt_InitFont ...
 *   } else {
 *       ... falhar aqui e' melhor do que abrir o jogo mudo ...
 *   }
 */
#ifndef NXCOMPAT_SYSTEM_FONT_H
#define NXCOMPAT_SYSTEM_FONT_H

#define NXCOMPAT_SYSTEM_FONT_CONTRACT 1
#define NXCOMPAT_SYSTEM_FONT_PATH_MAX 1024u

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED __attribute__((unused))
#else
#define NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
#endif

/* Assinaturas aceitas no inicio do arquivo:
 *   00 01 00 00  TrueType
 *   "true"       TrueType (Apple)
 *   "ttcf"       TrueType Collection
 *   "OTTO"       OpenType com outlines CFF                                  */
NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_has_signature(const char *path) {
  static const char k_truetype[4] = {0x00, 0x01, 0x00, 0x00};
  unsigned char head[4];
  size_t got;
  FILE *stream;

  stream = fopen(path, "rb");
  if (stream == NULL) {
    return 0;
  }
  got = fread(head, 1u, sizeof head, stream);
  fclose(stream);
  if (got != sizeof head) {
    return 0;
  }
  if (memcmp(head, k_truetype, sizeof head) == 0 ||
      memcmp(head, "true", sizeof head) == 0 ||
      memcmp(head, "ttcf", sizeof head) == 0 ||
      memcmp(head, "OTTO", sizeof head) == 0) {
    return 1;
  }
  return 0;
}

NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_accept(const char *path, char *out,
                                       size_t out_size) {
  size_t length;

  if (path == NULL || path[0] == '\0' || out == NULL || out_size == 0u) {
    return 0;
  }
  length = strlen(path);
  if (length + 1u > out_size) {
    return 0;
  }
  if (!nxcompat_system_font_has_signature(path)) {
    return 0;
  }
  memcpy(out, path, length + 1u);
  return 1;
}

/* Comparacao propria em vez de strcasecmp: o helper e' header-only e entra em
 * ports compilados com _POSIX_C_SOURCE fechado, onde <strings.h> nem sempre
 * esta' visivel. Quatro caracteres ASCII nao justificam a dependencia. */
NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_suffix_is(const char *tail,
                                          const char *lowercase) {
  size_t i;

  for (i = 0u; i < 4u; ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != lowercase[i]) {
      return 0;
    }
  }
  return 1;
}

NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_is_face_name(const char *name) {
  size_t length = strlen(name);
  const char *tail;

  if (length < 5u) {
    return 0;
  }
  tail = name + (length - 4u);
  return nxcompat_system_font_suffix_is(tail, ".ttf") ||
         nxcompat_system_font_suffix_is(tail, ".otf") ||
         nxcompat_system_font_suffix_is(tail, ".ttc");
}

NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_join(char *buffer, size_t buffer_size,
                                     const char *directory, const char *name) {
  int written = snprintf(buffer, buffer_size, "%s/%s", directory, name);
  return written > 0 && (size_t)written < buffer_size;
}

/* Varre `root` e um nivel de subpastas. Um nivel basta: e' assim que
 * /usr/share/fonts se organiza em toda distro usada pelas CFW. Descer mais
 * custaria tempo de boot sem cobrir caso real. */
NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_scan(const char *root, char *out,
                                     size_t out_size) {
  char path[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
  char nested[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
  struct dirent *entry;
  struct dirent *sub_entry;
  DIR *directory;
  DIR *sub;

  if (root == NULL || root[0] == '\0') {
    return 0;
  }
  directory = opendir(root);
  if (directory == NULL) {
    return 0;
  }
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    if (!nxcompat_system_font_join(path, sizeof path, root, entry->d_name)) {
      continue;
    }
    if (nxcompat_system_font_is_face_name(entry->d_name)) {
      if (nxcompat_system_font_accept(path, out, out_size)) {
        closedir(directory);
        return 1;
      }
      continue;
    }
    sub = opendir(path);
    if (sub == NULL) {
      continue;
    }
    while ((sub_entry = readdir(sub)) != NULL) {
      if (sub_entry->d_name[0] == '.' ||
          !nxcompat_system_font_is_face_name(sub_entry->d_name)) {
        continue;
      }
      if (!nxcompat_system_font_join(nested, sizeof nested, path,
                                     sub_entry->d_name)) {
        continue;
      }
      if (nxcompat_system_font_accept(nested, out, out_size)) {
        closedir(sub);
        closedir(directory);
        return 1;
      }
    }
    closedir(sub);
  }
  closedir(directory);
  return 0;
}

/* Escreve em `out` o caminho da primeira fonte utilizavel. Devolve 1 se achou,
 * 0 se nao -- e nesse caso `out` fica com string vazia.
 *
 * `preferred`, `env_name` e `extra_roots` podem ser NULL. `extra_roots` e'
 * varrido ANTES dos caminhos do sistema, entao um port que carrega a propria
 * fonte pode aponta'-la ali sem perder o resto da cadeia como rede. */
NXCOMPAT_SYSTEM_FONT_MAYBE_UNUSED
static int nxcompat_system_font_find(const char *preferred,
                                     const char *env_name,
                                     const char *const *extra_roots,
                                     size_t extra_root_count, char *out,
                                     size_t out_size) {
  static const char *const k_portmaster_env[] = {
      "NXCOMPAT_PORTMASTER_DIR",
      "CONTROLFOLDER",
  };
  static const char *const k_portmaster_relative[] = {
      "pylibs/resources/DejaVuSans.ttf",
      "pylibs/resources/font.ttf",
  };
  static const char *const k_known_paths[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
  };
  static const char *const k_scan_roots[] = {
      "/usr/share/fonts",
      "/usr/share/fonts/truetype",
      "/usr/local/share/fonts",
      "/usr/share/consolefonts",
  };
  char candidate[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
  const char *base;
  size_t i;
  size_t j;

  if (out == NULL || out_size == 0u) {
    return 0;
  }
  out[0] = '\0';

  if (nxcompat_system_font_accept(preferred, out, out_size)) {
    return 1;
  }
  if (env_name != NULL && env_name[0] != '\0' &&
      nxcompat_system_font_accept(getenv(env_name), out, out_size)) {
    return 1;
  }
  for (i = 0u; i < sizeof k_portmaster_env / sizeof k_portmaster_env[0]; ++i) {
    base = getenv(k_portmaster_env[i]);
    if (base == NULL || base[0] == '\0') {
      continue;
    }
    for (j = 0u;
         j < sizeof k_portmaster_relative / sizeof k_portmaster_relative[0];
         ++j) {
      if (!nxcompat_system_font_join(candidate, sizeof candidate, base,
                                     k_portmaster_relative[j])) {
        continue;
      }
      if (nxcompat_system_font_accept(candidate, out, out_size)) {
        return 1;
      }
    }
  }
  /* `extra_roots` vem ANTES dos caminhos do sistema: quem passou uma raiz
   * explicita quer aquela fonte, nao a primeira que a distro tiver. */
  if (extra_roots != NULL) {
    for (i = 0u; i < extra_root_count; ++i) {
      if (nxcompat_system_font_scan(extra_roots[i], out, out_size)) {
        return 1;
      }
    }
  }
  for (i = 0u; i < sizeof k_known_paths / sizeof k_known_paths[0]; ++i) {
    if (nxcompat_system_font_accept(k_known_paths[i], out, out_size)) {
      return 1;
    }
  }
  for (i = 0u; i < sizeof k_scan_roots / sizeof k_scan_roots[0]; ++i) {
    if (nxcompat_system_font_scan(k_scan_roots[i], out, out_size)) {
      return 1;
    }
  }
  out[0] = '\0';
  return 0;
}

#endif /* NXCOMPAT_SYSTEM_FONT_H */
