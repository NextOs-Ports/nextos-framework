// PowerPC intrinsics the game's headers declare as MWCC builtins.
//
// MWCC expanded these to single instructions; on AArch64 they are ordinary
// functions.  The semantics are the PowerPC ones, not the fastest possible
// ones, because the game's math (sqrtf, normalisation, the Newton-Raphson
// refinement in stl/math.h) is written around their exact behaviour.

#include <math.h>
#include <string.h>

#include "types.h"

const f32 __float_huge[] = { 1e30f * 1e30f };
const f32 __float_nan[] = { 0.0f / 0.0f };
const f64 __double_huge[] = { 1e300 * 1e300 };
const f64 __double_nan[] = { 0.0 / 0.0 };

f64 __fabs(f64 x) { return __builtin_fabs(x); }
f32 __fabsf(f32 x) { return __builtin_fabsf(x); }
f64 __fnabs(f64 x) { return -__builtin_fabs(x); }
f32 __fnabsf(f32 x) { return -__builtin_fabsf(x); }

f64 __fmadd(f64 a, f64 b, f64 c) { return a * b + c; }
f64 __fmsub(f64 a, f64 b, f64 c) { return a * b - c; }
f64 __fnmadd(f64 a, f64 b, f64 c) { return -(a * b + c); }
f64 __fnmsub(f64 a, f64 b, f64 c) { return -(a * b - c); }
f32 __fmadds(f32 a, f32 b, f32 c) { return a * b + c; }
f32 __fmsubs(f32 a, f32 b, f32 c) { return a * b - c; }
f32 __fnmadds(f32 a, f32 b, f32 c) { return -(a * b + c); }
f32 __fnmsubs(f32 a, f32 b, f32 c) { return -(a * b - c); }

// fsel: "select on greater-or-equal-to-zero", NaN counts as negative.
f64 __fsel(f64 a, f64 b, f64 c) { return (a >= 0.0) ? b : c; }
f32 __fsels(f32 a, f32 b, f32 c) { return (a >= 0.0f) ? b : c; }

// frsqrte / fres are the 750's low-precision estimates.  Callers always refine
// them with Newton-Raphson, so returning the exact value is both correct and
// closer to the intended result than an estimate would be.
f64 __frsqrte(f64 x) { return 1.0 / sqrt(x); }
f32 __fres(f32 x) { return 1.0f / x; }

f64 __fsqrt(f64 x) { return sqrt(x); }
f32 __fsqrts(f32 x) { return sqrtf(x); }

s64 __fctid(f64 x) { return (s64)x; }
s64 __fctiw(f64 x) { return (s32)x; }
f64 __fcfid(s64 x) { return (f64)x; }

// FPSCR manipulation has no meaning off PowerPC; the game only uses it to
// toggle exception enables around a couple of divides.
f64 __mffs(void) { return 0.0; }
void __mtfsf(int mask, f64 value) { (void)mask; (void)value; }
void __mtfsfi(int field, int value) { (void)field; (void)value; }
void __mtfsb0(int bit) { (void)bit; }
void __mtfsb1(int bit) { (void)bit; }
f64 __setflm(f64 value) { (void)value; return 0.0; }

int __abs(int x) { return x < 0 ? -x : x; }
long __labs(long x) { return x < 0 ? -x : x; }
int __cntlzw(unsigned int x) { return x == 0 ? 32 : __builtin_clz(x); }

// Byte-reversed load/store.
int __lhbrx(void* base, int index)
{
	u16 v;
	memcpy(&v, (u8*)base + index, sizeof(v));
	return (u16)__builtin_bswap16(v);
}

int __lwbrx(void* base, int index)
{
	u32 v;
	memcpy(&v, (u8*)base + index, sizeof(v));
	return (int)__builtin_bswap32(v);
}

void __sthbrx(unsigned short value, void* base, int index)
{
	u16 v = __builtin_bswap16(value);
	memcpy((u8*)base + index, &v, sizeof(v));
}

void __stwbrx(unsigned int value, void* base, int index)
{
	u32 v = __builtin_bswap32(value);
	memcpy((u8*)base + index, &v, sizeof(v));
}

// Rotate-left-word-immediate-then-AND-with-mask and friends.
static u32 ppc_mask(int mb, int me)
{
	u32 begin = 0xFFFFFFFFu >> mb;
	u32 end = (me >= 31) ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
	u32 mask = begin & end;
	return (mb <= me) ? mask : ~mask;
}

static u32 ppc_rotl(u32 value, int sh) { return (value << (sh & 31)) | (value >> ((32 - (sh & 31)) & 31)); }

int __rlwinm(int s, int sh, int mb, int me) { return (int)(ppc_rotl((u32)s, sh) & ppc_mask(mb, me)); }
int __rlwnm(int s, int sh, int mb, int me) { return (int)(ppc_rotl((u32)s, sh) & ppc_mask(mb, me)); }
int __rlwimi(int a, int s, int sh, int mb, int me)
{
	u32 mask = ppc_mask(mb, me);
	return (int)((ppc_rotl((u32)s, sh) & mask) | ((u32)a & ~mask));
}

void __sync(void) { __sync_synchronize(); }
void __isync(void) { __sync_synchronize(); }
void __eieio(void) { __sync_synchronize(); }
