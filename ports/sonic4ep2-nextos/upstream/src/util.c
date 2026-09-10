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

/* SILENCIOSO por padrao: cada debugPrintf antes fazia fopen+fclose no "debug.log"
 * (I/O no eMMC/SD por CHAMADA) + vprintf no stdout (-> log.txt via tee). Com
 * centenas de chamadas por frame, isso travava a thread de audio (SFX somem/choppy)
 * e contribuia p/ crashes. Agora so loga com SONIC_DEBUG=1; senao no-op total. */
int debugPrintf(const char *text, ...) {
  static int dbg = -1;
  if (dbg < 0) dbg = getenv("SONIC_DEBUG") ? 1 : 0;
  if (!dbg) return 0;

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
