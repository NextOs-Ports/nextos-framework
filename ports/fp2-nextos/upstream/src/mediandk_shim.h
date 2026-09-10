/*
 * mediandk_shim.h -- see mediandk_shim.c for why an empty container is the
 * right answer for this port's intro video.
 */

#ifndef FP2_MEDIANDK_SHIM_H
#define FP2_MEDIANDK_SHIM_H

#include <stddef.h>

#include "nx_elf.h"

typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaCodec AMediaCodec;

const nx_import *fp2_media_table(size_t *n);
void *fp2_media_sym(const char *name);
/* 1 once the decoder has reported end of stream without a single frame -- the
 * measured proof that this device cannot play the clip. */
int fp2_media_video_gave_up(void);

#endif /* FP2_MEDIANDK_SHIM_H */
