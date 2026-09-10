#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "so_util.h"

/* Android/bionic std streams. On arm64 bionic FILE is 152 bytes; keep a wide
 * region and map any &__sF[i] passed into glibc stdio back to real streams. */
static char magic_sF[3 * 512] __attribute__((aligned(16)));
#define SF_STRIDE 152

static FILE *map_sF(void *fp) {
  uintptr_t base = (uintptr_t)magic_sF;
  uintptr_t p = (uintptr_t)fp;
  if (p >= base && p < base + sizeof(magic_sF)) {
    unsigned idx = (unsigned)((p - base) / SF_STRIDE);
    return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
  }
  return (FILE *)fp;
}

static int sf_fprintf(void *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_sF(fp), fmt, ap);
  va_end(ap);
  return r;
}
static int sf_vfprintf(void *fp, const char *fmt, va_list ap) {
  return vfprintf(map_sF(fp), fmt, ap);
}
static size_t sf_fwrite(const void *p, size_t s, size_t n, void *fp) {
  return fwrite(p, s, n, map_sF(fp));
}
static size_t sf_fread(void *p, size_t s, size_t n, void *fp) {
  return fread(p, s, n, map_sF(fp));
}
static int sf_fputs(const char *s, void *fp) { return fputs(s, map_sF(fp)); }
static int sf_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int sf_putc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int sf_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }
static int sf_fileno(void *fp) { return fileno(map_sF(fp)); }
static int sf_ferror(void *fp) { return ferror(map_sF(fp)); }
static int sf_feof(void *fp) { return feof(map_sF(fp)); }
static void sf_clearerr(void *fp) { clearerr(map_sF(fp)); }
static int sf_setvbuf(void *fp, char *buf, int mode, size_t sz) {
  return setvbuf(map_sF(fp), buf, mode, sz);
}
static int sf_fseek(void *fp, long off, int whence) {
  return fseek(map_sF(fp), off, whence);
}
static long sf_ftell(void *fp) { return ftell(map_sF(fp)); }
static int sf_fseeko(void *fp, off_t off, int whence) {
  return fseeko(map_sF(fp), off, whence);
}
static off_t sf_ftello(void *fp) { return ftello(map_sF(fp)); }
static int sf_getc(void *fp) { return getc(map_sF(fp)); }
static int sf_fgetc(void *fp) { return fgetc(map_sF(fp)); }
static char *sf_fgets(char *s, int n, void *fp) {
  return fgets(s, n, map_sF(fp));
}
static int sf_fclose(void *fp) {
  uintptr_t base = (uintptr_t)magic_sF;
  uintptr_t p = (uintptr_t)fp;
  if (p >= base && p < base + sizeof(magic_sF))
    return 0;
  return fclose((FILE *)fp);
}
static int sf_vfwprintf(void *fp, const wchar_t *fmt, va_list ap) {
  return vfwprintf(map_sF(fp), fmt, ap);
}
static int sf_fwide(void *fp, int mode) { return fwide(map_sF(fp), mode); }

static int *b_errno(void) { return &errno; }

static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[magic/%s] ", tag ? tag : "?");
  vfprintf(stderr, fmt ? fmt : "", ap);
  fputc('\n', stderr);
  va_end(ap);
  return 0;
}

static int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  (void)prio;
  fprintf(stderr, "[magic/%s] ", tag ? tag : "?");
  vfprintf(stderr, fmt ? fmt : "", ap);
  fputc('\n', stderr);
  return 0;
}

static int b_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  fprintf(stderr, "[magic/%s] %s\n", tag ? tag : "?", text ? text : "");
  return 0;
}

static void b_log_assert(const char *cond, const char *tag, const char *fmt,
                         ...) {
  fprintf(stderr, "[magic/%s ASSERT] %s ", tag ? tag : "?",
          cond ? cond : "");
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
  abort();
}

static int b_system_property_get(const char *name, char *value) {
  const char *v = "";
  if (!name)
    v = "";
  else if (!strcmp(name, "ro.build.version.sdk"))
    v = "29";
  else if (!strcmp(name, "ro.build.version.release"))
    v = "10";
  else if (!strcmp(name, "ro.product.model"))
    v = "NextOS Amlogic-old";
  else if (!strcmp(name, "ro.product.manufacturer"))
    v = "Amlogic";
  else if (!strcmp(name, "ro.product.cpu.abi"))
    v = "arm64-v8a";
  else if (!strcmp(name, "ro.hardware"))
    v = "amlogic";
  if (value)
    strcpy(value, v);
  return (int)strlen(v);
}

static void b_android_set_abort_message(const char *msg) {
  fprintf(stderr, "[abort-message] %s\n", msg ? msg : "");
}

