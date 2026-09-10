/* util.c -- logging. Everything goes to stderr; the launcher redirects it to
 * the port log (opened O_SYNC there), so a wedge still leaves the tail on disk. */
#include <stdarg.h>
#include <stdio.h>

#include "util.h"

int debugPrintf(const char *text, ...) {
  va_list list;
  va_start(list, text);
  vfprintf(stderr, text, list);
  va_end(list);
  return 0;
}
