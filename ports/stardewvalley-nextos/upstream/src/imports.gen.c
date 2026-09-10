/*
 * imports.gen.c -- tabela base de resolucao de imports para o port Stardew.
 *
 * A maioria dos simbolos Bionic (libc, libm, familia _chk, system property,
 * __sF, etc.) eh resolvida automaticamente: os shims em bionic_shims.c sao
 * exportados pelo loader (-rdynamic) e pegos pelo fallback dlsym(RTLD_DEFAULT)
 * do so_resolve; libc, libm e zlib vem das libs reais preloadadas. Aqui ficam
 * os shims que o gtalcs2 mantinha em imports.gen.c (errno + android_log) e os
 * REMAPS de nome (sigaction -> my_sigaction).
 *
 * Conforme o log "UNRESOLVED import" aparecer, adicione a entrada aqui.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include "so_util.h"
#include "sdv_egl_bridge.h"
#include "sdv_monogame_gl_policy.h"

struct bionic_sigaction;
extern int my_sigaction(int sig, const struct bionic_sigaction *act,
                        struct bionic_sigaction *oldact);

#include <signal.h>
#include <pthread.h>
/* Quem manda sinal no proprio processo: o Mono usa raise/pthread_kill/abort em
 * varios caminhos (suspensao do GC, crash chaining, asserts). Sem isto o dump
 * so' mostra o PC dentro do `raise` da glibc e nao diz QUEM pediu. */
extern void sdv_print_addr(const char *label, uintptr_t addr);
static int sdv_raise(int sig) {
    if (getenv("SDV_SIGLOG")) {
        fprintf(stderr, "[raise] sig=%d pedido por:\n", sig);
        sdv_print_addr("  caller", (uintptr_t)__builtin_return_address(0));
    }
    return raise(sig);
}
/* O Mono suspende/retoma cada thread no GC com sinal (SIGPWR/SIGXCPU): em
 * gameplay isso e' varias vezes por segundo, entao o log fica atras da flag. */
static int sdv_pthread_kill(pthread_t t, int sig) {
    if (getenv("SDV_SIGLOG")) {
        fprintf(stderr, "[pthread_kill] sig=%d tid=%p pedido por:\n", sig, (void *)t);
        sdv_print_addr("  caller", (uintptr_t)__builtin_return_address(0));
    }
    return pthread_kill(t, sig);
}
/* O Mono no perfil Android NAO usa pthread_kill: manda sinal com
 * syscall(SYS_tgkill, ...) direto (mono_threads_pthread_kill). Sem interceptar
 * o `syscall` nao da' pra ver quem sinaliza quem. */
#include <sys/syscall.h>
static long sdv_syscall(long n, long a, long b, long c, long d, long e, long f) {
    if ((n == SYS_tgkill || n == SYS_tkill) && getenv("SDV_SIGLOG")) {
        fprintf(stderr, "[tgkill] n=%ld pid=%ld tid=%ld sig=%ld pedido por:\n",
                n, a, b, (n == SYS_tgkill) ? c : b);
        sdv_print_addr("  caller", (uintptr_t)__builtin_return_address(0));
    }
    return syscall(n, a, b, c, d, e, f);
}
static void sdv_abort(void) {
    fprintf(stderr, "[abort] pedido por:\n");
    sdv_print_addr("  caller", (uintptr_t)__builtin_return_address(0));
    abort();
}

/* Familia stat/strlcpy/arc4random: ver bionic_shims.c (glibc antiga nao exporta). */
/* sigset_t bionic (8B) x glibc (128B) — ver bionic_shims.c. */
extern int sdv_sigemptyset(unsigned long *s);
extern int sdv_sigfillset(unsigned long *s);
extern int sdv_sigaddset(unsigned long *s, int sig);
extern int sdv_sigdelset(unsigned long *s, int sig);
extern int sdv_sigismember(const unsigned long *s, int sig);
extern int sdv_sigprocmask(int how, const unsigned long *set, unsigned long *oldset);
extern int sdv_pthread_sigmask(int how, const unsigned long *set, unsigned long *oldset);
extern int sdv_sigsuspend(const unsigned long *set);
extern int sdv_sigpending(unsigned long *set);
extern int sdv_sigwait(const unsigned long *set, int *sig);

extern int sdv_stat(const char *path, struct stat *buf);
extern int sdv_lstat(const char *path, struct stat *buf);
extern int sdv_fstat(int fd, struct stat *buf);
extern int sdv_fstatat(int dirfd, const char *path, struct stat *buf, int flags);
extern int sdv_mknod(const char *path, mode_t mode, dev_t dev);
extern size_t sdv_strlcpy(char *dst, const char *src, size_t size);
extern size_t sdv_strlcat(char *dst, const char *src, size_t size);
extern void sdv_arc4random_buf(void *buf, size_t n);
/* Definidos em main.c — dlopen/dlsym de .so Bionic via nosso so-loader. */
extern void *sdv_so_dlopen(const char *name);
extern void *sdv_so_dlsym(void *handle, const char *name);
extern int sdv_so_is_handle(void *handle);
extern int sdv_so_dlclose(void *handle);
void *sdv_dlopen(const char *filename, int flag);

#define SDV_LIBLOG_HANDLE ((void *)(uintptr_t)0x5344564c4f47ULL)
#define SDV_LIBDL_HANDLE  ((void *)(uintptr_t)0x534456444c00ULL)

static int is_virtual_liblog(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "liblog") == 0 || strcmp(base, "liblog.so") == 0;
}

static int is_virtual_libdl(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "dl") == 0 || strcmp(base, "libdl") == 0 ||
           strcmp(base, "dl.so") == 0 || strcmp(base, "libdl.so") == 0;
}

static int is_monogame_openal(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "libopenal32.so") == 0 ||
           strcmp(base, "libopenal.so") == 0 ||
           strcmp(base, "libopenal.so.1") == 0;
}

static void *open_host_openal(void) {
    static void *handle;
    if (!handle) handle = dlopen("libopenal.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) handle = dlopen("libopenal.so", RTLD_NOW | RTLD_GLOBAL);
    return handle;
}

/* Mali-450 nao oferece ES3. libGLESv3.so existe como symlink enganoso e
 * libGL.so e gl4es; se o MonoGame apenas conseguir dlopen neles, tenta criar
 * contexto 3.x/full GL antes do ES2 e seleciona entrypoints incompatíveis. */
static int is_unsupported_monogame_gl(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "libGLESv3.so") == 0 ||
           strcmp(base, "libGL.so") == 0 ||
           strcmp(base, "libGL.so.1") == 0;
}

