/*
 * util.h -- logging (stderr + log O_SYNC persistente na pasta do port) + helpers.
 * FF4 The After Years so-loader (Mali-450). Escrito do zero p/ o FF4.
 */
#ifndef FF4A_UTIL_H
#define FF4A_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* Abre o log persistente O_SYNC (sobrevive a wedge/power-cycle). path relativo
 * à pasta do port; se NULL usa "ff4a.log". Idempotente. */
void log_open(const char *path);
void log_close(void);

/* printf que vai p/ stderr E p/ o log O_SYNC (flush imediato). */
void debugPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* tempo monotônico em ms (frame pacing). */
uint64_t now_ms(void);

#endif
