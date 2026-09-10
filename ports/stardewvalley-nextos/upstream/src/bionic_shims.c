/* bionic_shims.c — shims bionic p/ o so-loader (F1): FORTIFY _chk -> glibc unchecked,
 * __sF (stdio bionic), __system_property_get, ZSTD trace, android_set_abort_message.
 * Exportados (build c/ -rdynamic) -> o fallback dlsym(RTLD_DEFAULT) do so_resolve acha. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/select.h>

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
            v = getenv("SDV_MONO_LOG") ? getenv("SDV_MONO_LOG") : "";  /* ex: "all" */
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

/* ====== sigaction/sigprocmask: ABI bionic x glibc (arm64) ======
 * struct sigaction bionic arm64 = { int sa_flags; void* sa_handler; unsigned long sa_mask; void* sa_restorer; } = 32B.
 * glibc = 152B (sigset_t=128B). Se chamar a glibc direto no oldact (buffer bionic de 32B) -> estoura a stack.
 * Campos bsa_* p/ não colidir com as MACROS sa_handler/sa_sigaction da glibc. */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
struct bionic_sigaction { int bsa_flags; void *bsa_handler; unsigned long bsa_mask; void *bsa_restorer; };

/* ====== sigset_t: 8 BYTES no bionic, 128 na glibc ======
 * O guest declara `sigset_t set;` na PILHA (8 bytes) e chama sigemptyset/
 * sigfillset/sigprocmask da glibc, que escrevem 128 -> apagam 120 bytes do
 * frame (registradores salvos + endereco de retorno). Assinatura no dump:
 * uma fileira de x6..x22 = 0xffffffffffffffff (o 0xFF do sigfillset).
 * Convencao do bionic: bit (signo-1) da palavra de 64 bits. */
typedef unsigned long bionic_sigset_t;

static void bset_to_glibc(const bionic_sigset_t *b, sigset_t *g) {
  sigemptyset(g);
  if (!b) return;
  for (int s = 1; s <= 64; s++)
    if (*b & (1UL << (s - 1))) sigaddset(g, s);
}
static void bset_from_glibc(const sigset_t *g, bionic_sigset_t *b) {
  unsigned long m = 0;
  for (int s = 1; s <= 64; s++)
    if (sigismember(g, s) > 0) m |= (1UL << (s - 1));
  *b = m;
}

int sdv_sigemptyset(bionic_sigset_t *s) { if (s) *s = 0UL;  return 0; }
int sdv_sigfillset(bionic_sigset_t *s)  { if (s) *s = ~0UL; return 0; }
int sdv_sigaddset(bionic_sigset_t *s, int sig) {
  if (!s || sig < 1 || sig > 64) return -1;
  *s |= (1UL << (sig - 1)); return 0;
}
int sdv_sigdelset(bionic_sigset_t *s, int sig) {
  if (!s || sig < 1 || sig > 64) return -1;
  *s &= ~(1UL << (sig - 1)); return 0;
}
int sdv_sigismember(const bionic_sigset_t *s, int sig) {
  if (!s || sig < 1 || sig > 64) return -1;
  return (*s & (1UL << (sig - 1))) ? 1 : 0;
}
int sdv_sigprocmask(int how, const bionic_sigset_t *set, bionic_sigset_t *oldset) {
  sigset_t gs, go;
  bset_to_glibc(set, &gs);
  int r = sigprocmask(how, set ? &gs : NULL, oldset ? &go : NULL);
  if (oldset && r == 0) bset_from_glibc(&go, oldset);
  return r;
}
int sdv_pthread_sigmask(int how, const bionic_sigset_t *set, bionic_sigset_t *oldset) {
  sigset_t gs, go;
  bset_to_glibc(set, &gs);
  int r = pthread_sigmask(how, set ? &gs : NULL, oldset ? &go : NULL);
  if (oldset && r == 0) bset_from_glibc(&go, oldset);
  return r;
}
int sdv_sigsuspend(const bionic_sigset_t *set) {
  sigset_t gs; bset_to_glibc(set, &gs);
  return sigsuspend(&gs);
}
int sdv_sigpending(bionic_sigset_t *set) {
  sigset_t gs; int r = sigpending(&gs);
  if (set && r == 0) bset_from_glibc(&gs, set);
  return r;
}
int sdv_sigwait(const bionic_sigset_t *set, int *sig) {
  sigset_t gs; bset_to_glibc(set, &gs);
  return sigwait(&gs, sig);
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
    sigemptyset(&ga.sa_mask);
    for (int s = 1; s < 64; s++) if (act->bsa_mask & (1UL << (s - 1))) sigaddset(&ga.sa_mask, s);
    pga = &ga;
  }
  if (oldact) { memset(&go, 0, sizeof go); pgo = &go; }
  int r = sigaction(sig, pga, pgo);
  /* Saber QUAIS sinais o Mono reserva (suspender/retomar/abortar a thread do GC)
   * e quais handlers ele instala e' o que explica um tgkill "misterioso". */
  if (getenv("SDV_SIGLOG"))
    fprintf(stderr, "[sigaction] sig=%d act=%s handler=%p flags=0x%x -> r=%d\n",
            sig, act ? "set" : "query",
            act ? act->bsa_handler : NULL, act ? (unsigned)act->bsa_flags : 0u, r);
  if (oldact) {
    oldact->bsa_flags = go.sa_flags;
    oldact->bsa_handler = (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction : (void *)go.sa_handler;
    unsigned long m = 0; for (int s = 1; s < 64; s++) if (sigismember(&go.sa_mask, s)) m |= (1UL << (s - 1));
    oldact->bsa_mask = m; oldact->bsa_restorer = NULL;
  }
  return r;
}

