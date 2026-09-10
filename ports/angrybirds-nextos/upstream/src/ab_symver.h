/* Keep the universal loader on fcntl's original ARM glibc ABI. Debian Buster
 * republishes the same interface at GLIBC_2.28; the port uses no new command
 * semantics and therefore binds the compatible GLIBC_2.4 export explicitly.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_SYMVER_H
#define AB_SYMVER_H

#include <fcntl.h>

#if defined(__arm__) && defined(__GLIBC__)
__asm__(".symver ab_fcntl_compat,fcntl@GLIBC_2.4");
int ab_fcntl_compat(int descriptor, int command, ...);
#define fcntl ab_fcntl_compat
#endif

#endif
