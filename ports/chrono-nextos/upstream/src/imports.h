#ifndef IMPORTS_H
#define IMPORTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *symbol;
  uintptr_t func;
} DynLibFunction;

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;

#endif // IMPORTS_H
