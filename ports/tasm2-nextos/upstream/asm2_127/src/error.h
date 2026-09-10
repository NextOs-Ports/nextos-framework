#ifndef ASM2_ERROR_H
#define ASM2_ERROR_H

void fatal_error(const char *format, ...)
    __attribute__((format(printf, 1, 2), noreturn));

#endif

