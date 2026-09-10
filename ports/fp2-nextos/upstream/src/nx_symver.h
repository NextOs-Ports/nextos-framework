/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * nx_symver.h -- bind libm references to their oldest exported version.
 *
 * Adapted from ports/gangstarrio/src/nx_symver.h.  Same reason, different
 * symbols: the NextOS toolchain sysroot in use here is glibc 2.44 while the
 * device image still ships 2.43, and glibc 2.44 re-published the double
 * precision `cosh` and `sinh`.  The linker binds to the newest version it can
 * see, so an otherwise fine binary dies on the device with:
 *
 *   ./fp22-nextos: /usr/lib/libm.so.6: version `GLIBC_2.44' not found
 *
 * The old versions are still exported by the same libm:
 *
 *   nm -D --with-symbol-versions <sysroot>/usr/lib/libm.so.6
 *     -> cosh@@GLIBC_2.44  and  cosh@GLIBC_2.17
 *
 * Nothing here needs the new implementation, so bind to the floor.  AArch64
 * glibc starts at 2.17, which is the oldest version that can exist.
 *
 * Note the export table in bionic.c stringifies its argument
 * (`#define E(n) { #n, (void *)(uintptr_t)n }`), so the `#define cosh nx_cosh`
 * shortcut used by the gangstarrio header would rename the exported symbol.
 * Callers must therefore reference the aliases explicitly and spell the
 * exported name by hand.
 */

#ifndef NX_SYMVER_H
#define NX_SYMVER_H

/* <math.h> must come first: __GLIBC__ only exists after a libc header. */
#include <math.h>

#if defined(__linux__) && defined(__GLIBC__) && defined(__aarch64__) && \
    !defined(NX_SYMVER_DISABLE)

#define NX_SYMVER_LIBM_OLD "GLIBC_2.17"

/* A merely referenced (undefined) symbol keeps its default @@ binding under
 * current binutils, so the reference has to travel through a private alias. */
#define NX_SYMVER_BIND_D1(symbol)                                             \
    __asm__(".symver nx_symver_" #symbol "," #symbol "@" NX_SYMVER_LIBM_OLD); \
    double nx_symver_##symbol(double);

NX_SYMVER_BIND_D1(cosh)
NX_SYMVER_BIND_D1(sinh)

#define NX_SYMVER_COSH nx_symver_cosh
#define NX_SYMVER_SINH nx_symver_sinh

#else

#define NX_SYMVER_COSH cosh
#define NX_SYMVER_SINH sinh

#endif

#endif /* NX_SYMVER_H */
