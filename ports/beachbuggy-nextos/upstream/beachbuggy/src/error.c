#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "error.h"

void fatal_error(const char *fmt, ...) {
  va_list list;
  va_start(list, fmt);
  fprintf(stderr, "\nFATAL ERROR: ");
  vfprintf(stderr, fmt, list);
  fprintf(stderr, "\n");
  va_end(list);
  fflush(stderr);
  abort();
}
