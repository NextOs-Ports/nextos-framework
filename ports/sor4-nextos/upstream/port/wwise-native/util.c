/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

#define LOG_NAME "debug.log"

/* SOR4/Mali-450: o log nativo (jni_shim/so_util/...) era escrito a CADA chamada
 * (fopen+vprintf), gerando ~centenas de linhas/sessao + custo de I/O sincrono.
 * Agora GATEADO por env: silencioso por padrao, liga com WWISE_LOG=1 quando
 * precisar diagnosticar (SOR4_NATLOG=1; mesmo padrao do SOR4_MGLOG no .NET). */
static int dbg_on(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("SOR4_NATLOG");
    v = (e && e[0] == '1') ? 1 : 0;
  }
  return v;
}

int debugPrintf(const char *text, ...) {
  va_list list;

  if (!dbg_on()) return 0;

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
