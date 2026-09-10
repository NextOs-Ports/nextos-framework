/*
 * error.h -- error handler
 */

#ifndef __ERROR_H__
#define __ERROR_H__

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

#endif
