#include "util.h"

#include <stdarg.h>
#include <stdio.h>

void debugPrintf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fflush(stderr);
}

