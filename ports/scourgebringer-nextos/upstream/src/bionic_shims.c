/* bionic_shims.c — shims bionic p/ o so-loader (F1): FORTIFY _chk -> glibc unchecked,
 * __sF (stdio bionic), __system_property_get, ZSTD trace, android_set_abort_message.
 * Exportados (build c/ -rdynamic) -> o fallback dlsym(RTLD_DEFAULT) do so_resolve acha. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* ---- FORTIFY _chk: ignoram o arg de tamanho, chamam a versão glibc ---- */
size_t __strlen_chk(const char *s, size_t n){ (void)n; return strlen(s); }
char  *__strchr_chk(const char *s, int c, size_t n){ (void)n; return (char *)strchr(s, c); }
char  *__strrchr_chk(const char *s, int c, size_t n){ (void)n; return (char *)strrchr(s, c); }
char  *__strcpy_chk(char *d, const char *s, size_t n){ (void)n; return strcpy(d, s); }
char  *__strncpy_chk(char *d, const char *s, size_t n, size_t dn){ (void)dn; return strncpy(d, s, n); }
char  *__strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn){ (void)dn; (void)sn; return strncpy(d, s, n); }
char  *__strcat_chk(char *d, const char *s, size_t dn){ (void)dn; return strcat(d, s); }
char  *__strncat_chk(char *d, const char *s, size_t n, size_t dn){ (void)dn; return strncat(d, s, n); }
void  *__memcpy_chk(void *d, const void *s, size_t n, size_t dn){ (void)dn; return memcpy(d, s, n); }
void  *__memmove_chk(void *d, const void *s, size_t n, size_t dn){ (void)dn; return memmove(d, s, n); }
void  *__memset_chk(void *d, int c, size_t n, size_t dn){ (void)dn; return memset(d, c, n); }
ssize_t __write_chk(int fd, const void *b, size_t n, size_t bn){ (void)bn; return write(fd, b, n); }
ssize_t __read_chk(int fd, void *b, size_t n, size_t bn){ (void)bn; return read(fd, b, n); }
size_t __fread_chk(void *b, size_t size, size_t count, FILE *f, size_t bn){ (void)bn; return fread(b, size, count, f); }
ssize_t __sendto_chk(int fd, const void *b, size_t n, size_t bn, int fl, const struct sockaddr *a, socklen_t al){ (void)bn; return sendto(fd, b, n, fl, a, al); }
void __FD_SET_chk(int fd, void *s, size_t n){ (void)n; FD_SET(fd, (fd_set*)s); }
int  __FD_ISSET_chk(int fd, void *s, size_t n){ (void)n; return FD_ISSET(fd, (fd_set*)s); }
void __FD_CLR_chk(int fd, void *s, size_t n){ (void)n; FD_CLR(fd, (fd_set*)s); }
int __vsnprintf_chk(char *d, size_t n, int fl, size_t dn, const char *f, va_list ap){ (void)fl; (void)dn; return vsnprintf(d, n, f, ap); }
int __snprintf_chk(char *d, size_t n, int fl, size_t dn, const char *f, ...){ va_list ap; va_start(ap,f); int r=vsnprintf(d,n,f,ap); va_end(ap); return r; }
int __vsprintf_chk(char *d, int fl, size_t dn, const char *f, va_list ap){ (void)fl; (void)dn; return vsprintf(d, f, ap); }
int __sprintf_chk(char *d, int fl, size_t dn, const char *f, ...){ va_list ap; va_start(ap,f); int r=vsprintf(d,f,ap); va_end(ap); return r; }

/* ---- bionic misc ---- */
/* __system_property_get: Mono/Xamarin le varias props no boot. Logamos cada
 * pedido e devolvemos valores reais pros conhecidos (TMPDIR etc.) pra nao
 * quebrar paths. Default: string vazia. */
