#ifndef TS_CPUINFO_COMPAT_H
#define TS_CPUINFO_COMPAT_H

#include <stddef.h>

/* FMOD Ex 4.44's ARMv7 CPU probe predates the AArch64 /proc/cpuinfo feature
 * names.  Translate proven equivalent feature names in-place while retaining
 * the native file shape.  Returns 1 when aliases were added, 0 when the input
 * did not need translation, and -1 for invalid arguments. */
int ts_cpuinfo_add_armv7_aliases(char *buffer, size_t *length,
                                 size_t capacity);

#endif
