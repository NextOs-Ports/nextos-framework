/*
 * bionic_shims.c -- the handful of libc entry points that glibc cannot serve
 * as-is for the Bionic guest (libcocos2dcpp.so + libfmod.so).
 *
 * Everything else in the 458 undefined symbols is plain libc/libm and resolves
 * through so_resolve's dlsym(RTLD_DEFAULT) fallback.  What is here is only what
 * genuinely differs:
 *
 *   __sF                  Bionic's FILE array (stdin/stdout/stderr), 152 B
 *                         stride on LP64.  Those pointers are NOT glibc FILE*.
 *   __errno               Bionic spells __errno_location this way.
 *   __android_log_*       liblog.
 *   __stack_chk_guard     the guest reads it as a plain global.
 *   sigaction             Bionic puts sa_flags FIRST on LP64 -- a different
 *                         struct.  The game only installs its own crash
 *                         handler, which we do not want anyway.
 *   setjmp family         Bionic jmp_buf is 256 B, glibc's tag is 312 B.
 *   sem_*                 Bionic sem_t is a single int; glibc's is 32 B.
 *   dlopen/dlsym          used by libfmod to find its audio backend at
 *                         runtime; we hand it our OpenSL ES shim.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h>

#include "opensles_shim.h"
#include "util.h"

/* ---------------------------------------------------------------- stdio --- */
/* sizeof(struct __sFILE) on Bionic LP64. */
#define BIONIC_FILE_SZ 152
unsigned char __sF[BIONIC_FILE_SZ * 3];

static FILE *map_file(void *f) {
  uintptr_t p = (uintptr_t)f, base = (uintptr_t)__sF;
  if (p >= base && p < base + sizeof(__sF)) {
    int idx = (int)((p - base) / BIONIC_FILE_SZ);
    if (idx == 0)
      return stdin;
    if (idx == 1)
      return stdout;
    return stderr;
  }
  return (FILE *)f;
}

size_t gd_fwrite(const void *p, size_t sz, size_t n, void *f) {
  return fwrite(p, sz, n, map_file(f));
}
size_t gd_fread(void *p, size_t sz, size_t n, void *f) {
  return fread(p, sz, n, map_file(f));
}
int gd_fputs(const char *s, void *f) { return fputs(s, map_file(f)); }
int gd_fputc(int c, void *f) { return fputc(c, map_file(f)); }
int gd_ungetc(int c, void *f) { return ungetc(c, map_file(f)); }
int gd_feof(void *f) { return feof(map_file(f)); }
int gd_ferror(void *f) { return ferror(map_file(f)); }
int gd_fileno(void *f) { return fileno(map_file(f)); }
int gd_fseek(void *f, long off, int wh) { return fseek(map_file(f), off, wh); }
long gd_ftell(void *f) { return ftell(map_file(f)); }
int gd_fseeko(void *f, off_t off, int wh) { return fseeko(map_file(f), off, wh); }
off_t gd_ftello(void *f) { return ftello(map_file(f)); }
char *gd_fgets(char *s, int n, void *f) { return fgets(s, n, map_file(f)); }
int gd_fflush(void *f) { return fflush(f ? map_file(f) : NULL); }
int gd_fclose(void *f) {
  FILE *r = map_file(f);
  if (r == stdin || r == stdout || r == stderr)
    return 0;
  return fclose(r);
}
int gd_getc(void *f) { return getc(map_file(f)); }
int gd_putc(int c, void *f) { return putc(c, map_file(f)); }
void gd_setbuf(void *f, char *buf) { setbuf(map_file(f), buf); }
int gd_setvbuf(void *f, char *buf, int mode, size_t sz) {
  return setvbuf(map_file(f), buf, mode, sz);
}
wint_t gd_getwc(void *f) { return getwc(map_file(f)); }
wint_t gd_putwc(wchar_t c, void *f) { return putwc(c, map_file(f)); }
wint_t gd_ungetwc(wint_t c, void *f) { return ungetwc(c, map_file(f)); }

