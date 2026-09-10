#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal_error(const char *format, ...) {
  va_list args;
  fputs("FATAL: ", stderr);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fflush(stderr);
  exit(1);
}

