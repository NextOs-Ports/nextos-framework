/* util.c -- misc utilities (Linux/SDL port) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "util.h"

int debugPrintf(const char *text, ...) {
  va_list ap;
  va_start(ap, text);
  int r = vfprintf(stderr, text, ap);
  va_end(ap);
  fflush(stderr);
  return r;
}

void cpu_boost(int on) { (void)on; }

int ret0(void) { return 0; }
int retm1(void) { return -1; }
