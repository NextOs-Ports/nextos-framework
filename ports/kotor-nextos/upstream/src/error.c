/* error.c -- fatal error handler (Linux port) */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "error.h"

void fatal_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "\n=== FATAL ===\n");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n=============\n");
  va_end(ap);
  fflush(stderr);
  exit(1);
}