#include <string.h>
int __system_property_get(const char *name, char *value){
    const char *v = "";
    if (name) {
        if (!strcmp(name, "TMPDIR") || !strcmp(name, "java.io.tmpdir"))
            v = "/tmp";
        else if (!strcmp(name, "ro.build.version.sdk"))
            v = "34";
        else if (!strcmp(name, "ro.product.cpu.abi"))
            v = "arm64-v8a";
        else if (!strcmp(name, "debug.mono.log"))
            v = getenv("SB_MONO_LOG") ? getenv("SB_MONO_LOG") : "";  /* ex: "all" */
        fprintf(stderr, "[prop] %s -> '%s'\n", name, v);
    }
    if (value) { strncpy(value, v, 91); value[91] = 0; }
    return (int)strlen(v);
}
void android_set_abort_message(const char *m){ fprintf(stderr, "[abort] %s\n", m?m:""); }

/* ---- ZSTD trace hooks (opcionais): no-op ---- */
unsigned long long ZSTD_trace_compress_begin(void *cctx){ (void)cctx; return 0; }
void ZSTD_trace_compress_end(unsigned long long id, const void *t){ (void)id; (void)t; }
unsigned long long ZSTD_trace_decompress_begin(void *dctx){ (void)dctx; return 0; }
void ZSTD_trace_decompress_end(unsigned long long id, const void *t){ (void)id; (void)t; }

/* ---- __sF: stdio bionic. F1: buffer válido (3 * tamanho generoso); se o jogo
 * passar &__sF[i] p/ stdio glibc no init, tratamos na F1b. ---- */
char __sF[3 * 512];

/* ====== signal ABI: Bionic arm64 x glibc ================================
 *
 * Bionic LP64 exposes an 8-byte sigset_t and a 32-byte struct sigaction:
 *
 *   { int flags; void *handler; uint64_t mask; void *restorer; }
 *
 * glibc uses a 128-byte sigset_t and a 152-byte struct sigaction on aarch64.
 * Calling any host sigset function with guest storage therefore overwrites up
 * to 120 bytes of the Mono caller's stack.  sigaction alone is not enough:
 * Mono also imports sigemptyset/fillset/addset/delset/ismember, sigprocmask,
 * pthread_sigmask and sigsuspend during runtime signal initialization.
 *
 * Keep every guest-facing object in the Bionic layout and translate through a
 * full host sigset_t only inside these wrappers.  Names use bsa_* so glibc's
 * sa_handler/sa_sigaction macros cannot rewrite the guest struct fields. */
#include <signal.h>

typedef uint64_t bionic_sigset_t;

struct bionic_sigaction {
  int bsa_flags;
  void *bsa_handler;
  bionic_sigset_t bsa_mask;
  void *bsa_restorer;
};

_Static_assert(sizeof(bionic_sigset_t) == 8,
               "Bionic arm64 sigset_t must remain 8 bytes");
_Static_assert(sizeof(struct bionic_sigaction) == 32,
               "Bionic arm64 sigaction must remain 32 bytes");

static int bionic_signal_bit(int sig, uint64_t *bit) {
  if (sig < 1 || sig > 64) {
    errno = EINVAL;
    return -1;
  }
  *bit = UINT64_C(1) << (sig - 1);
  return 0;
}

int my_sigemptyset(bionic_sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = 0;
  return 0;
}

int my_sigfillset(bionic_sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = UINT64_MAX;
  return 0;
}

int my_sigaddset(bionic_sigset_t *set, int sig) {
  uint64_t bit;
  if (!set || bionic_signal_bit(sig, &bit) != 0) {
    if (!set) errno = EINVAL;
    return -1;
  }
  *set |= bit;
  return 0;
}

int my_sigdelset(bionic_sigset_t *set, int sig) {
  uint64_t bit;
  if (!set || bionic_signal_bit(sig, &bit) != 0) {
    if (!set) errno = EINVAL;
    return -1;
  }
  *set &= ~bit;
  return 0;
}

int my_sigismember(const bionic_sigset_t *set, int sig) {
  uint64_t bit;
  if (!set || bionic_signal_bit(sig, &bit) != 0) {
    if (!set) errno = EINVAL;
    return -1;
  }
  return (*set & bit) != 0;
}

static void bionic_sigset_to_host(const bionic_sigset_t *bionic,
                                  sigset_t *host) {
  sigemptyset(host);
  if (!bionic) return;
  for (int sig = 1; sig <= 64; ++sig) {
    uint64_t bit = UINT64_C(1) << (sig - 1);
    if (*bionic & bit) sigaddset(host, sig);
  }
}

