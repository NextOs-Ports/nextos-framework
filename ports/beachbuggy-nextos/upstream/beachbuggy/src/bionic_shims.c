#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "bionic_shims.h"
#include "opensles_shim.h"
#include "util.h"

#define BIONIC_FILE_SZ 152
unsigned char __sF[BIONIC_FILE_SZ * 3];

static FILE *map_file(void *f) {
  uintptr_t p = (uintptr_t)f, base = (uintptr_t)__sF;
  if (p >= base && p < base + sizeof(__sF)) {
    int idx = (int)((p - base) / BIONIC_FILE_SZ);
    if (idx == 0) return stdin;
    if (idx == 1) return stdout;
    return stderr;
  }
  return (FILE *)f;
}

#define GD_CT_U 0x01
#define GD_CT_L 0x02
#define GD_CT_N 0x04
#define GD_CT_S 0x08
#define GD_CT_P 0x10
#define GD_CT_C 0x20
#define GD_CT_X 0x40
#define GD_CT_B 0x80

static unsigned char bionic_ctype_table[257];
const unsigned char *_ctype_ = bionic_ctype_table;

__attribute__((constructor)) static void bionic_ctype_init(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char v = 0;
    if (c >= 'A' && c <= 'Z') v |= GD_CT_U;
    if (c >= 'a' && c <= 'z') v |= GD_CT_L;
    if (c >= '0' && c <= '9') v |= GD_CT_N;
    if (c == ' ' || (c >= '\t' && c <= '\r')) v |= GD_CT_S;
    if (c == ' ' || c == '\t') v |= GD_CT_B;
    if (c < 0x20 || c == 0x7f) v |= GD_CT_C;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) v |= GD_CT_X;
    if (c > 0x20 && c < 0x7f && !(v & (GD_CT_U | GD_CT_L | GD_CT_N))) v |= GD_CT_P;
    bionic_ctype_table[c + 1] = v;
  }
  bionic_ctype_table[0] = 0;
}

int *bionic_errno(void) { return __errno_location(); }

int bionic_system_property_get(const char *name, char *value) {
  if (!name || !value) return 0;
  if (!strcmp(name, "ro.build.version.sdk")) {
    strcpy(value, "28");
    return (int)strlen(value);
  }
  if (!strcmp(name, "ro.product.model")) {
    strcpy(value, "NextOS Handheld");
    return (int)strlen(value);
  }
  if (!strcmp(name, "ro.product.manufacturer")) {
    strcpy(value, "NextOS");
    return (int)strlen(value);
  }
  value[0] = '\0';
  return 0;
}

static const char lvl[] = "??VDIWEF";

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  char buf[2048];
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debugPrintf("[%c/%s] %s\n", lvl[(prio >= 0 && prio < 8) ? prio : 0], tag ? tag : "?", buf);
  return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  debugPrintf("[%c/%s] %s\n", lvl[(prio >= 0 && prio < 8) ? prio : 0], tag ? tag : "?", text ? text : "");
  return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  return __android_log_write(prio, tag, buf);
}

int bionic_sigaction(int sig, const void *act, void *oldact) {
  (void)sig; (void)act;
  if (oldact) memset(oldact, 0, 32);
  return 0;
}

