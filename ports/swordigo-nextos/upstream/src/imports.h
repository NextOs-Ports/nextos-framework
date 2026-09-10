#ifndef SWORDIGO_IMPORTS_H
#define SWORDIGO_IMPORTS_H

#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;

void init_openal(void);
void deinit_openal(void);

#endif
