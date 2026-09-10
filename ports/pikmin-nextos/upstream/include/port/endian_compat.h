#ifndef _PIKI_PORT_ENDIAN_COMPAT_H
#define _PIKI_PORT_ENDIAN_COMPAT_H

// The engine reads four-byte things out of memory as one word in a handful of
// places - pane tags, GXColor structs, particle definition records.  On the
// GameCube that word came out most-significant-byte-first, and the constants it
// is compared against ('pall', 'yoko', ...) are written that way.  On a
// little-endian host the same cast produces the bytes reversed.
//
// These helpers spell the byte order out so the meaning survives the move.  On
// the console they are the identity.

#include "types.h"

#ifdef TARGET_PC

#define PIKI_TAG_FROM_BYTES(p)                                                                     \
	(((u32)((immut u8*)(p))[0] << 24) | ((u32)((immut u8*)(p))[1] << 16) | ((u32)((immut u8*)(p))[2] << 8) \
	 | (u32)((immut u8*)(p))[3])

#define PIKI_TAG_BYTE(tag, index) ((char)(((u32)(tag) >> (8 * (3 - (index)))) & 0xFF))

#define PIKI_STORE_TAG(p, tag)                                                                     \
	do {                                                                                           \
		u8* _pikiTagDst = (u8*)(p);                                                                \
		u32 _pikiTagVal = (u32)(tag);                                                              \
		_pikiTagDst[0]  = (u8)(_pikiTagVal >> 24);                                                 \
		_pikiTagDst[1]  = (u8)(_pikiTagVal >> 16);                                                 \
		_pikiTagDst[2]  = (u8)(_pikiTagVal >> 8);                                                  \
		_pikiTagDst[3]  = (u8)(_pikiTagVal);                                                       \
	} while (0)

// GXColor is {r, g, b, a}; read as one word on the console that is 0xRRGGBBAA,
// which is what GXColor1u32 expects.
#define PIKI_COLOR_TO_U32(col) (((u32)(col).r << 24) | ((u32)(col).g << 16) | ((u32)(col).b << 8) | (u32)(col).a)

#define PIKI_BE_U32(p) (((u32)((immut u8*)(p))[0] << 24) | ((u32)((immut u8*)(p))[1] << 16) | ((u32)((immut u8*)(p))[2] << 8) | (u32)((immut u8*)(p))[3])
#define PIKI_BE_S16(p) ((s16)(((u32)((immut u8*)(p))[0] << 8) | (u32)((immut u8*)(p))[1]))
#define PIKI_BE_U16(p) ((u16)(((u32)((immut u8*)(p))[0] << 8) | (u32)((immut u8*)(p))[1]))

#else

#define PIKI_TAG_FROM_BYTES(p) (*(u32*)(p))
#define PIKI_TAG_BYTE(tag, index) (((immut char*)&(tag))[index])
#define PIKI_STORE_TAG(p, tag) (*(u32*)(p) = (u32)(tag))
#define PIKI_COLOR_TO_U32(col) (*(u32*)&(col))
#define PIKI_BE_U32(p) (*(u32*)(p))
#define PIKI_BE_S16(p) (*(s16*)(p))
#define PIKI_BE_U16(p) (*(u16*)(p))

#endif

#endif