/* ---- ICU versionada x ICU "estilo Android" -------------------------------
 * O libSystem.Globalization.Native do .NET Android abre `libicuuc.so` /
 * `libicui18n.so` (sem versao) e resolve os simbolos SEM sufixo, porque no
 * Android a ICU do sistema e' assim. Distro Linux (ArkOS/Ubuntu, ROCKNIX...)
 * so' tem `libicuuc.so.NN` e exporta `u_getVersion_NN` & cia. Sem ponte o .NET
 * aborta com "Unable to load required ICU Globalization data".
 * Aqui: (1) o dlopen tenta as variantes versionadas presentes no aparelho;
 * (2) o dlsym repete a busca com o sufixo `_NN` descoberto.
 * Symlink no dir do port NAO resolve: cartao exfat recusa symlink (errno 524). */
#include <dirent.h>
static void *g_icu_handles[4];
static int   g_icu_nhandles;
static char  g_icu_suffix[16];

static int is_icu_lib(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strncmp(base, "libicuuc.so", 11) == 0 ||
           strncmp(base, "libicui18n.so", 13) == 0 ||
           strncmp(base, "libicudata.so", 13) == 0;
}

/* Varre os dirs de biblioteca atras de `<base>.<versao>` e devolve a versao. */
static int icu_find_version(const char *base, char *out, size_t outsz) {
    static const char *dirs[] = {
        "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", "/lib", NULL };
    size_t blen = strlen(base);
    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (strncmp(e->d_name, base, blen) != 0) continue;
            const char *tail = e->d_name + blen;      /* esperado: ".NN" */
            if (*tail != '.') continue;
            tail++;
            /* so' a versao MAJOR (libicuuc.so.63, nao .63.2) */
            int ok = *tail != '\0';
            for (const char *p = tail; *p; p++) if (*p < '0' || *p > '9') { ok = 0; break; }
            if (!ok) continue;
            snprintf(out, outsz, "%s", tail);
            closedir(dp);
            return 1;
        }
        closedir(dp);
    }
    return 0;
}

static void *icu_dlopen(const char *filename, int flag) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    char stem[32];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strstr(stem, ".so");
    if (dot) dot[3] = '\0';                       /* "libicuuc.so" */

    char ver[16] = "";
    if (!icu_find_version(stem, ver, sizeof ver)) {
        fprintf(stderr, "[icu] nenhuma versao de %s encontrada no sistema\n", stem);
        return NULL;
    }
    char versioned[64];
    snprintf(versioned, sizeof versioned, "%s.%s", stem, ver);
    void *h = dlopen(versioned, flag | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[icu] dlopen(%s) falhou: %s\n", versioned, dlerror());
        return NULL;
    }
    if (!g_icu_suffix[0]) snprintf(g_icu_suffix, sizeof g_icu_suffix, "_%s", ver);
    if (g_icu_nhandles < (int)(sizeof g_icu_handles / sizeof g_icu_handles[0]))
        g_icu_handles[g_icu_nhandles++] = h;
    fprintf(stderr, "[icu] '%s' -> %s (sufixo de simbolo '%s')\n",
            filename, versioned, g_icu_suffix);
    return h;
}

static int is_icu_handle(void *h) {
    for (int i = 0; i < g_icu_nhandles; i++) if (g_icu_handles[i] == h) return 1;
    return 0;
}

/* Bionic arm64: __errno() devolve int* p/ o errno da thread. */
static int *sdv_errno_loc(void) { return __errno_location(); }

/* android_log -> stderr (Mono loga muito no boot; util pra debug). */
int __android_log_write(int prio, const char *tag, const char *text) {
    (void)prio; (void)tag;
    if (text) { fputs("[alog] ", stderr); fputs(text, stderr); fputc('\n', stderr); }
    return 0;
}
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag;
    va_list ap; va_start(ap, fmt);
    fputs("[alog] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio; (void)tag;
    fputs("[alog] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    return 0;
}
void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    (void)cond; (void)tag;
    va_list ap; va_start(ap, fmt);
    fputs("[alog ASSERT] ", stderr);
    if (fmt) vfprintf(stderr, fmt, ap); else fputs("(no msg)", stderr);
    fputc('\n', stderr); va_end(ap);
}

/* FORTIFY umask. */
mode_t __umask_chk(mode_t mask) { return umask(mask); }

/* android_dlopen_ext: Mono/Xamarin carrega libSystem.*.so e blobs AOT com ela.
 * Tentamos dlopen real primeiro (log do que pediu); se falhar, o chamador
 * loga e segue. (Os .so Bionic podem precisar do loader custom depois.) */
void *android_dlopen_ext(const char *filename, int flag, const void *extinfo) {
    (void)extinfo;
    if (is_virtual_liblog(filename)) {
        fprintf(stderr, "[dlopen_ext] '%s' -> liblog virtual\n", filename);
        return SDV_LIBLOG_HANDLE;
    }
    if (is_virtual_libdl(filename)) {
        fprintf(stderr, "[dlopen_ext] '%s' -> libdl virtual\n", filename);
        return SDV_LIBDL_HANDLE;
    }
    if (is_monogame_openal(filename)) {
        void *h = open_host_openal();
        fprintf(stderr, "[dlopen_ext] '%s' -> OpenAL host %p\n", filename, h);
        return h;
    }
    if (is_unsupported_monogame_gl(filename)) {
        fprintf(stderr, "[dlopen_ext] '%s' bloqueada (forca GLES2)\n", filename);
        return NULL;
    }
    void *h = dlopen(filename, flag | RTLD_GLOBAL);
    if (h) {
        fprintf(stderr, "[dlopen_ext] '%s' -> %p (glibc)\n", filename ? filename : "(null)", h);
        return h;
    }
    /* glibc rejeitou (ELF Bionic, "invalid ELF header") — tenta nosso so-loader.
     * Resolve imports contra a tabela combinada (mono+xamarin+shims). */
    fprintf(stderr, "[dlopen_ext] '%s' glibc FAIL -> so-loader\n", filename ? filename : "(null)");
    void *sh = sdv_so_dlopen(filename);
    if (sh) return sh;
    fprintf(stderr, "[dlopen_ext]   so-loader tambem falhou\n");
    return NULL;
}

/* dlsym: se o handle veio do nosso so-loader (sdv_so_dlopen), busca no snapshot
 * do modulo; senao delega pro dlsym da glibc. */
