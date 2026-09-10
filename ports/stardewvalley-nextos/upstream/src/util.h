/*
 * util.h -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <stddef.h>

int debugPrintf(const char *text, ...);

int ret0(void);
int ret1(void);
int retm1(void);

/* Dir do proprio binario (= GAMEDIR do port em qualquer CFW). Ver util.c. */
const char *sdv_base_dir(void);
const char *sdv_path_in_base(char *buf, size_t bufsz, const char *sub);

#endif