static void b_assert2(const char *file, int line, const char *func,
                      const char *expr) {
  fprintf(stderr, "[assert] %s:%d %s: %s\n", file ? file : "?", line,
          func ? func : "?", expr ? expr : "?");
  abort();
}

static size_t b_strlen_chk(const char *s, size_t maxlen) {
  (void)maxlen;
  return strlen(s);
}
static char *b_strchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strchr((char *)s, c);
}
static char *b_strrchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strrchr((char *)s, c);
}
static char *b_strcpy_chk(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}
static char *b_strncpy_chk(char *dst, const char *src, size_t n,
                           size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}
static char *b_strncpy_chk2(char *dst, const char *src, size_t n,
                            size_t dstlen, size_t srclen) {
  (void)dstlen;
  (void)srclen;
  return strncpy(dst, src, n);
}
static char *b_strcat_chk(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}
static char *b_strncat_chk(char *dst, const char *src, size_t n,
                           size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}
static void *b_memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}
static void *b_memmove_chk(void *dst, const void *src, size_t n,
                           size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}
static void *b_memset_chk(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}
static ssize_t b_read_chk(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen;
  return read(fd, buf, count);
}
static ssize_t b_write_chk(int fd, const void *buf, size_t count,
                           size_t buflen) {
  (void)buflen;
  return write(fd, buf, count);
}
static ssize_t b_readlink_chk(const char *path, char *buf, size_t sz,
                              size_t buflen) {
  (void)buflen;
  return readlink(path, buf, sz);
}
static int b_vsnprintf_chk(char *dst, size_t n, int flags, size_t dstlen,
                           const char *fmt, va_list ap) {
  (void)flags;
  (void)dstlen;
  return vsnprintf(dst, n, fmt, ap);
}
static int b_snprintf_chk(char *dst, size_t n, int flags, size_t dstlen,
                          const char *fmt, ...) {
  (void)flags;
  (void)dstlen;
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(dst, n, fmt, ap);
  va_end(ap);
  return r;
}
static int b_vsprintf_chk(char *dst, int flags, size_t dstlen, const char *fmt,
                          va_list ap) {
  (void)flags;
  (void)dstlen;
  return vsprintf(dst, fmt, ap);
}
static int b_sprintf_chk(char *dst, int flags, size_t dstlen, const char *fmt,
                         ...) {
  (void)flags;
  (void)dstlen;
  va_list ap;
  va_start(ap, fmt);
  int r = vsprintf(dst, fmt, ap);
  va_end(ap);
  return r;
}
static mode_t b_umask_chk(mode_t mask) { return umask(mask); }

static void b_FD_SET_chk(int fd, void *set, size_t setsize) {
  (void)setsize;
  FD_SET(fd, (fd_set *)set);
}
static void b_FD_CLR_chk(int fd, void *set, size_t setsize) {
  (void)setsize;
  FD_CLR(fd, (fd_set *)set);
}
static int b_FD_ISSET_chk(int fd, void *set, size_t setsize) {
  (void)setsize;
  return FD_ISSET(fd, (fd_set *)set);
}

static int b_open_2(const char *path, int flags) {
  return open(path, flags, 0666);
}
static int b_openat_2(int dirfd, const char *path, int flags) {
  return openat(dirfd, path, flags, 0666);
}
static pid_t b_gettid(void) { return (pid_t)syscall(SYS_gettid); }

struct bionic_sigaction {
  int bsa_flags;
  void *bsa_handler;
  unsigned long bsa_mask;
  void *bsa_restorer;
};