void __assert2(const char *file, int line, const char *func, const char *failed_expression) {
  fprintf(stderr, "assertion failed in %s:%d (%s): %s\n", file, line, func, failed_expression);
  abort();
}

/* ====== familia stat: a glibc < 2.33 NAO exporta stat/lstat/fstat/fstatat =====
 * Ate a 2.32 esses nomes moram no libc_nonshared.a e o que a libc.so.6 publica
 * e' __xstat/__lxstat/__fxstat/__fxstatat. Logo o fallback dlsym(RTLD_DEFAULT)
 * do so_resolve devolve NULL no ArkOS (2.30) e o slot da PLT fica com o valor
 * de link -> `br x17` salta pra lixo (foi assim que Util::file_exists matava o
 * Runtime_init do monodroid no R36S). Na NextOS (2.43) tudo isso existe, por
 * isso o Mali-450 nunca viu o problema.
 * Os wrappers abaixo NAO se chamam `stat` — sao registrados por nome na tabela
 * de imports (que tem prioridade sobre o dlsym), evitando colisao com as
 * `extern inline` que o header da glibc antiga define.
 * O `struct stat` do bionic arm64 e' identico ao da glibc arm64 (layout do
 * kernel: st_mode em +16, 128 bytes) -> passthrough direto, sem traducao. */
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

int sdv_stat(const char *path, struct stat *buf)              { return stat(path, buf); }
int sdv_lstat(const char *path, struct stat *buf)             { return lstat(path, buf); }
int sdv_fstat(int fd, struct stat *buf)                       { return fstat(fd, buf); }
int sdv_fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
  return fstatat(dirfd, path, buf, flags);
}
int sdv_mknod(const char *path, mode_t mode, dev_t dev)       { return mknod(path, mode, dev); }

/* strlcpy/strlcat: BSD, so' entraram na glibc 2.38. */
size_t sdv_strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size) {
    size_t n = len < size - 1 ? len : size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}
size_t sdv_strlcat(char *dst, const char *src, size_t size) {
  size_t dl = strnlen(dst, size);
  if (dl == size) return size + strlen(src);
  return dl + sdv_strlcpy(dst + dl, src, size - dl);
}

/* arc4random_buf: glibc 2.36+. Aqui via getrandom(2) com fallback /dev/urandom. */
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
void sdv_arc4random_buf(void *buf, size_t n) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < n) {
    long r = syscall(SYS_getrandom, p + got, n - got, 0);
    if (r > 0) { got += (size_t)r; continue; }
    if (r < 0 && errno == EINTR) continue;
    break;
  }
  if (got < n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
      }
      close(fd);
    }
  }
  /* ultimo recurso: nao deixa buffer indefinido */
  for (; got < n; got++) p[got] = (unsigned char)(got * 31u + 7u);
}