static int futex_wait(volatile int *addr, int val) {
  return (int)syscall(SYS_futex, (int *)addr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static int futex_wake(volatile int *addr, int n) {
  return (int)syscall(SYS_futex, (int *)addr, FUTEX_WAKE, n, NULL, NULL, 0);
}

int bionic_sem_init(void *s, int pshared, unsigned value) {
  (void)pshared;
  __atomic_store_n((int *)s, (int)value, __ATOMIC_SEQ_CST);
  return 0;
}

int bionic_sem_destroy(void *s) {
  (void)s;
  return 0;
}

int bionic_sem_trywait(void *s) {
  volatile int *p = (volatile int *)s;
  for (;;) {
    int old = __atomic_load_n((int *)p, __ATOMIC_SEQ_CST);
    if (old <= 0) {
      errno = EAGAIN;
      return -1;
    }
    if (__atomic_compare_exchange_n((int *)p, &old, old - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return 0;
  }
}

int bionic_sem_wait(void *s) {
  volatile int *p = (volatile int *)s;
  for (;;) {
    int old = __atomic_load_n((int *)p, __ATOMIC_SEQ_CST);
    if (old > 0) {
      if (__atomic_compare_exchange_n((int *)p, &old, old - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
      continue;
    }
    futex_wait(p, old);
  }
}

int bionic_sem_post(void *s) {
  volatile int *p = (volatile int *)s;
  __atomic_add_fetch((int *)p, 1, __ATOMIC_SEQ_CST);
  futex_wake(p, 1);
  return 0;
}

int bionic_sem_getvalue(void *s, int *out) {
  if (out) *out = __atomic_load_n((int *)s, __ATOMIC_SEQ_CST);
  return 0;
}

static char g_opensles_handle;

void *bionic_dlopen(const char *name, int flags) {
  if (name && (strstr(name, "libOpenSLES") || strstr(name, "libaaudio"))) {
    debugPrintf("[dl] dlopen(%s) -> OpenSL ES shim\n", name);
    return &g_opensles_handle;
  }
  void *h = name ? dlopen(name, flags) : NULL;
  debugPrintf("[dl] dlopen(%s) -> %p\n", name ? name : "(null)", h);
  return h;
}

void *bionic_dlsym(void *handle, const char *name) {
  if (handle == &g_opensles_handle) {
    if (!strcmp(name, "slCreateEngine")) return (void *)&slCreateEngine_shim;
    if (!strcmp(name, "SL_IID_ENGINE")) return (void *)&sl_IID_ENGINE;
    if (!strcmp(name, "SL_IID_PLAY")) return (void *)&sl_IID_PLAY;
    if (!strcmp(name, "SL_IID_BUFFERQUEUE") || !strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
      return (void *)&sl_IID_BUFFERQUEUE;
    if (!strcmp(name, "SL_IID_VOLUME")) return (void *)&sl_IID_VOLUME;
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

int bionic_dlclose(void *handle) {
  if (handle == &g_opensles_handle) return 0;
  return handle ? dlclose(handle) : 0;
}

char *bionic_dlerror(void) { return dlerror(); }
char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len, size_t src_len) {
  (void)dst_len; (void)src_len;
  return strncpy(dst, src, n);
}

void __FD_SET_chk(int fd, void *set, size_t setsize) {
  (void)setsize;
  if (fd >= 0 && fd < FD_SETSIZE) {
    FD_SET(fd, (fd_set *)set);
  }
}

void __assert2(const char *file, int line, const char *func, const char *expr) {
  fprintf(stderr, "Assertion failed: %s (%s:%d: %s)\n", expr ? expr : "?", file ? file : "?", line, func ? func : "?");
  abort();
}

void android_set_abort_message(const char *msg) {
  fprintf(stderr, "Abort message: %s\n", msg ? msg : "(null)");
}
size_t __strlen_chk(const char *s, size_t maxlen) {
  (void)maxlen;
  return strlen(s);
}

int bb_fflush(void *f) { return fflush(f ? map_file(f) : NULL); }

int bb_fprintf(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_file(f), fmt, ap);
  va_end(ap);
  return r;
}

int bb_vfprintf(void *f, const char *fmt, va_list ap) {
  return vfprintf(map_file(f), fmt, ap);
}

size_t bb_fwrite(const void *p, size_t sz, size_t n, void *f) {
  return fwrite(p, sz, n, map_file(f));
}

size_t bb_fread(void *p, size_t sz, size_t n, void *f) {
  return fread(p, sz, n, map_file(f));
}

int bb_fputs(const char *s, void *f) { return fputs(s, map_file(f)); }
int bb_fputc(int c, void *f) { return fputc(c, map_file(f)); }
int bb_ungetc(int c, void *f) { return ungetc(c, map_file(f)); }
int bb_feof(void *f) { return feof(map_file(f)); }
int bb_ferror(void *f) { return ferror(map_file(f)); }
int bb_fileno(void *f) { return fileno(map_file(f)); }
int bb_fseek(void *f, long off, int wh) { return fseek(map_file(f), off, wh); }
long bb_ftell(void *f) { return ftell(map_file(f)); }
char *bb_fgets(char *s, int n, void *f) { return fgets(s, n, map_file(f)); }

int bb_fclose(void *f) {
  FILE *r = map_file(f);
  if (r == stdin || r == stdout || r == stderr) return 0;
  return fclose(r);
}

int bb_getc(void *f) { return getc(map_file(f)); }
int bb_putc(int c, void *f) { return putc(c, map_file(f)); }
void bb_setbuf(void *f, char *buf) { setbuf(map_file(f), buf); }
int bb_setvbuf(void *f, char *buf, int mode, size_t sz) {
  return setvbuf(map_file(f), buf, mode, sz);
}
