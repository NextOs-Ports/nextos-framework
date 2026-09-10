/*
 * error.c -- error handler
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Part of the NextOS so-loader lineage (ARM64)
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"

void fatal_error(const char *fmt, ...) {
  va_list list;
  va_start(list, fmt);
  fprintf(stderr, "FATAL ERROR: ");
  vfprintf(stderr, fmt, list);
  va_end(list);
  fprintf(stderr, "\n");
  exit(1);
}
