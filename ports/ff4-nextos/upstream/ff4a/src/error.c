/*
 * error.c -- fatal_error. FF4 so-loader.
 */
#include "error.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal_error(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debugPrintf("FATAL: %s\n", buf);
  log_close();
  abort();
}
