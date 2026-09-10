/*
 * imports.h -- tabela de shims p/ imports que o glibc não cobre (bionicismos,
 * AAsset, OpenSL). O resto resolve via dlsym(RTLD_DEFAULT) no so_resolve.
 */
#ifndef FF4A_IMPORTS_H
#define FF4A_IMPORTS_H

#include "so_util.h"

extern DynLibFunction ff4a_imports[];
extern int ff4a_imports_count;

#endif