static int sdv_dlclose(void *handle) {
    if (handle == SDV_LIBLOG_HANDLE || handle == SDV_LIBDL_HANDLE) return 0;
    if (sdv_so_is_handle(handle)) return sdv_so_dlclose(handle);
    return dlclose(handle);
}

/* Diagnostico temporario do primeiro quadro. O MonoGame resolve glViewport
 * por dlsym; envolver somente esse simbolo mostra os quatro inteiros exatos
 * antes que cheguem ao Mali, sem alterar o restante do dispatch GL. */
static void (*sdv_real_glViewport)(int, int, int, int);
static void (*sdv_real_glClearColor)(float, float, float, float);
static void (*sdv_real_glClear)(unsigned int);
static void (*sdv_real_glUseProgram)(unsigned int);
static void (*sdv_real_glColorMask)(unsigned char, unsigned char,
                                    unsigned char, unsigned char);
static void (*sdv_real_glScissor)(int, int, int, int);
static void (*sdv_real_glEnable)(unsigned int);
static void (*sdv_real_glDisable)(unsigned int);
static void (*sdv_real_glBindFramebuffer)(unsigned int, unsigned int);
static void (*sdv_real_glGenFramebuffers)(int, unsigned int *);
static void (*sdv_real_glFramebufferTexture2D)(unsigned int, unsigned int,
                                               unsigned int, unsigned int,
                                               int);
static void (*sdv_real_glFramebufferRenderbuffer)(unsigned int, unsigned int,
                                                  unsigned int,
                                                  unsigned int);
static unsigned int (*sdv_real_glCheckFramebufferStatus)(unsigned int);
static void (*sdv_real_glBindRenderbuffer)(unsigned int, unsigned int);
static void (*sdv_real_glRenderbufferStorage)(unsigned int, unsigned int,
                                              int, int);
static void (*sdv_real_glDrawArrays)(unsigned int, int, int);
static void (*sdv_real_glDrawElements)(unsigned int, int, unsigned int,
                                       const void *);
static void (*sdv_real_glVertexAttribPointer)(unsigned int, int, unsigned int,
                                              unsigned char, int,
                                              const void *);
static void (*sdv_real_glBindTexture)(unsigned int, unsigned int);
static unsigned int (*sdv_real_glGetError)(void);
static float sdv_trace_clear_color[4];
static unsigned int sdv_trace_texture;
static unsigned int sdv_trace_fbo;
struct sdv_trace_attrib {
    int size;
    unsigned int type;
    unsigned char normalized;
    int stride;
    const unsigned char *pointer;
};
static struct sdv_trace_attrib sdv_trace_attribs[16];

static int sdv_gl_trace_enabled(void) {
    const char *value = getenv("SDV_GL_TRACE");
    return value && value[0] && value[0] != '0';
}

static void sdv_glViewport_trace(int x, int y, int width, int height) {
    static unsigned int calls;
    if (calls < 40 || x < 0 || y < 0 || width > 4096 || height > 4096)
        fprintf(stderr, "[sdv-gl] glViewport #%u %d,%d %dx%d\n",
                calls + 1, x, y, width, height);
    ++calls;
    if (sdv_real_glViewport)
        sdv_real_glViewport(x, y, width, height);
}

static void sdv_glClearColor_trace(float r, float g, float b, float a) {
    sdv_trace_clear_color[0] = r;
    sdv_trace_clear_color[1] = g;
    sdv_trace_clear_color[2] = b;
    sdv_trace_clear_color[3] = a;
    if (sdv_real_glClearColor) sdv_real_glClearColor(r, g, b, a);
}

static void sdv_glClear_trace(unsigned int mask) {
    static unsigned int calls;
    if (sdv_gl_trace_enabled() && calls < 40)
        fprintf(stderr, "[sdv-gl] glClear #%u mask=%x rgba=%.3f,%.3f,%.3f,%.3f\n",
                calls + 1, mask, sdv_trace_clear_color[0],
                sdv_trace_clear_color[1], sdv_trace_clear_color[2],
                sdv_trace_clear_color[3]);
    ++calls;
    if (sdv_real_glClear) {
        if (getenv("SDV_GL_BRIGHT_CLEAR") && (mask & 0x4000u) &&
            sdv_real_glClearColor)
            sdv_real_glClearColor(0.75f, 0.0f, 0.75f, 1.0f);
        sdv_real_glClear(mask);
    }
}

static void sdv_glUseProgram_trace(unsigned int program) {
    static unsigned int calls;
    if (sdv_gl_trace_enabled() && calls < 40)
        fprintf(stderr, "[sdv-gl] glUseProgram #%u program=%u\n",
                calls + 1, program);
    ++calls;
    if (sdv_real_glUseProgram) sdv_real_glUseProgram(program);
}

static void sdv_glColorMask_trace(unsigned char r, unsigned char g,
                                  unsigned char b, unsigned char a) {
    static unsigned int calls;
    ++calls;
    if (sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glColorMask #%u %u,%u,%u,%u\n",
                calls, r, g, b, a);
    if (sdv_real_glColorMask) sdv_real_glColorMask(r, g, b, a);
}

static void sdv_glScissor_trace(int x, int y, int width, int height) {
    static unsigned int calls;
    ++calls;
    if (sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glScissor #%u %d,%d %dx%d\n",
                calls, x, y, width, height);
    if (sdv_real_glScissor) sdv_real_glScissor(x, y, width, height);
}

