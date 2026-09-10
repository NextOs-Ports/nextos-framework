/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * nx_symver.h -- bind libm float references to their oldest exported version.
 *
 * Why this exists
 * ---------------
 * Every universal artifact measured in M17 sits at GLIBC_2.27 for exactly one
 * reason: glibc 2.27 published new, faster implementations of the single
 * precision libm entry points and the linker binds to the newest version it
 * can see.  Nothing in the ports needs the new implementation.
 *
 *   readelf -sW --dyn-syms ports/kotor/kotor-universal
 *     -> exp2f@GLIBC_2.27 expf@GLIBC_2.27 log2f@GLIBC_2.27 logf@GLIBC_2.27
 *        powf@GLIBC_2.27
 *   readelf -sW --dyn-syms ports/horizonchase/horizonchase-universal
 *     -> powf@GLIBC_2.27
 *
 * The older versions are still exported by the same libm the devices ship:
 *
 *   readelf -sW --dyn-syms /opt/prebuilt/arm-linux-gnueabihf/libc/lib/libm.so.6
 *     -> powf@@GLIBC_2.27 and powf@GLIBC_2.4  (same for expf/exp2f/logf/log2f)
 *
 * Including this header before any use of those functions binds the references
 * to the old version and drops the artifact floor from GLIBC_2.27 to GLIBC_2.4
 * on ARMHF and to GLIBC_2.17 on AArch64, with identical numerical behaviour on
 * every device that already runs the current builds.
 *
 * Usage
 * -----
 *   #include "nx_symver.h"     // pulls in <math.h> itself
 *   ... and build with the matching -fno-builtin-<fn> flags, otherwise GCC
 *   expands its own builtin and emits a call to the unversioned name before
 *   the macro below can redirect it.  NX_SYMVER_CFLAGS below lists them.
 *
 * The plain ".symver powf,powf@GLIBC_2.4" form does NOT work here: with a
 * merely referenced (undefined) symbol current binutils keeps the default
 * @@GLIBC_2.27 binding.  Redirecting through a private alias does work, which
 * is why the header takes the longer route.
 *
 * Verify with:
 *   python3 -B framework/nxabi/nxabi.py audit <binary>
 * The "glibc-preferred" warning names whichever symbol still raises the floor.
 */

#ifndef NX_SYMVER_H
#define NX_SYMVER_H

/* <math.h> must come first: __GLIBC__ only exists after a libc header has been
 * included, so testing it before this line silently disables the whole file. */
#include <math.h>

#if defined(__linux__) && defined(__GLIBC__) && !defined(NX_SYMVER_DISABLE)

#if defined(__aarch64__)
/* AArch64 glibc starts at 2.17; that is the oldest version that can exist. */
#define NX_SYMVER_LIBM_OLD "GLIBC_2.17"
#elif defined(__arm__)
#define NX_SYMVER_LIBM_OLD "GLIBC_2.4"
#endif

#ifdef NX_SYMVER_LIBM_OLD

/* Bind a private alias to the old exported version, then route every source
 * level call through the alias. */
#define NX_SYMVER_BIND1(symbol)                                            \
    __asm__(".symver nx_symver_" #symbol "," #symbol "@" NX_SYMVER_LIBM_OLD); \
    float nx_symver_##symbol(float);                                       \
    static inline float nx_##symbol(float a) { return nx_symver_##symbol(a); }

#define NX_SYMVER_BIND2(symbol)                                            \
    __asm__(".symver nx_symver_" #symbol "," #symbol "@" NX_SYMVER_LIBM_OLD); \
    float nx_symver_##symbol(float, float);                                \
    static inline float nx_##symbol(float a, float b)                      \
    { return nx_symver_##symbol(a, b); }

/* The five single precision entry points that glibc 2.27 re-published. */
NX_SYMVER_BIND2(powf)
NX_SYMVER_BIND1(expf)
NX_SYMVER_BIND1(exp2f)
NX_SYMVER_BIND1(logf)
NX_SYMVER_BIND1(log2f)

#define powf  nx_powf
#define expf  nx_expf
#define exp2f nx_exp2f
#define logf  nx_logf
#define log2f nx_log2f

/* Build flags the caller must add so GCC does not expand its own builtin and
 * bypass the macros above:
 *   NX_SYMVER_CFLAGS = -fno-builtin-powf -fno-builtin-expf -fno-builtin-exp2f
 *                      -fno-builtin-logf -fno-builtin-log2f
 */

#endif /* NX_SYMVER_LIBM_OLD */

#endif /* __linux__ && __GLIBC__ && !NX_SYMVER_DISABLE */

#endif /* NX_SYMVER_H */
