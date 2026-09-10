/*
 * crash.h -- handler de SIGSEGV/SIGBUS/... com backtrace relativo a libff4a.
 */
#ifndef FF4A_CRASH_H
#define FF4A_CRASH_H

/* instala os handlers; base/size = módulo carregado (p/ offsets relativos). */
void crash_init(void);

#endif
