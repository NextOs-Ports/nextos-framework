#ifndef HUNTDOWN_MEDIA_NDK_FAIL_H
#define HUNTDOWN_MEDIA_NDK_FAIL_H

#include <stddef.h>

/*
 * Huntdown's Unity player imports Android MediaNDK directly.  NextOS does not
 * provide libmediandk, so every imported symbol must still have the correct
 * broad return semantics.  In particular, returning AMEDIA_OK without having
 * created a codec leaves Unity holding null/invalid native objects.
 *
 * This table deliberately implements a clean "backend unavailable" result.
 * It does not skip either video or alter managed game state: Unity receives
 * the same kind of creation/prepare failure that an Android decoder can
 * report and remains responsible for its normal error path.
 */
typedef struct HdMediaNdkSymbol {
  const char *name;
  void *address;
} HdMediaNdkSymbol;

const HdMediaNdkSymbol *hd_media_ndk_fail_symbols(size_t *count);

#endif
