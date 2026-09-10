/*
 * util.h -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stddef.h>
#include <stdint.h>

int debugPrintf(const char *text, ...);

/* Runtime paths are rooted at the directory selected by the launcher.  Keeping
 * this in one place avoids baking a particular firmware's ROM mount into the
 * loader. */
const char *ter_game_dir(void);
int ter_game_path(char *out, size_t out_size, const char *relative);

int ret0(void);
int ret1(void);
int retm1(void);

#endif
