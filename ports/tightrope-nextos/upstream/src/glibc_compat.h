/*
 * glibc_compat.h -- the handful of names the public build's older glibc does
 * not have.
 *
 * The public binary is built against glibc 2.28 (Debian buster) so that every
 * ELF we ship stays at or below the GLIBC_2.30 ceiling the multi-device policy
 * sets.  The NextOS Elite build uses the current sysroot (2.43) and has all of
 * these already, so each one is compiled in only when the toolchain's own
 * headers do not declare it.
 *
 * These are re-implementations of the *host* libc call, and they exist because
 * the game's Bionic import table names them -- not because we want a different
 * behaviour.  Lowering a symbol version by editing strings is never an option;
 * this is the supported way: build on the older libc and fill the gaps.
 */

#ifndef TR_GLIBC_COMPAT_H
#define TR_GLIBC_COMPAT_H

#include <features.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __GLIBC_PREREQ
#define __GLIBC_PREREQ(maj, min) 0
#endif

/* glibc grew strlcpy/strlcat only in 2.38; Bionic has always had them. */
#if !__GLIBC_PREREQ(2, 38)
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

static inline size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t used = strnlen(dst, size);
    if (used == size)
        return size + strlen(src);
    return used + strlcpy(dst + used, src, size - used);
}
#endif

/* gettid() as a libc function arrived in glibc 2.30. */
#if !__GLIBC_PREREQ(2, 30)
static inline pid_t gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}
#endif

#endif /* TR_GLIBC_COMPAT_H */