static int b_sigaction(int sig, const struct bionic_sigaction *act,
                       struct bionic_sigaction *oldact) {
  struct sigaction ga;
  struct sigaction go;
  struct sigaction *pga = NULL;
  struct sigaction *pgo = NULL;

  if (act) {
    memset(&ga, 0, sizeof(ga));
    ga.sa_flags = act->bsa_flags;
    if (act->bsa_flags & SA_SIGINFO)
      ga.sa_sigaction = (void (*)(int, siginfo_t *, void *))act->bsa_handler;
    else
      ga.sa_handler = (void (*)(int))act->bsa_handler;
    sigemptyset(&ga.sa_mask);
    for (int s = 1; s < 64; s++)
      if (act->bsa_mask & (1UL << (s - 1)))
        sigaddset(&ga.sa_mask, s);
    pga = &ga;
  }
  if (oldact) {
    memset(&go, 0, sizeof(go));
    pgo = &go;
  }
  int r = sigaction(sig, pga, pgo);
  if (oldact) {
    oldact->bsa_flags = go.sa_flags;
    oldact->bsa_handler =
        (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction
                                   : (void *)go.sa_handler;
    unsigned long m = 0;
    for (int s = 1; s < 64; s++)
      if (sigismember(&go.sa_mask, s))
        m |= (1UL << (s - 1));
    oldact->bsa_mask = m;
    oldact->bsa_restorer = NULL;
  }
  return r;
}

static int b_pthread_sigmask(int how, const unsigned long *set,
                             unsigned long *oldset) {
  sigset_t gs;
  sigset_t go;
  sigset_t *pgs = NULL;
  sigset_t *pgo = oldset ? &go : NULL;
  if (set) {
    sigemptyset(&gs);
    for (int s = 1; s < 64; s++)
      if (*set & (1UL << (s - 1)))
        sigaddset(&gs, s);
    pgs = &gs;
  }
  int r = pthread_sigmask(how, pgs, pgo);
  if (oldset) {
    unsigned long m = 0;
    for (int s = 1; s < 64; s++)
      if (sigismember(&go, s))
        m |= (1UL << (s - 1));
    *oldset = m;
  }
  return r;
}

#define GL_COMPILE_STATUS 0x8B81

static void (*real_glShaderSource)(unsigned, int, const char *const *,
                                   const int *);
static void (*real_glCompileShader)(unsigned);
static void (*real_glGetShaderiv)(unsigned, unsigned, int *);
static void (*real_glGetShaderInfoLog)(unsigned, int, int *, char *);
static void (*real_glTexParameteri)(unsigned, unsigned, int);
static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned,
                                 unsigned, const void *);

static char *replace_all(const char *src, const char *find, const char *repl) {
  size_t fl = strlen(find);
  size_t rl = strlen(repl);
  size_t n = 0;
  for (const char *p = src; (p = strstr(p, find)); p += fl)
    n++;
  char *out = (char *)malloc(strlen(src) + n * (rl > fl ? rl - fl : 0) + 1);
  if (!out)
    return NULL;
  char *o = out;
  const char *p = src;
  const char *q;
  while ((q = strstr(p, find))) {
    memcpy(o, p, q - p);
    o += q - p;
    memcpy(o, repl, rl);
    o += rl;
    p = q + fl;
  }
  strcpy(o, p);
  return out;
}

static void magic_glShaderSource(unsigned shader, int count,
                                 const char *const *strings,
                                 const int *lengths) {
  if (!real_glShaderSource)
    real_glShaderSource = dlsym(RTLD_DEFAULT, "glShaderSource");
  size_t total = 1;
  for (int i = 0; i < count; i++) {
    if (!strings || !strings[i])
      continue;
    total += lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                        : strlen(strings[i]);
  }
  char *cat = (char *)malloc(total);
  if (!cat) {
    if (real_glShaderSource)
      real_glShaderSource(shader, count, strings, lengths);
    return;
  }
  char *w = cat;
  for (int i = 0; i < count; i++) {
    if (!strings || !strings[i])
      continue;
    size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                          : strlen(strings[i]);
    memcpy(w, strings[i], n);
    w += n;
  }
  *w = 0;

  char *patched = replace_all(cat, "highp", "mediump");
  const char *one = patched ? patched : cat;
  if (real_glShaderSource)
    real_glShaderSource(shader, 1, &one, NULL);
  free(patched);
  free(cat);
}

static void magic_glCompileShader(unsigned shader) {
  if (!real_glCompileShader)
    real_glCompileShader = dlsym(RTLD_DEFAULT, "glCompileShader");
  if (real_glCompileShader)
    real_glCompileShader(shader);
  if (!real_glGetShaderiv)
    real_glGetShaderiv = dlsym(RTLD_DEFAULT, "glGetShaderiv");
  if (!real_glGetShaderInfoLog)
    real_glGetShaderInfoLog = dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
  int status = 1;
  if (real_glGetShaderiv)
    real_glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status != 1 && real_glGetShaderInfoLog) {
    char log[1024] = {0};
    real_glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
    fprintf(stderr, "[magic/gl] shader %u compile fail: %s\n", shader, log);
  }
}

static void magic_glTexParameteri(unsigned target, unsigned pname, int param) {
  if (!real_glTexParameteri)
    real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (pname == 0x2801) {
    if (param == 0x2700 || param == 0x2701 || param == 0x2702 ||
        param == 0x2703)
      param = 0x2601;
  }
  if (pname == 0x813D)
    return;
  if (real_glTexParameteri)
    real_glTexParameteri(target, pname, param);
}