static void sdv_glEnable_trace(unsigned int cap) {
    static unsigned int calls;
    ++calls;
    if (sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glEnable #%u cap=%x\n", calls, cap);
    if (sdv_real_glEnable) sdv_real_glEnable(cap);
}

static void sdv_glDisable_trace(unsigned int cap) {
    static unsigned int calls;
    ++calls;
    if (sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glDisable #%u cap=%x\n", calls, cap);
    if (sdv_real_glDisable) sdv_real_glDisable(cap);
}

static void sdv_glBindFramebuffer_trace(unsigned int target,
                                         unsigned int framebuffer) {
    static unsigned int calls;
    if (sdv_gl_trace_enabled() && calls < 40)
        fprintf(stderr, "[sdv-gl] glBindFramebuffer #%u target=%x fbo=%u\n",
                calls + 1, target, framebuffer);
    ++calls;
    sdv_trace_fbo = framebuffer;
    if (sdv_real_glBindFramebuffer)
        sdv_real_glBindFramebuffer(target, framebuffer);
}

static void sdv_glGenFramebuffers_trace(int count, unsigned int *buffers) {
    if (sdv_real_glGenFramebuffers)
        sdv_real_glGenFramebuffers(count, buffers);
    if (sdv_gl_trace_enabled()) {
        fprintf(stderr, "[sdv-gl] glGenFramebuffers count=%d", count);
        for (int i = 0; buffers && i < count && i < 8; ++i)
            fprintf(stderr, " %u", buffers[i]);
        fputc('\n', stderr);
    }
}

static void sdv_glFramebufferTexture2D_trace(unsigned int target,
                                              unsigned int attachment,
                                              unsigned int textarget,
                                              unsigned int texture,
                                              int level) {
    if (sdv_gl_trace_enabled())
        fprintf(stderr,
                "[sdv-gl] glFramebufferTexture2D fbo=%u target=%x attachment=%x texture=%u level=%d\n",
                sdv_trace_fbo, target, attachment, texture, level);
    if (sdv_real_glFramebufferTexture2D)
        sdv_real_glFramebufferTexture2D(target, attachment, textarget,
                                        texture, level);
}

static void sdv_glFramebufferRenderbuffer_trace(unsigned int target,
                                                 unsigned int attachment,
                                                 unsigned int rbtarget,
                                                 unsigned int renderbuffer) {
    if (sdv_gl_trace_enabled())
        fprintf(stderr,
                "[sdv-gl] glFramebufferRenderbuffer fbo=%u attachment=%x rb=%u\n",
                sdv_trace_fbo, attachment, renderbuffer);
    if (sdv_real_glFramebufferRenderbuffer)
        sdv_real_glFramebufferRenderbuffer(target, attachment, rbtarget,
                                           renderbuffer);
}

static unsigned int sdv_glCheckFramebufferStatus_trace(unsigned int target) {
    unsigned int status = sdv_real_glCheckFramebufferStatus
        ? sdv_real_glCheckFramebufferStatus(target) : 0;
    if (sdv_gl_trace_enabled())
        fprintf(stderr, "[sdv-gl] glCheckFramebufferStatus fbo=%u -> %x\n",
                sdv_trace_fbo, status);
    return status;
}

static void sdv_glBindRenderbuffer_trace(unsigned int target,
                                          unsigned int renderbuffer) {
    if (sdv_gl_trace_enabled())
        fprintf(stderr, "[sdv-gl] glBindRenderbuffer target=%x rb=%u\n",
                target, renderbuffer);
    if (sdv_real_glBindRenderbuffer)
        sdv_real_glBindRenderbuffer(target, renderbuffer);
}

static void sdv_glRenderbufferStorage_trace(unsigned int target,
                                             unsigned int format,
                                             int width, int height) {
    if (sdv_gl_trace_enabled())
        fprintf(stderr,
                "[sdv-gl] glRenderbufferStorage target=%x format=%x %dx%d\n",
                target, format, width, height);
    if (sdv_real_glRenderbufferStorage)
        sdv_real_glRenderbufferStorage(target, format, width, height);
}

static void sdv_trace_draw_error(const char *kind, unsigned int call) {
    if (sdv_gl_trace_enabled() && sdv_real_glGetError) {
        unsigned int error = sdv_real_glGetError();
        if (error)
            fprintf(stderr, "[sdv-gl] %s #%u ERROR=%x\n", kind, call, error);
    }
}

static void sdv_glDrawArrays_trace(unsigned int mode, int first, int count) {
    static unsigned int calls;
    ++calls;
    if (sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glDrawArrays #%u mode=%x first=%d count=%d\n",
                calls, mode, first, count);
    if (sdv_real_glDrawArrays) sdv_real_glDrawArrays(mode, first, count);
    sdv_trace_draw_error("glDrawArrays", calls);
}

static void sdv_glVertexAttribPointer_trace(unsigned int index, int size,
                                             unsigned int type,
                                             unsigned char normalized,
                                             int stride, const void *pointer) {
    static unsigned int calls;
    ++calls;
    if (index < 16) {
        sdv_trace_attribs[index].size = size;
        sdv_trace_attribs[index].type = type;
        sdv_trace_attribs[index].normalized = normalized;
        sdv_trace_attribs[index].stride = stride;
        sdv_trace_attribs[index].pointer = (const unsigned char *)pointer;
    }
    if (sdv_gl_trace_enabled() && calls <= 24)
        fprintf(stderr,
                "[sdv-gl] glVertexAttribPointer #%u index=%u size=%d type=%x norm=%u stride=%d ptr=%p\n",
                calls, index, size, type, normalized, stride, pointer);
    if (sdv_real_glVertexAttribPointer)
        sdv_real_glVertexAttribPointer(index, size, type, normalized, stride,
                                       pointer);
}

static void sdv_glBindTexture_trace(unsigned int target, unsigned int texture) {
    sdv_trace_texture = texture;
    if (sdv_real_glBindTexture) sdv_real_glBindTexture(target, texture);
}

static void sdv_dump_draw_attribs(unsigned int call) {
    if (!sdv_gl_trace_enabled() || !(call <= 12 || call % 120u == 0u))
        return;
    fprintf(stderr, "[sdv-gl] draw #%u fbo=%u texture=%u", call,
            sdv_trace_fbo, sdv_trace_texture);
    for (unsigned int index = 0; index < 16; ++index) {
        const struct sdv_trace_attrib *a = &sdv_trace_attribs[index];
        uintptr_t address = (uintptr_t)a->pointer;
        if (!a->pointer || address < 0x10000u) continue;
        if (a->type == 0x1401u && a->size == 4) {
            const unsigned char *v0 = a->pointer;
            const unsigned char *v1 = a->pointer + a->stride;
            const unsigned char *v2 = a->pointer + a->stride * 2;
            const unsigned char *v3 = a->pointer + a->stride * 3;
            fprintf(stderr,
                    " color[%u]=%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x",
                    index, v0[0], v0[1], v0[2], v0[3],
                    v1[0], v1[1], v1[2], v1[3],
                    v2[0], v2[1], v2[2], v2[3],
                    v3[0], v3[1], v3[2], v3[3]);
        } else if (a->type == 0x1406u && a->size >= 2) {
            float xy[2] = {0.0f, 0.0f};
            memcpy(xy, a->pointer, sizeof(xy));
            fprintf(stderr, " float[%u]=%.3f,%.3f", index, xy[0], xy[1]);
        }
    }
    fputc('\n', stderr);
}

static void sdv_glDrawElements_trace(unsigned int mode, int count,
                                      unsigned int type, const void *indices) {
    static unsigned int calls;
    ++calls;
    if (sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr,
                "[sdv-gl] glDrawElements #%u mode=%x count=%d type=%x indices=%p\n",
                calls, mode, count, type, indices);
    sdv_dump_draw_attribs(calls);
    const char *skip = getenv("SDV_GL_SKIP_DRAW");
    int skip_this = skip &&
        ((skip[0] == 'e' && (calls % 2u) == 0u) ||
         (skip[0] == 'o' && (calls % 2u) == 1u));
    if (!skip_this && sdv_real_glDrawElements)
        sdv_real_glDrawElements(mode, count, type, indices);
    else if (skip_this && sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glDrawElements #%u SKIPPED\n", calls);
    sdv_trace_draw_error("glDrawElements", calls);
}

void *sdv_dlsym(void *handle, const char *name) {
    if (handle == SDV_LIBLOG_HANDLE) {
        if (!name) return NULL;
        if (strcmp(name, "__android_log_write") == 0)  return &__android_log_write;
        if (strcmp(name, "__android_log_print") == 0)  return &__android_log_print;
        if (strcmp(name, "__android_log_vprint") == 0) return &__android_log_vprint;
        if (strcmp(name, "__android_log_assert") == 0) return &__android_log_assert;
        return NULL;
    }
    if (handle == SDV_LIBDL_HANDLE) {
        if (!name) return NULL;
        if (strcmp(name, "dlopen") == 0)  return &sdv_dlopen;
        if (strcmp(name, "dlsym") == 0)   return &sdv_dlsym;
        if (strcmp(name, "dlclose") == 0) return &sdv_dlclose;
        if (strcmp(name, "dlerror") == 0) return &dlerror;
        return NULL;
    }
    if (sdv_monogame_hide_global_egl_bind_api(
            handle, name, sdv_egl_ready())) {
        static int reported;
        if (!reported) {
            fprintf(stderr,
                    "[sdv-egl] hidden global eglBindAPI from missing libGL "
                    "probe; keeping existing GLES context\n");
            reported = 1;
        }
        return NULL;
    }
    /* Nunca entregue um handle do loader custom ao ld.so da glibc: se o
     * simbolo estiver ausente, dlsym(handle_fake, ...) pode dereferenciar a
     * nossa struct como link_map e abortar. */
    void *p = sdv_so_is_handle(handle)
        ? sdv_so_dlsym(handle, name)
        : dlsym(handle, name);
    /* ICU de distro exporta `u_getVersion_63`; o shim do .NET pede
     * `u_getVersion`. Repete com o sufixo descoberto no dlopen. */
    if (!p && name && g_icu_suffix[0] && is_icu_handle(handle)) {
        char sufixado[192];
        if (snprintf(sufixado, sizeof sufixado, "%s%s", name, g_icu_suffix)
                < (int)sizeof sufixado)
            p = dlsym(handle, sufixado);
    }
    if (name && name[0] == 'g' && name[1] == 'l' && sdv_egl_ready()) {
        void *context_p = sdv_egl_get_proc_address(name);
        if (context_p) {
            if (sdv_gl_trace_enabled() &&
                (strcmp(name, "glClear") == 0 ||
                 strcmp(name, "glDrawElements") == 0 ||
                 strcmp(name, "glReadPixels") == 0))
                fprintf(stderr, "[sdv-gl] resolve %s dlsym=%p sdl=%p\n",
                        name, p, context_p);
            p = context_p;
        }
    }
    /* Os wrappers temporarios foram úteis no diagnóstico, mas a indireção
     * corrompe o driver Mali após alguns milhares de draws. Mantemos o código
     * como referência de investigação, sem qualquer caminho de ativação. */
    if (0 && p && name) {
#define SDV_GL_TRACE_SYMBOL(symbol, storage, wrapper) do {                   \
        if (strcmp(name, symbol) == 0) {                                     \
            memcpy(&(storage), &p, sizeof(storage));                         \
            return &(wrapper);                                               \
        }                                                                    \
    } while (0)
        SDV_GL_TRACE_SYMBOL("glViewport", sdv_real_glViewport,
                            sdv_glViewport_trace);
        SDV_GL_TRACE_SYMBOL("glClearColor", sdv_real_glClearColor,
                            sdv_glClearColor_trace);
        SDV_GL_TRACE_SYMBOL("glClear", sdv_real_glClear, sdv_glClear_trace);
        SDV_GL_TRACE_SYMBOL("glUseProgram", sdv_real_glUseProgram,
                            sdv_glUseProgram_trace);
        SDV_GL_TRACE_SYMBOL("glColorMask", sdv_real_glColorMask,
                            sdv_glColorMask_trace);
        SDV_GL_TRACE_SYMBOL("glScissor", sdv_real_glScissor,
                            sdv_glScissor_trace);
        SDV_GL_TRACE_SYMBOL("glEnable", sdv_real_glEnable,
                            sdv_glEnable_trace);
        SDV_GL_TRACE_SYMBOL("glDisable", sdv_real_glDisable,
                            sdv_glDisable_trace);
        SDV_GL_TRACE_SYMBOL("glBindFramebuffer", sdv_real_glBindFramebuffer,
                            sdv_glBindFramebuffer_trace);
        SDV_GL_TRACE_SYMBOL("glBindFramebufferEXT", sdv_real_glBindFramebuffer,
                            sdv_glBindFramebuffer_trace);
        SDV_GL_TRACE_SYMBOL("glBindFramebufferOES", sdv_real_glBindFramebuffer,
                            sdv_glBindFramebuffer_trace);
        SDV_GL_TRACE_SYMBOL("glGenFramebuffers", sdv_real_glGenFramebuffers,
                            sdv_glGenFramebuffers_trace);
        SDV_GL_TRACE_SYMBOL("glGenFramebuffersEXT", sdv_real_glGenFramebuffers,
                            sdv_glGenFramebuffers_trace);
        SDV_GL_TRACE_SYMBOL("glGenFramebuffersOES", sdv_real_glGenFramebuffers,
                            sdv_glGenFramebuffers_trace);
        SDV_GL_TRACE_SYMBOL("glFramebufferTexture2D",
                            sdv_real_glFramebufferTexture2D,
                            sdv_glFramebufferTexture2D_trace);
        SDV_GL_TRACE_SYMBOL("glFramebufferTexture2DEXT",
                            sdv_real_glFramebufferTexture2D,
                            sdv_glFramebufferTexture2D_trace);
        SDV_GL_TRACE_SYMBOL("glFramebufferTexture2DOES",
                            sdv_real_glFramebufferTexture2D,
                            sdv_glFramebufferTexture2D_trace);
        SDV_GL_TRACE_SYMBOL("glFramebufferRenderbuffer",
                            sdv_real_glFramebufferRenderbuffer,
                            sdv_glFramebufferRenderbuffer_trace);
        SDV_GL_TRACE_SYMBOL("glFramebufferRenderbufferEXT",
                            sdv_real_glFramebufferRenderbuffer,
                            sdv_glFramebufferRenderbuffer_trace);
        SDV_GL_TRACE_SYMBOL("glCheckFramebufferStatus",
                            sdv_real_glCheckFramebufferStatus,
                            sdv_glCheckFramebufferStatus_trace);
        SDV_GL_TRACE_SYMBOL("glCheckFramebufferStatusEXT",
                            sdv_real_glCheckFramebufferStatus,
                            sdv_glCheckFramebufferStatus_trace);
        SDV_GL_TRACE_SYMBOL("glCheckFramebufferStatusOES",
                            sdv_real_glCheckFramebufferStatus,
                            sdv_glCheckFramebufferStatus_trace);
        SDV_GL_TRACE_SYMBOL("glBindRenderbufferEXT",
                            sdv_real_glBindRenderbuffer,
                            sdv_glBindRenderbuffer_trace);
        SDV_GL_TRACE_SYMBOL("glRenderbufferStorageEXT",
                            sdv_real_glRenderbufferStorage,
                            sdv_glRenderbufferStorage_trace);
        SDV_GL_TRACE_SYMBOL("glDrawArrays", sdv_real_glDrawArrays,
                            sdv_glDrawArrays_trace);
        SDV_GL_TRACE_SYMBOL("glDrawElements", sdv_real_glDrawElements,
                            sdv_glDrawElements_trace);
        SDV_GL_TRACE_SYMBOL("glVertexAttribPointer",
                            sdv_real_glVertexAttribPointer,
                            sdv_glVertexAttribPointer_trace);
        SDV_GL_TRACE_SYMBOL("glBindTexture", sdv_real_glBindTexture,
                            sdv_glBindTexture_trace);
        if (strcmp(name, "glGetError") == 0)
            memcpy(&sdv_real_glGetError, &p, sizeof(sdv_real_glGetError));
#undef SDV_GL_TRACE_SYMBOL
    }
    return p;
}
/* dlopen: igual ao android_dlopen_ext — glibc primeiro, fallback p/ so-loader
 * (monodroid_dlopen chama dlopen direto p/ alguns .so Bionic como libaot-*). */
void *sdv_dlopen(const char *filename, int flag) {
    if (is_virtual_liblog(filename)) {
        fprintf(stderr, "[dlopen] '%s' -> liblog virtual\n", filename);
        return SDV_LIBLOG_HANDLE;
    }
    if (is_virtual_libdl(filename)) {
        fprintf(stderr, "[dlopen] '%s' -> libdl virtual\n", filename);
        return SDV_LIBDL_HANDLE;
    }
    if (is_monogame_openal(filename)) {
        void *h = open_host_openal();
        fprintf(stderr, "[dlopen] '%s' -> OpenAL host %p\n", filename, h);
        return h;
    }
    if (is_unsupported_monogame_gl(filename)) {
        fprintf(stderr, "[dlopen] '%s' bloqueada (forca GLES2)\n", filename);
        return NULL;
    }
    if (is_icu_lib(filename)) {
        void *ih = dlopen(filename, flag | RTLD_GLOBAL);   /* raro, mas se existir */
        return ih ? ih : icu_dlopen(filename, flag);
    }
    void *h = dlopen(filename, flag | RTLD_GLOBAL);
    if (h) return h;
    return sdv_so_dlopen(filename);
}

/* String-ops NULL-safe: a JNI surface fake devolve NULL em offsets de vtable
 * nao populados (ret0); se o Mono passar isso pra strdup/strncmp, glibc crasha.
 * Wrappers NULL-safe deixam o boot avancar (tecnicas de ports Android->Linux). */
char       *sdv_strdup(const char *s)            { return strdup(s ? s : ""); }
size_t       sdv_strlen(const char *s)           { return s ? strlen(s) : 0; }
int          sdv_strcmp(const char *a, const char *b) { return strcmp(a?a:"", b?b:""); }
int          sdv_strncmp(const char *a, const char *b, size_t n) { return strncmp(a?a:"", b?b:"", n); }
char       *sdv_strcat(char *d, const char *s)   { if(d&&s) strcat(d,s); return d; }
char       *sdv_strcpy(char *d, const char *s)   { if(d) strcpy(d, s?s:""); return d; }
char       *sdv_strncpy(char *d, const char *s, size_t n) { if(d) strncpy(d, s?s:"", n); return d; }
char       *sdv_strchr(const char *s, int c)     { return s ? (char *)strchr(s, c) : NULL; }
char       *sdv_strrchr(const char *s, int c)    { return s ? (char *)strrchr(s, c) : NULL; }
char       *sdv_strstr(const char *s, const char *b){ return (s&&b) ? (char *)strstr(s, b) : NULL; }

/* mkdir "sempre sucesso": Mono tenta criar dirs de env (TMPDIR/cache) com paths
 * derivados do contexto Android (NULL/garbage no Linux) -> EINVAL -> entra no
 * path de erro (strerror) que crasha. Retornar 0 evita o path de erro. */
int sdv_mkdir(const char *p, mode_t m){ if (p && *p) mkdir(p, m); return 0; }
/* strerror NULL-safe (Mono loga erros no boot). */
char *sdv_strerror(int err){ return strerror(err); }

/* syslog/vfprintf/fprintf NULL-safe: Mono em estado de erro chama essas com
 * fmt/arg NULL -> glibc faz strlen(NULL) interno (nao interceptavel via GOT).
 * Como SAO imports do libmonodroid, shimamos pra tratar NULL com seguranca. */
#include <stdarg.h>
void sdv_syslog(int pri, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (fmt) { fputs("[syslog] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); }
    va_end(ap);
}
void sdv_vsyslog(int pri, const char *fmt, va_list ap) {
    if (fmt) { fputs("[syslog] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); }
}
FILE *sdv_stream_remap(FILE *);   /* forward decl (definido abaixo) */
int sdv_vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!fmt) return 0;
    return vfprintf(sdv_stream_remap(f), fmt, ap);
}

/* ---- __sF (Bionic stdio FILE array) shim ---------------------------------
 * Mono (compilado pra Bionic) referencia o simbolo __sF@LIBC — o array de
 * FILE estaticos (stdin/stdout/stderr) com o LAYOUT da Bionic. glibc NAO
 * exporta __sF -> o GOT fica NULL -> mono usa "__sF + 0x130" (=stderr na
 * Bionic, sizeof(FILE_bionic)=0x98) como stream -> fwrite(NULL+0x130) crash.
 * Fornecemos um __sF proprio (buffer) e interceptamos fwrite/fprintf/fputs
 * (PLT) pra remapear qualquer stream na regiao __sF -> stdin/stdout/stderr
 * da glibc real. Map por offset: <0x98=stdin, 0x98..0x130=stdout, >=0x130=stderr. */
static char g_sF[0x200];   /* regiao __sF (3 slots de 0x98 = 0x1c8; folga) */
FILE *sdv_stream_remap(FILE *s) {
    if ((char *)s >= g_sF && (char *)s < g_sF + sizeof(g_sF)) {
        size_t off = (size_t)((char *)s - g_sF);
        if (off >= 0x130) return stderr;
        if (off >= 0x98)  return stdout;
        return stdin;
    }
    return s;
}
size_t sdv_fwrite(const void *ptr, size_t sz, size_t n, FILE *s) {
    return fwrite(ptr, sz, n, sdv_stream_remap(s));
}
int sdv_fprintf(FILE *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(sdv_stream_remap(s), fmt ? fmt : "", ap);
    va_end(ap); return r;
}
int sdv_fputs(const char *s2, FILE *s) {
    return fputs(s2 ? s2 : "", sdv_stream_remap(s));
}
int sdv_fputc(int c, FILE *s) {
    return fputc(c, sdv_stream_remap(s));
}

/* sysconf: Mono (Bionic) passa valores _SC da Bionic, que diferem do glibc.
 * sysconf(39) no glibc NAO e _SC_PAGESIZE -> devolve valor errado (ex: 1000),
 * corrompendo mono_pagesize() -> block_size nao pot de 2 -> assertion
 * lock-free-alloc.c:608. Mapeamos os _SC Bionic p/ valores reais. Mono usa
 * {39,40,96,98,99} (Bionic: _SC_PAGESIZE=39, _SC_PAGE_SIZE=40,
 * _SC_NPROCESSORS_CONF=96, _SC_NPROCESSORS_ONLN=97, cache=98/99). */
long sdv_sysconf(int name) {
    switch (name) {
        case 39: case 40: return getpagesize();   /* _SC_PAGESIZE / _SC_PAGE_SIZE */
        case 96: return get_nprocs_conf();        /* _SC_NPROCESSORS_CONF */
        case 97: return get_nprocs();             /* _SC_NPROCESSORS_ONLN */
        case 98: case 99: return 64;              /* cache-line (Bionic), fallback sane */
        default:  return sysconf(name);           /* best-effort p/ outros */
    }
}

/* ---- sem_* (POSIX semaphores) shim ----------------------------------------
 * Mono declara sem_t com o LAYOUT/TAMANHO da Bionic (16B); glibc sem_t tem 32B
 * e layout diferente. glibc sem_init/sem_wait operam em 32B -> overflow/corrupte
 * o semaforo global do mono -> SIGBUS/SEGV em sem_wait. Implementamos sem_*
 * com futex direto sobre offset 0 (count int32) — autossuficiente, cabe em
 * qualquer layout, sem depender da glibc sem_t. */
static int sdv_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *to) {
    return (int)syscall(SYS_futex, uaddr, op, val, to, NULL, 0);
}
int sdv_sem_init(void *sem, int pshared, unsigned int value) {
    (void)pshared;
    *(volatile uint32_t *)sem = value;
    return 0;
}
int sdv_sem_destroy(void *sem) { (void)sem; return 0; }
int sdv_sem_post(void *sem) {
    volatile uint32_t *c = (volatile uint32_t *)sem;
    __sync_fetch_and_add(c, 1);
    sdv_futex((uint32_t *)c, FUTEX_WAKE, 1, NULL);
    return 0;
}
int sdv_sem_wait(void *sem) {
    volatile uint32_t *c = (volatile uint32_t *)sem;
    for (;;) {
        uint32_t v = *c;
        if (v > 0) {
            if (__sync_bool_compare_and_swap(c, v, v - 1)) return 0;
            sched_yield();   /* perdeu a corrida; cede p/ nao busy-spin */
            continue;
        }
        /* count 0: bloqueia no kernel ate ser postado (sem busy-spin) */
        sdv_futex((uint32_t *)c, FUTEX_WAIT, 0, NULL);
    }
}
int sdv_sem_trywait(void *sem) {
    volatile uint32_t *c = (volatile uint32_t *)sem;
    uint32_t v = *c;
    if (v > 0 && __sync_bool_compare_and_swap(c, v, v - 1)) return 0;
    errno = EAGAIN; return -1;
}
int sdv_sem_timedwait(void *sem, const struct timespec *to) {
    volatile uint32_t *c = (volatile uint32_t *)sem;

    if (!to) { errno = EINVAL; return -1; }
    for (;;) {
        uint32_t v = *c;
        if (v > 0 && __sync_bool_compare_and_swap(c, v, v - 1)) return 0;

        /* sem_timedwait recebe um deadline CLOCK_REALTIME absoluto, enquanto
         * FUTEX_WAIT espera uma duracao relativa. Recalcule em todo retry para
         * EINTR/EAGAIN nao estender o timeout original. */
        if (to->tv_nsec < 0 || to->tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return -1;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
        if (to->tv_sec < now.tv_sec ||
            (to->tv_sec == now.tv_sec && to->tv_nsec <= now.tv_nsec)) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct timespec remaining = {
            .tv_sec = to->tv_sec - now.tv_sec,
            .tv_nsec = to->tv_nsec - now.tv_nsec,
        };
        if (remaining.tv_nsec < 0) {
            --remaining.tv_sec;
            remaining.tv_nsec += 1000000000L;
        }

        /* Sempre espere count==0. Se um post ocorreu depois do CAS, o teste
         * atomico do futex devolve EAGAIN e o proximo loop consome o token. */
        if (sdv_futex((uint32_t *)c, FUTEX_WAIT, 0, &remaining) == 0) continue;
        if (errno == ETIMEDOUT) continue; /* ultima tentativa de consumir */
        if (errno != EAGAIN && errno != EINTR) return -1;
    }
}

/* setenv/putenv NULL-safe: Mono faz setenv("TMPDIR", <dir NULL da JNI>, 1);
 * glibc setenv chama strlen(value) internamente e crasha se value=NULL. */
int sdv_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value) { fprintf(stderr, "[setenv] skip %s=%p\n", name?name:"(null)", value); return 0; }
    return setenv(name, value, overwrite);
}
int sdv_putenv(char *str) {
    if (!str) return 0;
    return putenv(str);
}

DynLibFunction dynlib_functions[] = {
    {"__errno",               (uintptr_t)&sdv_errno_loc},
    {"__android_log_write",   (uintptr_t)&__android_log_write},
    {"__android_log_print",   (uintptr_t)&__android_log_print},
    {"__android_log_vprint",  (uintptr_t)&__android_log_vprint},
    {"__android_log_assert",  (uintptr_t)&__android_log_assert},
    {"__umask_chk",           (uintptr_t)&__umask_chk},
    {"android_dlopen_ext",    (uintptr_t)&android_dlopen_ext},
    {"dlopen",                (uintptr_t)&sdv_dlopen},
    {"dlsym",                 (uintptr_t)&sdv_dlsym},
    {"strdup",                (uintptr_t)&sdv_strdup},
    {"strlen",                (uintptr_t)&sdv_strlen},
    {"strcmp",                (uintptr_t)&sdv_strcmp},
    {"strncmp",               (uintptr_t)&sdv_strncmp},
    {"strcat",                (uintptr_t)&sdv_strcat},
    {"strcpy",                (uintptr_t)&sdv_strcpy},
    {"strncpy",               (uintptr_t)&sdv_strncpy},
    {"strchr",                (uintptr_t)&sdv_strchr},
    {"strrchr",               (uintptr_t)&sdv_strrchr},
    {"strstr",                (uintptr_t)&sdv_strstr},
    {"mkdir",                 (uintptr_t)&sdv_mkdir},
    {"strerror",              (uintptr_t)&sdv_strerror},
    {"syslog",                (uintptr_t)&sdv_syslog},
    {"vsyslog",               (uintptr_t)&sdv_vsyslog},
    {"vfprintf",              (uintptr_t)&sdv_vfprintf},
    {"fwrite",                (uintptr_t)&sdv_fwrite},
    {"fprintf",               (uintptr_t)&sdv_fprintf},
    {"fputs",                 (uintptr_t)&sdv_fputs},
    {"fputc",                 (uintptr_t)&sdv_fputc},
    {"__sF",                  (uintptr_t)g_sF},
    {"sysconf",               (uintptr_t)&sdv_sysconf},
    {"sem_init",              (uintptr_t)&sdv_sem_init},
    {"sem_destroy",           (uintptr_t)&sdv_sem_destroy},
    {"sem_wait",              (uintptr_t)&sdv_sem_wait},
    {"sem_post",              (uintptr_t)&sdv_sem_post},
    {"sem_trywait",           (uintptr_t)&sdv_sem_trywait},
    {"sem_timedwait",         (uintptr_t)&sdv_sem_timedwait},
    {"setenv",                (uintptr_t)&sdv_setenv},
    {"putenv",                (uintptr_t)&sdv_putenv},

    /* Bionic arm64 struct sigaction tem 32 bytes; a da glibc tem 152.
     * Se o runtime Mono instalar handlers via sigaction direto, estoura a
     * stack do oldact. Redirecionamos pro tradutor do bionic_shims. */
    {"sigaction",             (uintptr_t)&my_sigaction},

    /* sigset_t do bionic tem 8 bytes; o da glibc, 128. Um sigemptyset/
     * sigfillset da glibc sobre a variavel de pilha do guest apaga 120 bytes
     * do frame (registrador salvo + endereco de retorno) — o smash aparece
     * longe, com os registradores em 0xffffffffffffffff. */
    {"sigemptyset",           (uintptr_t)&sdv_sigemptyset},
    {"sigfillset",            (uintptr_t)&sdv_sigfillset},
    {"sigaddset",             (uintptr_t)&sdv_sigaddset},
    {"sigdelset",             (uintptr_t)&sdv_sigdelset},
    {"sigismember",           (uintptr_t)&sdv_sigismember},
    {"sigprocmask",           (uintptr_t)&sdv_sigprocmask},
    {"pthread_sigmask",       (uintptr_t)&sdv_pthread_sigmask},
    {"sigsuspend",            (uintptr_t)&sdv_sigsuspend},
    {"sigpending",            (uintptr_t)&sdv_sigpending},
    {"sigwait",               (uintptr_t)&sdv_sigwait},

    /* A glibc < 2.33 nao publica stat/lstat/fstat/fstatat/mknod na libc.so.6
     * (moram no libc_nonshared.a; o que ela exporta e' __xstat & cia). No ArkOS
     * do R36S (2.30) o fallback dlsym devolvia NULL e o salto pela PLT ia pra
     * lixo dentro de Util::file_exists, matando o Runtime_init. strlcpy so'
     * entrou na 2.38 e arc4random_buf na 2.36. Entradas na tabela = independem
     * da glibc do aparelho. */
    {"raise",                 (uintptr_t)&sdv_raise},
    {"syscall",               (uintptr_t)&sdv_syscall},
    {"pthread_kill",          (uintptr_t)&sdv_pthread_kill},
    {"abort",                 (uintptr_t)&sdv_abort},
    {"stat",                  (uintptr_t)&sdv_stat},
    {"stat64",                (uintptr_t)&sdv_stat},
    {"lstat",                 (uintptr_t)&sdv_lstat},
    {"lstat64",               (uintptr_t)&sdv_lstat},
    {"fstat",                 (uintptr_t)&sdv_fstat},
    {"fstat64",               (uintptr_t)&sdv_fstat},
    {"fstatat",               (uintptr_t)&sdv_fstatat},
    {"fstatat64",             (uintptr_t)&sdv_fstatat},
    {"mknod",                 (uintptr_t)&sdv_mknod},
    {"strlcpy",               (uintptr_t)&sdv_strlcpy},
    {"strlcat",               (uintptr_t)&sdv_strlcat},
    {"arc4random_buf",        (uintptr_t)&sdv_arc4random_buf},
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
