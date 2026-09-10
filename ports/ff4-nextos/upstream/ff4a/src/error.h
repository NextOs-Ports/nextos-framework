/*
 * error.h -- fatal_error (loga + aborta). FF4 so-loader.
 */
#ifndef FF4A_ERROR_H
#define FF4A_ERROR_H

void fatal_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
