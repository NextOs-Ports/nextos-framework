/*
 * error.h -- error handler
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Part of the NextOS so-loader lineage (ARM64)
 */

#ifndef __ERROR_H__
#define __ERROR_H__

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

#endif