/* --------------------------------------------------------------- ctype ----
 * Bionic exports `_ctype_` as a POINTER to a 257-byte BSD classification table
 * that the guest indexes at [c + 1] (the -1 slot is EOF).  glibc has no such
 * symbol, and the guest's statically linked libstdc++ reaches for it while
 * parsing floats.  Handing it a null pointer is a segfault on the first
 * long-double conversion, so the table is built here, once, before the guest
 * runs.  The bit values are the BSD ones Bionic inherited. */
#define GD_CT_U 0x01
#define GD_CT_L 0x02
#define GD_CT_N 0x04
#define GD_CT_S 0x08
#define GD_CT_P 0x10
#define GD_CT_C 0x20
#define GD_CT_X 0x40
#define GD_CT_B 0x80

static unsigned char gd_ctype_table[257];
const unsigned char *_ctype_ = gd_ctype_table;

__attribute__((constructor)) static void gd_ctype_init(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char v = 0;
    if (c >= 'A' && c <= 'Z') v |= GD_CT_U;
    if (c >= 'a' && c <= 'z') v |= GD_CT_L;
    if (c >= '0' && c <= '9') v |= GD_CT_N;
    if (c == ' ' || (c >= '\t' && c <= '\r')) v |= GD_CT_S;
    if (c == ' ' || c == '\t') v |= GD_CT_B;
    if (c < 0x20 || c == 0x7f) v |= GD_CT_C;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F'))
      v |= GD_CT_X;
    if (c > 0x20 && c < 0x7f && !(v & (GD_CT_U | GD_CT_L | GD_CT_N)))
      v |= GD_CT_P;
    gd_ctype_table[c + 1] = v;
  }
  gd_ctype_table[0] = 0; /* EOF */
}

/* Bionic's name for the double-precision fpclassify. */
int gd_fpclassifyd(double d) { return __fpclassify(d); }

/* Bionic hooks these around calls that may block, for its own watchdog.  There
 * is no watchdog here; they are pure no-ops. */
void gd_blocking_region_begin(void) {}
void gd_blocking_region_end(void) {}

int gd_fprintf(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_file(f), fmt, ap);
  va_end(ap);
  return r;
}
int gd_vfprintf(void *f, const char *fmt, va_list ap) {
  return vfprintf(map_file(f), fmt, ap);
}

/* --------------------------------------------------------------- errno ---- */
int *gd_errno(void) { return __errno_location(); }

/* ----------------------------------------------------------- android log -- */
static const char lvl[] = "??VDIWEF";

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  char buf[2048];
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(stderr, "[%c/%s] %s\n", lvl[(prio >= 0 && prio < 8) ? prio : 0],
          tag ? tag : "?", buf);
  return 0;
}
int __android_log_write(int prio, const char *tag, const char *text) {
  fprintf(stderr, "[%c/%s] %s\n", lvl[(prio >= 0 && prio < 8) ? prio : 0],
          tag ? tag : "?", text ? text : "");
  return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt,
                         va_list ap) {
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  return __android_log_write(prio, tag, buf);
}
void android_set_abort_message(const char *msg) {
  fprintf(stderr, "[abort] %s\n", msg ? msg : "(null)");
}
void gd_assert2(const char *file, int line, const char *fn, const char *msg) {
  fprintf(stderr, "[assert] %s:%d %s: %s\n", file ? file : "?", line,
          fn ? fn : "?", msg ? msg : "?");
  abort();
}

/* ------------------------------------------------------- stack protector -- */
/* Registered under the name "__stack_chk_guard"; glibc/aarch64 keeps its own
 * canary in TLS, so there is no clash. */
uintptr_t gd_stack_chk_guard = 0x2b1c9f7d5a3e0000ull;
void gd_stack_chk_fail(void) {
  fprintf(stderr, "*** stack smashing detected in guest ***\n");
  abort();
}

