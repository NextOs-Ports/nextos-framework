/* Compat p/ build RELEASE no debian:buster (glibc 2.28). Injetado via -include
 * SO no build_buster.sh (o build.sh host nao usa isto). glibc <2.30 nao expoe
 * o wrapper gettid(); provemos via syscall. Guardado por versao p/ nao conflitar
 * com o header do host (glibc >=2.30) que ja declara gettid(). */
#ifndef GTACTW_BUSTER_COMPAT_H
#define GTACTW_BUSTER_COMPAT_H
#include <features.h>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 30)
#    define GTACTW_HAVE_GETTID 1
#  endif
#endif

#ifndef GTACTW_HAVE_GETTID
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
static inline pid_t gettid(void) { return (pid_t)syscall(SYS_gettid); }
#endif

#endif
