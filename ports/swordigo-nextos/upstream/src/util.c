/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#include <stdarg.h>
#include <stdio.h>

#include "util.h"

int debugPrintf(const char *text, ...) {
  va_list list;

  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  fflush(stdout);

  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