/* ------------------------------------------------------------ sigaction --- */
/* Bionic LP64 struct sigaction starts with sa_flags, glibc with sa_handler.
 * The game installs a crash reporter we do not want on top of ours; report
 * success and keep our own handlers. */
int gd_sigaction(int sig, const void *act, void *oldact) {
  (void)sig;
  (void)act;
  if (oldact)
    memset(oldact, 0, 32);
  return 0;
}

/* -------------------------------------------------------------- setjmp ---- */
extern int gd_setjmp(void *env);
extern void gd_longjmp(void *env, int val) __attribute__((noreturn));

int gd_sigsetjmp(void *env, int savesigs) {
  (void)savesigs;
  return gd_setjmp(env);
}
void gd_siglongjmp(void *env, int val) { gd_longjmp(env, val); }

/* ------------------------------------------------------------ semaphore --- */
/* Bionic sem_t is a single 32-bit counter; implement it directly on a futex so
 * we never touch memory the guest did not reserve. */
static int futex_wait(volatile int *addr, int val) {
  return (int)syscall(SYS_futex, (int *)addr, FUTEX_WAIT, val, NULL, NULL, 0);
}
static int futex_wake(volatile int *addr, int n) {
  return (int)syscall(SYS_futex, (int *)addr, FUTEX_WAKE, n, NULL, NULL, 0);
}

