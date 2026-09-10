/*
 * aaudio_shim.h -- AAudio (Android 8+) to SDL2 audio bridge.
 *
 * Unity 6 asks for AAudio and nothing else: its FMOD output dlopens
 * libaaudio.so directly, never consults Build.VERSION.SDK_INT and never falls
 * back to OpenSL ES.  A port that only answers OpenSL ES therefore boots
 * completely silent, with the engine's own log reporting FMOD error 60
 * (FMOD_ERR_OUTPUT_INIT) and no other clue.
 */

#ifndef FP2_AAUDIO_SHIM_H
#define FP2_AAUDIO_SHIM_H

#include <stddef.h>

#include "nx_elf.h"

const nx_import *fp2_aaudio_table(size_t *n);
void *fp2_aaudio_sym(const char *name);
/* Closes any stream still open, so shutdown never races the SDL callback. */
void fp2_aaudio_stop(void);

#endif /* FP2_AAUDIO_SHIM_H */