static bionic_sigset_t host_sigset_to_bionic(const sigset_t *host) {
  bionic_sigset_t bionic = 0;
  for (int sig = 1; sig <= 64; ++sig)
    if (sigismember(host, sig) == 1)
      bionic |= UINT64_C(1) << (sig - 1);
  return bionic;
}

int my_sigprocmask(int how, const bionic_sigset_t *set,
                   bionic_sigset_t *oldset) {
  sigset_t host_set;
  sigset_t host_oldset;
  const sigset_t *host_set_ptr = NULL;
  sigset_t *host_oldset_ptr = oldset ? &host_oldset : NULL;

  if (set) {
    bionic_sigset_to_host(set, &host_set);
    host_set_ptr = &host_set;
  }
  int result = sigprocmask(how, host_set_ptr, host_oldset_ptr);
  if (result == 0 && oldset)
    *oldset = host_sigset_to_bionic(&host_oldset);
  return result;
}

int my_pthread_sigmask(int how, const bionic_sigset_t *set,
                       bionic_sigset_t *oldset) {
  sigset_t host_set;
  sigset_t host_oldset;
  const sigset_t *host_set_ptr = NULL;
  sigset_t *host_oldset_ptr = oldset ? &host_oldset : NULL;
  int saved_errno = errno;

  if (set) {
    bionic_sigset_to_host(set, &host_set);
    host_set_ptr = &host_set;
  }
  int result = pthread_sigmask(how, host_set_ptr, host_oldset_ptr);
  if (result == 0 && oldset)
    *oldset = host_sigset_to_bionic(&host_oldset);
  errno = saved_errno;
  return result;
}

int my_sigsuspend(const bionic_sigset_t *set) {
  if (!set) {
    errno = EFAULT;
    return -1;
  }
  sigset_t host_set;
  bionic_sigset_to_host(set, &host_set);
  return sigsuspend(&host_set);
}

int my_sigpending(bionic_sigset_t *set) {
  if (!set) {
    errno = EFAULT;
    return -1;
  }
  sigset_t host_set;
  int result = sigpending(&host_set);
  if (result == 0) *set = host_sigset_to_bionic(&host_set);
  return result;
}

int my_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *oldact) {
  struct sigaction ga, go; struct sigaction *pga = NULL, *pgo = NULL;
  /* CUP_NOSIGH: NÃO deixa o engine instalar handler de sinais de crash -> nosso
   * handler pega o fault ORIGINAL (em vez do re-raise do crash handler do Unity). */
  if (getenv("CUP_NOSIGH") && (sig==4||sig==5||sig==6||sig==7||sig==8||sig==11)) { (void)oldact; return 0; }
  if (sig==10) { (void)oldact; return 0; }  /* SIGUSR1 = nosso diag_handler; jogo NÃO sobrescreve */
  /* CUP_GCSIG: não deixa o engine/GC sobrescrever nossos handlers de SIGPWR(30)/
     SIGXCPU(24) — nossas threads usam o protocolo de suspensão que NÃO mata. */
  if (getenv("CUP_GCSIG") && (sig==30||sig==24)) { (void)oldact; return 0; }
  if (act) {
    memset(&ga, 0, sizeof ga); ga.sa_flags = act->bsa_flags;
    if (act->bsa_flags & SA_SIGINFO) ga.sa_sigaction = (void (*)(int, siginfo_t *, void *))act->bsa_handler;
    else ga.sa_handler = (void (*)(int))act->bsa_handler;
    bionic_sigset_to_host(&act->bsa_mask, &ga.sa_mask);
    pga = &ga;
  }
  if (oldact) { memset(&go, 0, sizeof go); pgo = &go; }
  int r = sigaction(sig, pga, pgo);
  if (oldact) {
    oldact->bsa_flags = go.sa_flags;
    oldact->bsa_handler = (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction : (void *)go.sa_handler;
    oldact->bsa_mask = host_sigset_to_bionic(&go.sa_mask);
    oldact->bsa_restorer = NULL;
  }
  return r;
}

void __assert2(const char *file, int line, const char *func, const char *failed_expression) {
  fprintf(stderr, "assertion failed in %s:%d (%s): %s\n", file, line, func, failed_expression);
  abort();
}

/* ====== filesystem ABI missing from glibc <= 2.32 =========================
 *
 * On aarch64, glibc only began exporting the public stat/lstat/fstat/fstatat
 * entry points from libc.so.6 in 2.33.  Older systems such as ArkOS' glibc
 * 2.30 keep the callable wrappers in libc_nonshared.a and export __xstat and
 * friends instead.  A dlsym("stat") fallback therefore returns NULL there.
 *
 * libmonodroid reaches stat() during Runtime_init while probing the embedded
 * DSO mode; libmonosgen later imports lstat/stat64/fstat64 too.  Registering
 * these wrappers explicitly keeps every guest import independent of the host
 * dynamic-symbol set.  Bionic and glibc use the same arm64 kernel stat layout
 * (128 bytes, st_mode at +16), so no field translation is needed. */
int sdv_stat(const char *path, struct stat *buf) {
  return stat(path, buf);
}

int sdv_lstat(const char *path, struct stat *buf) {
  return lstat(path, buf);
}

int sdv_fstat(int fd, struct stat *buf) {
  return fstat(fd, buf);
}

int sdv_fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
  return fstatat(dirfd, path, buf, flags);
}