int gd_sem_init(void *s, int pshared, unsigned value) {
  (void)pshared;
  __atomic_store_n((int *)s, (int)value, __ATOMIC_SEQ_CST);
  return 0;
}
int gd_sem_destroy(void *s) {
  (void)s;
  return 0;
}
int gd_sem_trywait(void *s) {
  volatile int *p = (volatile int *)s;
  for (;;) {
    int old = __atomic_load_n((int *)p, __ATOMIC_SEQ_CST);
    if (old <= 0) {
      errno = EAGAIN;
      return -1;
    }
    if (__atomic_compare_exchange_n((int *)p, &old, old - 1, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return 0;
  }
}
int gd_sem_wait(void *s) {
  volatile int *p = (volatile int *)s;
  for (;;) {
    int old = __atomic_load_n((int *)p, __ATOMIC_SEQ_CST);
    if (old > 0) {
      if (__atomic_compare_exchange_n((int *)p, &old, old - 1, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
      continue;
    }
    futex_wait(p, old);
  }
}
int gd_sem_post(void *s) {
  volatile int *p = (volatile int *)s;
  __atomic_add_fetch((int *)p, 1, __ATOMIC_SEQ_CST);
  futex_wake(p, 1);
  return 0;
}
int gd_sem_getvalue(void *s, int *out) {
  if (out)
    *out = __atomic_load_n((int *)s, __ATOMIC_SEQ_CST);
  return 0;
}

/* ----------------------------------------------------------------- misc --- */
pid_t gd_gettid(void) { return (pid_t)syscall(SYS_gettid); }

/* Bionic's _FORTIFY helper; the real check is not interesting for us. */
void gd_FD_SET_chk(int fd, void *set, size_t sz) {
  (void)sz;
  if (fd >= 0 && fd < FD_SETSIZE)
    FD_SET(fd, (fd_set *)set);
}

/* -------------------------------------------------------------- dlopen ---- */
/* libfmod probes libaaudio.so first, then libOpenSLES.so.  The device has
 * neither; we answer only for OpenSL ES and serve it from our own shim so the
 * FMOD Android output plugin finds a backend instead of falling back to "no
 * sound device". */
static char gd_opensles_handle;

/* ------------------------------------------------------------- the clock --
 * The engine's frame delta is (now - last) straight off gettimeofday, and its
 * physics then steps one fixed tick at a time for that whole span.  Loading a
 * level out of the 217 MB apk parks the render thread for about a second, so
 * the next frame is asked to simulate a second of collisions in one go and
 * never comes back: the game does not crash, nativeRender simply never
 * returns.
 *
 * Clamping the gap between calls does not help -- the engine calls the clock
 * many times while it loads, so no single gap is long, yet the director still
 * measures a full second from the previous frame.  What works is the frame
 * loop telling the clock afterwards how long it was stalled, and the clock
 * giving that time back: virtual = real - absorbed.  Time still runs at real
 * speed; it just did not run during the stall, exactly as if the game had been
 * paused through it.
 */
static pthread_mutex_t g_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_clock_absorbed_us;

static int64_t clock_real_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int64_t gd_clock_us(void) {
  pthread_mutex_lock(&g_clock_lock);
  int64_t v = clock_real_us() - g_clock_absorbed_us;
  pthread_mutex_unlock(&g_clock_lock);
  return v;
}

/* Called by the frame loop after a frame that took far longer than a frame. */
void gd_clock_absorb_us(int64_t us) {
  if (us <= 0)
    return;
  pthread_mutex_lock(&g_clock_lock);
  g_clock_absorbed_us += us;
  pthread_mutex_unlock(&g_clock_lock);
}

int gd_gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  if (!tv)
    return 0;
  int64_t v = gd_clock_us();
  tv->tv_sec = (time_t)(v / 1000000);
  tv->tv_usec = (suseconds_t)(v % 1000000);
  return 0;
}

void *gd_dlopen(const char *name, int flags) {
  if (name && strstr(name, "libOpenSLES")) {
    debugPrintf("[dl] dlopen(%s) -> opensles shim\n", name);
    return &gd_opensles_handle;
  }
  void *h = name ? dlopen(name, flags) : NULL;
  debugPrintf("[dl] dlopen(%s) -> %p\n", name ? name : "(null)", h);
  return h;
}

void *gd_dlsym(void *handle, const char *name) {
  if (handle == &gd_opensles_handle) {
    if (!strcmp(name, "slCreateEngine"))
      return (void *)&slCreateEngine_shim;
    if (!strcmp(name, "SL_IID_ENGINE"))
      return (void *)&sl_IID_ENGINE;
    if (!strcmp(name, "SL_IID_PLAY"))
      return (void *)&sl_IID_PLAY;
    if (!strcmp(name, "SL_IID_BUFFERQUEUE") ||
        !strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
      return (void *)&sl_IID_BUFFERQUEUE;
    if (!strcmp(name, "SL_IID_VOLUME"))
      return (void *)&sl_IID_VOLUME;
    if (!strcmp(name, "SL_IID_ANDROIDCONFIGURATION")) {
      static const void *iid_androidconfig = "ACFG";
      return (void *)&iid_androidconfig;
    }
    debugPrintf("[dl] dlsym(opensles, %s) -> NULL\n", name);
    return NULL;
  }
  void *r = dlsym(handle ? handle : RTLD_DEFAULT, name);
  debugPrintf("[dl] dlsym(%p, %s) -> %p\n", handle, name, r);
  return r;
}

int gd_dlclose(void *handle) {
  if (handle == &gd_opensles_handle)
    return 0;
  return handle ? dlclose(handle) : 0;
}
char *gd_dlerror(void) { return dlerror(); }

/* --------------------------------------------------------- pthread keys --- */
/* Bionic never hands out key 0; guest code that stores a key in a
 * zero-initialised field treats 0 as "no key".  Burn key 0 if glibc gives it. */
int gd_pthread_key_create(unsigned *key, void (*dtor)(void *)) {
  pthread_key_t k;
  int r = pthread_key_create(&k, dtor);
  if (r == 0 && k == 0) {
    pthread_key_t k2;
    if (pthread_key_create(&k2, dtor) == 0)
      k = k2; /* key 0 stays allocated on purpose */
  }
  if (key)
    *key = (unsigned)k;
  return r;
}
