/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Part of the NextOS so-loader lineage (ARM64)
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define LOG_NAME "debug.log"

/* DOIS NIVEIS, de proposito.
 *
 * `debugPrintf` e' o canal VERBOSO da bancada: sao centenas de chamadas por
 * frame, e a versao antiga fazia fopen+fclose no "debug.log" A CADA CHAMADA
 * (I/O no eMMC/SD dentro do frame) — isso travava a thread de audio e ajudava
 * a derrubar o jogo. Fica desligado por padrao (`TS_DEBUG=1` liga).
 *
 * `logPrintf` e' o canal do RELEASE: as poucas linhas que decidem um relato de
 * campo — qual EGLConfig veio, se o audio caiu para ALSA, por que o jogo
 * saiu, se o save foi escrito. Sempre ligado, direto no stderr, que o run.sh
 * ja aponta para o log do port. "Nenhum log" nao pode ser um estado possivel,
 * e um relato de "nao salva" sem uma linha de diagnostico custa uma release.
 * O volume e' de dezenas de linhas por sessao inteira: custo irrelevante. */
static int coi_debug_on(void) {
  static int dbg = -1;
  if (dbg < 0)
    dbg = ts_env_enabled("TS_DEBUG");
  return dbg;
}

int ts_env_enabled(const char *name) {
  const char *value = name ? getenv(name) : NULL;

  if (!value || !value[0])
    return 0;
  return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
         strcmp(value, "FALSE") != 0 && strcmp(value, "off") != 0 &&
         strcmp(value, "OFF") != 0 && strcmp(value, "no") != 0 &&
         strcmp(value, "NO") != 0;
}

int logPrintf(const char *text, ...) {
  va_list list;
  va_start(list, text);
  vfprintf(stderr, text, list);
  va_end(list);
  return 0;
}

int debugPrintf(const char *text, ...) {
  if (!coi_debug_on()) return 0;

  va_list list;
  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
