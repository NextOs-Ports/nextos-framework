#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdint.h>
#include <stddef.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;

const char *resolve_android_path(const char *path);

#endif
