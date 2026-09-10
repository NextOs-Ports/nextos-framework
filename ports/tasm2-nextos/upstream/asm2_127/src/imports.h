#ifndef ASM2_IMPORTS_H
#define ASM2_IMPORTS_H

#include <stddef.h>

#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;
#if defined(__i386__)
void asm2_imports_initialize(void);
#endif

#endif
