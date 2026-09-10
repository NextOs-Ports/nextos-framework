/*
 * util.h -- misc utility functions
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(const char *text, ...);
uintptr_t read_tls_stack_guard(void);
const char *resolve_android_path(const char *path);

/* The bgfx `sh_*` files are disposable provider-specific shader binaries.
 * Configure a measured graphics identity before nativeInit so caches from a
 * different renderer/provider can never be consumed as current state. */
int configure_shader_cache_identity(const char *identity);

int ret0(void);
int ret1(void);
int retm1(void);

#endif
