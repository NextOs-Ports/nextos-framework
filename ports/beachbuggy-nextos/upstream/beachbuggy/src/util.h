#ifndef __UTIL_H__
#define __UTIL_H__

#include <stddef.h>
#include <stdint.h>

int debugPrintf(const char *text, ...);
int logPrintf(const char *text, ...);

const char *bb_game_dir(void);
int bb_game_path(char *dst, size_t dst_size, const char *relative);

int ret0(void);
int ret1(void);
int retm1(void);

#endif