int sdv_mknod(const char *path, mode_t mode, dev_t dev) {
  return mknod(path, mode, dev);
}

/* strlcpy/strlcat only became host-glibc symbols in 2.38. */
size_t sdv_strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size) {
    size_t count = len < size - 1 ? len : size - 1;
    memcpy(dst, src, count);
    dst[count] = '\0';
  }
  return len;
}

size_t sdv_strlcat(char *dst, const char *src, size_t size) {
  size_t dst_len = strnlen(dst, size);
  if (dst_len == size) return size + strlen(src);
  return dst_len + sdv_strlcpy(dst + dst_len, src, size - dst_len);
}

/* arc4random_buf only became a glibc export in 2.36.  The Android
 * libSystem.Native guest uses it to seed System.Random during Activity
 * creation, so leaving the import unresolved on ArkOS/glibc 2.30 makes its
 * PLT entry jump back to the link-time resolver address.  This is the same
 * port-local bridge proven by the approved Stardew Valley Mono-Android port:
 * getrandom(2), then /dev/urandom, with a fully initialized last-resort
 * buffer for the non-cryptographic caller. */
void sdv_arc4random_buf(void *buf, size_t size) {
  unsigned char *bytes = (unsigned char *)buf;
  size_t filled = 0;

  while (filled < size) {
    long result = syscall(SYS_getrandom, bytes + filled, size - filled, 0);
    if (result > 0) {
      filled += (size_t)result;
      continue;
    }
    if (result < 0 && errno == EINTR) continue;
    break;
  }

  if (filled < size) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      while (filled < size) {
        ssize_t result = read(fd, bytes + filled, size - filled);
        if (result > 0) {
          filled += (size_t)result;
          continue;
        }
        if (result < 0 && errno == EINTR) continue;
        break;
      }
      close(fd);
    }
  }

  for (; filled < size; ++filled)
    bytes[filled] = (unsigned char)(filled * 31u + 7u);
}

/* Bionic exports _ctype_ as a pointer variable.  Its table is indexed with
 * c+1 (slot zero is EOF) and its flag bits differ from glibc's tables. */
static char sb_ctype_table[1 + 256];
const char *sb_bionic_ctype = sb_ctype_table;

__attribute__((constructor)) static void sb_ctype_init(void) {
  sb_ctype_table[0] = 0;
  for (int c = 0; c < 256; ++c) {
    unsigned flags = 0;
    if (isupper(c))  flags |= 0x01;
    if (islower(c))  flags |= 0x02;
    if (isdigit(c))  flags |= 0x04;
    if (isspace(c))  flags |= 0x08;
    if (ispunct(c))  flags |= 0x10;
    if (iscntrl(c))  flags |= 0x20;
    if (isxdigit(c)) flags |= 0x40;
    if (c == ' ')    flags |= 0x80;
    sb_ctype_table[c + 1] = (char)flags;
  }
}
