/*
 * util.h -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Part of the NextOS so-loader lineage (ARM64)
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(const char *text, ...); /* verboso, so' com COI_DEBUG=1 */
int logPrintf(const char *text, ...);   /* release: sempre no stderr do port */

int ret0(void);
int ret1(void);
int retm1(void);

#endif