static void magic_glTexImage2D(unsigned target, int level, int internalformat,
                              int width, int height, int border,
                              unsigned format, unsigned type,
                              const void *pixels) {
  if (!real_glTexImage2D)
    real_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
  switch (internalformat) {
  case 0x8058:
  case 0x8057:
  case 0x8056:
    internalformat = 0x1908;
    break;
  case 0x8051:
  case 0x8D62:
    internalformat = 0x1907;
    break;
  case 0x8229:
  case 0x822B:
    internalformat = (int)format;
    break;
  }
  if (real_glTexImage2D)
    real_glTexImage2D(target, level, internalformat, width, height, border,
                      format, type, pixels);
}

DynLibFunction magic_stub_table[] = {
    {"__sF", (uintptr_t)magic_sF},
    {"fprintf", (uintptr_t)sf_fprintf},
    {"vfprintf", (uintptr_t)sf_vfprintf},
    {"fwrite", (uintptr_t)sf_fwrite},
    {"fread", (uintptr_t)sf_fread},
    {"fputs", (uintptr_t)sf_fputs},
    {"fputc", (uintptr_t)sf_fputc},
    {"putc", (uintptr_t)sf_putc},
    {"fflush", (uintptr_t)sf_fflush},
    {"fileno", (uintptr_t)sf_fileno},
    {"ferror", (uintptr_t)sf_ferror},
    {"feof", (uintptr_t)sf_feof},
    {"clearerr", (uintptr_t)sf_clearerr},
    {"setvbuf", (uintptr_t)sf_setvbuf},
    {"fseek", (uintptr_t)sf_fseek},
    {"ftell", (uintptr_t)sf_ftell},
    {"fseeko", (uintptr_t)sf_fseeko},
    {"ftello", (uintptr_t)sf_ftello},
    {"getc", (uintptr_t)sf_getc},
    {"fgetc", (uintptr_t)sf_fgetc},
    {"fgets", (uintptr_t)sf_fgets},
    {"fclose", (uintptr_t)sf_fclose},
    {"vfwprintf", (uintptr_t)sf_vfwprintf},
    {"fwide", (uintptr_t)sf_fwide},
    {"__errno", (uintptr_t)b_errno},
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_vprint", (uintptr_t)b_log_vprint},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"__android_log_assert", (uintptr_t)b_log_assert},
    {"__system_property_get", (uintptr_t)b_system_property_get},
    {"android_set_abort_message", (uintptr_t)b_android_set_abort_message},
    {"__assert2", (uintptr_t)b_assert2},
    {"__strlen_chk", (uintptr_t)b_strlen_chk},
    {"__strchr_chk", (uintptr_t)b_strchr_chk},
    {"__strrchr_chk", (uintptr_t)b_strrchr_chk},
    {"__strcpy_chk", (uintptr_t)b_strcpy_chk},
    {"__strncpy_chk", (uintptr_t)b_strncpy_chk},
    {"__strncpy_chk2", (uintptr_t)b_strncpy_chk2},
    {"__strcat_chk", (uintptr_t)b_strcat_chk},
    {"__strncat_chk", (uintptr_t)b_strncat_chk},
    {"__memcpy_chk", (uintptr_t)b_memcpy_chk},
    {"__memmove_chk", (uintptr_t)b_memmove_chk},
    {"__memset_chk", (uintptr_t)b_memset_chk},
    {"__read_chk", (uintptr_t)b_read_chk},
    {"__write_chk", (uintptr_t)b_write_chk},
    {"__readlink_chk", (uintptr_t)b_readlink_chk},
    {"__vsnprintf_chk", (uintptr_t)b_vsnprintf_chk},
    {"__snprintf_chk", (uintptr_t)b_snprintf_chk},
    {"__vsprintf_chk", (uintptr_t)b_vsprintf_chk},
    {"__sprintf_chk", (uintptr_t)b_sprintf_chk},
    {"__umask_chk", (uintptr_t)b_umask_chk},
    {"__FD_SET_chk", (uintptr_t)b_FD_SET_chk},
    {"__FD_CLR_chk", (uintptr_t)b_FD_CLR_chk},
    {"__FD_ISSET_chk", (uintptr_t)b_FD_ISSET_chk},
    {"__open_2", (uintptr_t)b_open_2},
    {"__openat_2", (uintptr_t)b_openat_2},
    {"gettid", (uintptr_t)b_gettid},
    {"sigaction", (uintptr_t)b_sigaction},
    {"pthread_sigmask", (uintptr_t)b_pthread_sigmask},
    {"glShaderSource", (uintptr_t)magic_glShaderSource},
    {"glCompileShader", (uintptr_t)magic_glCompileShader},
    {"glTexParameteri", (uintptr_t)magic_glTexParameteri},
    {"glTexImage2D", (uintptr_t)magic_glTexImage2D},
};

const int magic_stub_count =
    sizeof(magic_stub_table) / sizeof(magic_stub_table[0]);
