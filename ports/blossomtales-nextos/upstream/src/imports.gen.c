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
#include "fbo_compat.h"
#include "etc1.h"
#include "jni_shim.h"

struct bionic_sigaction;
extern int my_sigaction(int sig, const struct bionic_sigaction *act,
                        struct bionic_sigaction *oldact);
/* Definidos em main.c — dlopen/dlsym de .so Bionic via nosso so-loader. */
extern void *sdv_so_dlopen(const char *name);
/* ver host_libs.c: casa nome curto do Android com a lib versionada do host */
extern void *nx_dlopen_host_versioned(const char *name, int flag);
extern void *sdv_so_dlsym(void *handle, const char *name);
extern int sdv_so_is_handle(void *handle);
extern int sdv_so_dlclose(void *handle);
void *sdv_dlopen(const char *filename, int flag);

#define SB_LIBLOG_HANDLE ((void *)(uintptr_t)0x5344564c4f47ULL)
#define SB_LIBDL_HANDLE  ((void *)(uintptr_t)0x534456444c00ULL)

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

/* libaaudio.so virtual: implementada em aaudio_shim.c sobre o SDL2. E por ela
 * que o FMOD abre a saida de audio no Android. */
#define SB_AAUDIO_HANDLE ((void *)(uintptr_t)0x41417544494fULL)
int sb_aaudio_available(void);
void *sb_aaudio_dlsym(const char *name);
static int is_virtual_aaudio(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "libaaudio.so") == 0 || strcmp(base, "aaudio") == 0 ||
           strcmp(base, "libaaudio") == 0;
}

static int is_monogame_openal(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, "libopenal32.so") == 0 ||
           strcmp(base, "libopenal.so") == 0 ||
           strcmp(base, "libopenal.so.1") == 0;
}

/* EOS (Epic Online Services): a lib real crasha no init_array (imports
 * OpenSL/sigsetjmp) e recusar da DllNotFoundException que mata o game loop.
 * Handle virtual: EOS_Initialize retorna != Success e o EpicHelper.Init sai
 * limpo (brtrue p/ o fim) — jogo segue offline/co-op local. */
#define SB_EOS_HANDLE ((void *)(uintptr_t)0xE05E05ULL)
static int is_virtual_eos(const char *filename) {
    if (!filename) return 0;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    /* SOMENTE a lib nativa (DllImport "EOSSDK"). "EOSSDK.Android.so" e
     * afins sao o mono sondando a imagem AOT da assembly EOSSDK.Android.dll
     * — interceptar esses fez o mono ler mono_aot_* do stub e crashar. */
    return strcmp(base, "EOSSDK") == 0 || strcmp(base, "EOSSDK.so") == 0 ||
           strcmp(base, "libEOSSDK") == 0 || strcmp(base, "libEOSSDK.so") == 0;
}
static int64_t eos_stub_fail(void) { return 11; }   /* EOS_NotConfigured */
static const char *eos_stub_version(void) { return "1.16.3-NextOS-stub"; }

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
    if (is_virtual_eos(filename)) {
        fprintf(stderr, "[dlopen_ext] '%s' -> EOS stub virtual\n", filename);
        return SB_EOS_HANDLE;
    }
    if (is_virtual_aaudio(filename)) {
        int ok = sb_aaudio_available();
        fprintf(stderr, "[dlopen_ext] '%s' -> libaaudio virtual (SDL) %s\n", filename,
                ok ? "OK" : "INDISPONIVEL");
        return ok ? SB_AAUDIO_HANDLE : NULL;
    }
    if (is_virtual_liblog(filename)) {
        fprintf(stderr, "[dlopen_ext] '%s' -> liblog virtual\n", filename);
        return SB_LIBLOG_HANDLE;
    }
    if (is_virtual_libdl(filename)) {
        fprintf(stderr, "[dlopen_ext] '%s' -> libdl virtual\n", filename);
        return SB_LIBDL_HANDLE;
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
    /* Antes do so-loader: a lib pode existir no host so com sufixo de versao
     * (libicuuc.so -> libicuuc.so.76). Ver host_libs.c. */
    h = nx_dlopen_host_versioned(filename, flag | RTLD_GLOBAL);
    if (h) return h;
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
/* Handles virtuais NUNCA podem chegar ao dlclose da glibc: o ld.so trata o
 * argumento como link_map e le campos dele (o FMOD faz dlclose no handle de
 * libaaudio.so ao desmontar a saida — isso crashava dentro do ld-linux). */
static int sdv_is_virtual_handle(void *handle) {
    return handle == SB_LIBLOG_HANDLE || handle == SB_LIBDL_HANDLE ||
           handle == SB_EOS_HANDLE || handle == SB_AAUDIO_HANDLE;
}
int sdv_dlclose(void *handle);
int sdv_dlclose(void *handle) {
    if (!handle) return 0;
    if (sdv_is_virtual_handle(handle)) return 0;
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
static void (*sdv_real_glDeleteFramebuffers)(int, const unsigned int *);
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
#define SB_MAX_KNOWN_FBOS 32
static unsigned int sb_known_fbos[SB_MAX_KNOWN_FBOS];

int sdv_gl_copy_known_framebuffers(unsigned int *out, int capacity) {
    int count = 0;
    if (!out || capacity <= 0) return 0;
    for (int i = 0; i < SB_MAX_KNOWN_FBOS && count < capacity; ++i)
        if (sb_known_fbos[i]) out[count++] = sb_known_fbos[i];
    return count;
}

static void sb_remember_fbo(unsigned int id) {
    if (!id) return;
    for (int i = 0; i < SB_MAX_KNOWN_FBOS; ++i) {
        if (sb_known_fbos[i] == id) return;
        if (!sb_known_fbos[i]) {
            sb_known_fbos[i] = id;
            return;
        }
    }
}

static void sb_forget_fbo(unsigned int id) {
    for (int i = 0; i < SB_MAX_KNOWN_FBOS; ++i)
        if (sb_known_fbos[i] == id) sb_known_fbos[i] = 0;
}
struct sdv_trace_attrib {
    int size;
    unsigned int type;
    unsigned char normalized;
    int stride;
    const unsigned char *pointer;
};
static struct sdv_trace_attrib sdv_trace_attribs[16];

static int sdv_gl_trace_enabled(void) {
    const char *value = getenv("SB_GL_TRACE");
    return value && value[0] && value[0] != '0';
}

/* O present proprio precisa conhecer tanto os FBOs quanto o tamanho das
 * texturas anexadas. Isso e uma capacidade de apresentacao, nao uma
 * consequencia do otimizador opcional de texturas. */
static int sb_fbo_tracking_enabled(void) {
    const char *track = getenv("SB_FBO_TRACK");
    const char *present = getenv("SB_PRESENT_FBO");
    return (track && track[0] && track[0] != '0') ||
           (present && present[0] && present[0] != '0');
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
        if (getenv("SB_GL_BRIGHT_CLEAR") && (mask & 0x4000u) &&
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
    for (int i = 0; buffers && i < count; ++i)
        sb_remember_fbo(buffers[i]);
    if (sdv_gl_trace_enabled()) {
        fprintf(stderr, "[sdv-gl] glGenFramebuffers count=%d", count);
        for (int i = 0; buffers && i < count && i < 8; ++i)
            fprintf(stderr, " %u", buffers[i]);
        fputc('\n', stderr);
    }
}

static void sdv_glDeleteFramebuffers_track(int count,
                                            const unsigned int *buffers) {
    for (int i = 0; buffers && i < count; ++i)
        sb_forget_fbo(buffers[i]);
    if (sdv_real_glDeleteFramebuffers)
        sdv_real_glDeleteFramebuffers(count, buffers);
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
    const char *skip = getenv("SB_GL_SKIP_DRAW");
    int skip_this = skip &&
        ((skip[0] == 'e' && (calls % 2u) == 0u) ||
         (skip[0] == 'o' && (calls % 2u) == 1u));
    if (!skip_this && sdv_real_glDrawElements)
        sdv_real_glDrawElements(mode, count, type, indices);
    else if (skip_this && sdv_gl_trace_enabled() && calls <= 80)
        fprintf(stderr, "[sdv-gl] glDrawElements #%u SKIPPED\n", calls);
    sdv_trace_draw_error("glDrawElements", calls);
}

/* ---- ASTC -> RGBA8 (Mali-450 sem ASTC nativo) --------------------------
 * MonoGame gate: SupportsAstc = GL_EXTENSIONS contem astc_ldr. Anunciamos a
 * extensao e decodificamos glCompressedTexImage2D ASTC na CPU (astc_shim.c /
 * libastcUtil, mesmo caminho comprovado do FNA3D-nextos no MCI). Hooks so em
 * funcoes de LOAD (glGetString/glCompressedTexImage2D) — wrappers em funcao
 * de draw corrompem o driver Mali (nota acima). */
int astc_decode_shim(void *dst, int dst_size, const void *src, int src_size,
                     int w, int h, int blk_x, int blk_y);
/* astc_shim.c: cache v2 de pixels finais (pos-downscale/pos-4444) */
void *nextos_tc2_load(const void *src, int src_size, int w, int h,
                      unsigned int flags, int *dw, int *dh,
                      unsigned int *gltype);
void nextos_tc2_store(const void *src, int src_size, int w, int h,
                      unsigned int flags, int dw, int dh, unsigned int gltype,
                      const void *pix, int pix_len);

static const unsigned char *(*sdv_real_glGetString)(unsigned int);
static void (*sdv_real_glCompressedTexImage2D)(unsigned int, int, unsigned int,
                                               int, int, int, int, const void *);
static void (*sdv_real_glTexImage2D)(unsigned int, int, int, int, int, int,
                                     unsigned int, unsigned int, const void *);

static void *sdv_fbo_core_resolve(const char *name, void *userdata) {
    (void)userdata;
    return sdv_egl_get_proc_address(name);
}

static int sdv_extension_present(const char *text, const char *token) {
    size_t length;
    const char *at;
    if (!text || !token || !*token) return 0;
    length = strlen(token);
    at = text;
    while ((at = strstr(at, token)) != NULL) {
        if ((at == text || at[-1] == ' ') &&
            (at[length] == '\0' || at[length] == ' ')) return 1;
        at += length;
    }
    return 0;
}

static const unsigned char *sdv_glGetString_compat(unsigned int pname) {
    const unsigned char *r =
        sdv_real_glGetString ? sdv_real_glGetString(pname) : NULL;
    if (pname == 0x1F03 /* GL_EXTENSIONS */) {
        static char *buf;
        static int initialized;
        if (!initialized) {
            const char *base = r ? (const char *)r : "";
            const char *astc = "GL_KHR_texture_compression_astc_ldr";
            size_t cap = strlen(base) + strlen(astc) + 64u;
            char *candidate;
            int fbo, changed = 0;
            initialized = 1;
            candidate = malloc(cap);
            if (!candidate) return r;
            fbo = bt_fbo_compat_extensions(base, candidate, cap,
                                           sdv_fbo_core_resolve, NULL);
            if (fbo < 0) {
                free(candidate);
                return r;
            }
            if (fbo > 0) {
                changed = 1;
                fprintf(stderr, "[bt/fbo] %s: GL_EXT_framebuffer_object anunciada (closure=13)\n",
                        bt_fbo_compat_marker());
            }
            if (getenv("SB_ASTC") &&
                !sdv_extension_present(candidate, astc)) {
                size_t used = strlen(candidate);
                snprintf(candidate + used, cap - used, "%s%s",
                         used ? " " : "", astc);
                changed = 1;
                fprintf(stderr, "[astc] GL_KHR_texture_compression_astc_ldr anunciada\n");
            }
            if (changed)
                buf = candidate;
            else
                free(candidate);
        }
        if (buf) return (const unsigned char *)buf;
    }
    return r;
}

/* Dims de bloco pelo enum Khronos; -1 se nao-ASTC. */
static int astc_block_dims(unsigned int ifmt, int *bx, int *by) {
    static const unsigned char dims[14][2] = {
        {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
        {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
    };
    unsigned int idx;
    if (ifmt >= 0x93B0 && ifmt <= 0x93BD) idx = ifmt - 0x93B0;
    else if (ifmt >= 0x93D0 && ifmt <= 0x93DD) idx = ifmt - 0x93D0;
    else return -1;
    *bx = dims[idx][0];
    *by = dims[idx][1];
    return 0;
}

static long astc_expected_size(int w, int h, int bx, int by) {
    return (long)((w + bx - 1) / bx) * ((h + by - 1) / by) * 16;
}

/* RAM: converter o RGBA8 decodado p/ RGBA4444 (GL_UNSIGNED_SHORT_4_4_4_4,
 * nativo no Mali-450) corta a VRAM/RAM das texturas ASTC pela METADE. So no
 * caminho ASTC: as fmt=0 (RGBA cruas) podem receber glTexSubImage2D parcial
 * (atlas de fonte) que exige type identico ao da alocacao. SB_TEX16=0 volta
 * a RGBA8 (qualidade maxima) se aparecer banding. */
static int sb_tex16_enabled(void) {
    static int initialized, enabled;
    if (!initialized) {
        /* Todas as texturas do ScourgeBringer sao SurfaceFormat.Color (RGBA8)
         * e o jogo e pixel art: RGBA4444 introduz banding visivel e nao ha
         * pressao de memoria que justifique. Opt-in com SB_TEX16=1. */
        const char *v = getenv("SB_TEX16");
        enabled = v && *v && *v != '0';
        initialized = 1;
    }
    return enabled;
}

/* ETC1 nativo para texturas ASTC realmente opacas. Texturas com alpha ficam
 * em RGBA4444 ate o caminho de dupla camada estar integrado aos shaders. */
static int sb_etc1_enabled(void) {
    static int initialized, enabled;
    if (!initialized) {
        /* Sem texturas ASTC aqui, nao ha o que transcodificar. */
        const char *v = getenv("SB_ETC1");
        enabled = v && *v && *v != '0';
        initialized = 1;
    }
    return enabled;
}

static int sb_rgba_is_opaque(const unsigned char *rgba, size_t px) {
    for (size_t i = 0; i < px; i++)
        if (rgba[i * 4 + 3] < 250)
            return 0;
    return 1;
}
static void sb_rgba8_to_4444(const unsigned char *src, uint16_t *dst,
                               size_t px) {
    for (size_t i = 0; i < px; i++) {
        unsigned r = src[i * 4 + 0], g = src[i * 4 + 1];
        unsigned b = src[i * 4 + 2], a = src[i * 4 + 3];
        r = (r + 8) >> 4; if (r > 15) r = 15;
        g = (g + 8) >> 4; if (g > 15) g = 15;
        b = (b + 8) >> 4; if (b > 15) b = 15;
        a = (a + 8) >> 4; if (a > 15) a = 15;
        dst[i] = (uint16_t)((r << 12) | (g << 8) | (b << 4) | a);
    }
}

/* RGBA4444 tem linhas de width*2 bytes. Com largura impar e o default
 * GL_UNPACK_ALIGNMENT=4, o blob Mali-450 le padding que nao existe no buffer
 * compacto e pode segfaultar dentro de glTexImage2D. Use alinhamento 2 apenas
 * durante o upload e restaure o estado que o MonoGame deixou. */
typedef void (*TmntPixelStoreiFn)(unsigned int, int);
typedef void (*TmntGetIntegervFn)(unsigned int, int *);
static TmntPixelStoreiFn sb_pixel_storei;
static TmntGetIntegervFn sb_get_integerv;
static int sb_unpack_funcs_resolved;

static int sb_unpack_4444_begin(void) {
    int old_alignment = 4;
    if (!sb_unpack_funcs_resolved) {
        void *p;
        sb_unpack_funcs_resolved = 1;
        p = sdv_egl_get_proc_address("glPixelStorei");
        memcpy(&sb_pixel_storei, &p, sizeof sb_pixel_storei);
        p = sdv_egl_get_proc_address("glGetIntegerv");
        memcpy(&sb_get_integerv, &p, sizeof sb_get_integerv);
    }
    if (!sb_pixel_storei || !sb_get_integerv)
        return -1;
    sb_get_integerv(0x0CF5u /* GL_UNPACK_ALIGNMENT */, &old_alignment);
    if (old_alignment != 2)
        sb_pixel_storei(0x0CF5u /* GL_UNPACK_ALIGNMENT */, 2);
    return old_alignment;
}

static void sb_unpack_4444_end(int old_alignment) {
    if (old_alignment > 0 && old_alignment != 2 && sb_pixel_storei)
        sb_pixel_storei(0x0CF5u /* GL_UNPACK_ALIGNMENT */, old_alignment);
}

/* THRASH de RAM: mali mem de textura e NAO-swappavel; o preload inteiro passa
 * de 250MB e afoga o box de 832MB (sshd faminto, loading nunca termina).
 * Downscale 2x (box filter) dos atlas ASTC grandes corta a RAM de textura a
 * 1/4 — as UVs normalizadas do MonoGame nao percebem a dimensao real do GL.
 * SB_TEXSCALE=0 desliga; SB_TEXSCALE_MIN=N muda o limiar (default 512,
 * comparado por area: w*h >= N*N). Run diagnostico com 1024: mali bateu 362MB
 * em 40s — o grosso e a cauda de texturas medias, nao os atlas gigantes. */
static int sb_texscale_min(void) {
    static int initialized, min_dim;
    if (!initialized) {
        /* Politica herdada do TMNT (assets ASTC pesados). Aqui os atlas sao
         * pixel art e reduzi-los borra a arte inteira: desligado por padrao. */
        min_dim = -1;
        const char *m = getenv("SB_TEXSCALE_MIN");
        if (m && atoi(m) > 0) min_dim = atoi(m);
        initialized = 1;
    }
    return min_dim;
}
/* O perfil geral reduz centenas de sprites durante o preload, mas estes dois
 * atlas compoem exatamente a tela de titulo e perdiam nitidez demais em 720p.
 * Mantê-los full-size custa ~5 MiB no RGBA4444, sem abrir a porteira para o
 * restante do jogo. SB_MENU_FULL=0 permite comparar o perfil antigo. */
static int sb_keep_menu_texture_full(int w, int h) {
    static int initialized, enabled;
    if (!initialized) {
        const char *v = getenv("SB_MENU_FULL");
        enabled = !(v && v[0] == '0');
        initialized = 1;
    }
    if (!enabled) return 0;
    return (w == 1596 && h == 1705) || /* MainMenuTexture */
           (w == 889 && h == 914);     /* TitleScreenTexture */
}

/* O atlas principal do cenario so entra quando a fase correspondente e
 * carregada. Reduzi-lo pela metade (ex.: Level01 1691x964 -> 845x482) e
 * depois amplia-lo para o FBO 1920x1080 era a principal origem do aspecto
 * embaçado. Mantemos apenas BG/Level* da fase ativa em full; personagens,
 * inimigos e fontes possuem seus perfis seletivos próprios. */
static int sb_keep_stage_bg_texture_full(const char *path, int w, int h) {
    static int initialized, enabled;
    if (!initialized) {
        const char *v = getenv("SB_STAGE_BG_FULL");
        enabled = !(v && v[0] == '0');
        initialized = 1;
    }
    if (!enabled || !path || w <= 0 || h <= 0) return 0;
    return strstr(path, "/2d/Animations/BG/Level") != NULL;
}

/* Casa somente Bosses/Nome/NomeTexture.xnb. Projeteis, FX e clones ficam no
 * perfil geral; carregar os 53 XNB de Bosses em full no preload gastaria a
 * folga inteira. O chamador ainda exige que o preload de jogadores tenha
 * terminado, então apenas o bundle da fase/boss ativo ganha qualidade. */
static int sb_keep_main_boss_texture_full(const char *path) {
    static int initialized, enabled;
    static const char marker[] = "/2d/Animations/Bosses/";
    if (!initialized) {
        const char *v = getenv("SB_BOSS_FULL");
        enabled = !(v && v[0] == '0');
        initialized = 1;
    }
    if (!enabled || !path) return 0;
    const char *dir = strstr(path, marker);
    if (!dir) return 0;
    dir += sizeof(marker) - 1;
    const char *slash = strchr(dir, '/');
    if (!slash || slash == dir || strchr(slash + 1, '/')) return 0;
    size_t name_len = (size_t)(slash - dir);
    const char *base = slash + 1;
    return strncmp(base, dir, name_len) == 0 &&
           strcmp(base + name_len, "Texture.xnb") == 0;
}
static void sb_rgba8_halve(const unsigned char *src, int w, int h,
                             unsigned char *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy0 = y * 2, sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * 2, sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
            const unsigned char *p00 = src + ((size_t)sy0 * w + sx0) * 4;
            const unsigned char *p01 = src + ((size_t)sy0 * w + sx1) * 4;
            const unsigned char *p10 = src + ((size_t)sy1 * w + sx0) * 4;
            const unsigned char *p11 = src + ((size_t)sy1 * w + sx1) * 4;
            unsigned char *d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++)
                d[c] = (unsigned char)((p00[c] + p01[c] + p10[c] + p11[c] + 2) >> 2);
        }
    }
}

/* Texturas com Palette.xnb nao contem cores finais: cada texel codifica um
 * indice. Media/4444 ou escolher o maior canal inventa/privilegia indices e
 * transforma o personagem. Nearest-center apenas seleciona um texel original
 * inteiro; nenhuma componente e interpretada como cor ou alpha. */
static void sb_rgba8_resample_indexed(const unsigned char *src, int w, int h,
                                        unsigned char *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = (int)(((long long)(2 * y + 1) * h) / (2 * dh));
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)(((long long)(2 * x + 1) * w) / (2 * dw));
            if (sx >= w) sx = w - 1;
            memcpy(dst + ((size_t)y * dw + x) * 4,
                   src + ((size_t)sy * w + sx) * 4, 4);
        }
    }
}

static int sb_rgba8_looks_indexed(const unsigned char *src, size_t px) {
    /* Fundo transparente canonico dos atlases paletizados Paris. Uma pequena
     * contagem evita falso positivo por um unico pixel coincidente. */
    size_t limit = px < 65536 ? px : 65536;
    unsigned hits = 0;
    for (size_t i = 0; i < limit; i++) {
        const unsigned char *p = src + i * 4;
        /* XNB Color chega ao upload em ordem A/R/G/B neste build. */
        if (p[0] == 0 && p[1] == 0xd3 && p[2] == 0xd3 && p[3] == 0xff) {
            if (++hits >= 16) return 1;
        }
    }
    return 0;
}

/* ---- RAW RGBA8888 -> meia-res RGBA4444 (sheets fmt=0) -------------------
 * O OOM real nao vinha do caminho ASTC: os sheets grandes (XxxTexture) sao
 * RGBA8888 cruas via glTexImage2D(NULL) + glTexSubImage2D full-rect
 * (PlatformConstruct/SetData do MonoGame). Para os sheets inequivocamente
 * grandes, o storage ja nasce em meia-res RGBA4444 e todo upload e convertido
 * para o mesmo formato. Render targets widescreen ficam intactos. Nao ha
 * wrapper em glBindTexture (frame path corrompe o blob Mali; nota abaixo) —
 * o id vem de glGetIntegerv(GL_TEXTURE_BINDING_2D) dentro dos uploads.
 * SB_RAWSCALE=0 desliga; SB_RAWSCALE_MIN=N (default 1536, ambas as
 * dimensoes >= N). Exigir as duas dimensoes separa os sheets grandes dos
 * render targets widescreen 1920x1080 usados pela versao Netflix. */
typedef struct {
    unsigned id;
    unsigned short w, h, gpu_w, gpu_h;
    unsigned char scaled, qualifies, palette_exact, font_quality;
    unsigned char stream_group, stream_full, stream_evicted, stream_registered;
    unsigned use_clock;
    char *source_path;
} RawTexEnt;
static RawTexEnt raw_tex_tab[1024];
#define RAW_TEX_DIRECT_MAX 8192
static RawTexEnt *raw_tex_direct[RAW_TEX_DIRECT_MAX];
static RawTexEnt *raw_tex_find(unsigned id, int create) {
    RawTexEnt *free_slot = NULL;
    if (!id) return NULL;
    if (id < RAW_TEX_DIRECT_MAX) {
        RawTexEnt *direct = raw_tex_direct[id];
        if (direct && direct->id == id) return direct;
        /* Todo registro abaixo de RAW_TEX_DIRECT_MAX entra no mapa no mesmo
         * instante em que e criado. Logo NULL tambem e uma resposta O(1),
         * crucial no glBindTexture: a maioria dos IDs ASTC nao pertence ao
         * pager e antes fazia uma varredura de 1024 slots a cada draw. */
        if (!create) return NULL;
    }
    for (int i = 0; i < 1024; i++) {
        if (raw_tex_tab[i].id == id) {
            if (id < RAW_TEX_DIRECT_MAX) raw_tex_direct[id] = &raw_tex_tab[i];
            return &raw_tex_tab[i];
        }
        if (!raw_tex_tab[i].id && !free_slot) free_slot = &raw_tex_tab[i];
    }
    if (!create || !free_slot) return NULL;
    free_slot->id = id;
    if (id < RAW_TEX_DIRECT_MAX) raw_tex_direct[id] = free_slot;
    return free_slot;
}
int sdv_gl_texture_dimensions(unsigned int id, int *w, int *h) {
    RawTexEnt *e = raw_tex_find(id, 0);
    if (!e || !e->w || !e->h) return 0;
    if (w) *w = e->gpu_w ? e->gpu_w : e->w;
    if (h) *h = e->gpu_h ? e->gpu_h : e->h;
    return 1;
}
static int sb_rawscale_min(void) {
    static int initialized, min_dim;
    if (!initialized) {
        /* Mesmo motivo do texscale: os atlas 2048x2048 do jogo sao pixel art e
         * cabem na RAM sem reducao. Opt-in com SB_RAWSCALE_MIN=N. */
        min_dim = -1;
        const char *m = getenv("SB_RAWSCALE_MIN");
        if (m && atoi(m) > 0) min_dim = atoi(m);
        initialized = 1;
    }
    return min_dim;
}
static void (*sdv_real_glTexImage2D_raw)(unsigned int, int, int, int, int,
                                         int, unsigned int, unsigned int,
                                         const void *);
static void (*sdv_real_glTexSubImage2D)(unsigned int, int, int, int, int,
                                        int, unsigned int, unsigned int,
                                        const void *);
static void (*sdv_real_glDeleteTextures)(int, const unsigned int *);
static unsigned raw_bound_tex2d(void) {
    static void (*get_integerv)(unsigned int, int *);
    if (!get_integerv) {
        void *p = sdv_egl_get_proc_address("glGetIntegerv");
        memcpy(&get_integerv, &p, sizeof p);
    }
    int id = 0;
    if (get_integerv) get_integerv(0x8069 /* TEXTURE_BINDING_2D */, &id);
    return (unsigned)id;
}

/* ---- full-res seletivo dos atlas paletizados ---------------------------
 * O jogo faz preload dos 11 sheets de jogador (304 MiB RGBA8), embora a fase
 * use apenas o escolhido. Guardamos o caminho do XNB, reduzimos no preload e
 * descartamos os sheets inativos para 1x1. No primeiro bind real, lemos o XNB
 * original e redefinimos SOMENTE o id ativo em full-res. Inimigos usam o
 * mesmo mecanismo com LRU separado. GL continua exclusivamente na render
 * thread; o I/O ocorre uma vez na transicao/menu e nao no frame steady-state. */
static unsigned palette_stream_clock;
static unsigned palette_player_registered;
static int palette_players_pruned;

static int sb_palette_stream_enabled(void) {
    static int initialized, enabled;
    if (!initialized) {
        const char *v = getenv("SB_PALETTE_STREAM");
        enabled = v && v[0] && v[0] != '0';
        initialized = 1;
    }
    return enabled;
}

static int palette_stream_group_for_path(const char *path) {
    if (!path) return 0;
    if (strstr(path, "/2d/Animations/Players/")) return 1;
    if (strstr(path, "/2d/Animations/Enemies/") ||
        strstr(path, "/2d/Animations/Bosses/")) return 2;
    return 0;
}

static void palette_stream_attach(RawTexEnt *e) {
    if (!e || !e->palette_exact || !sb_palette_stream_enabled()) return;
    char path[4096];
    if (!jni_copy_last_texture_asset_path(path, sizeof path)) return;
    int group = palette_stream_group_for_path(path);
    if (!group) return;
    if (!e->source_path || strcmp(e->source_path, path) != 0) {
        char *copy = strdup(path);
        if (!copy) return;
        free(e->source_path);
        e->source_path = copy;
    }
    e->stream_group = (unsigned char)group;
    if (!e->stream_registered) {
        e->stream_registered = 1;
        if (group == 1) palette_player_registered++;
        fprintf(stderr, "[palette-stream] registra tex %u grupo=%d %s\n",
                e->id, group, path);
    }
}

static int palette_stream_resolve_gl(void) {
    if (!sdv_real_glBindTexture) {
        void *p = sdv_egl_get_proc_address("glBindTexture");
        memcpy(&sdv_real_glBindTexture, &p, sizeof p);
    }
    if (!sdv_real_glTexImage2D_raw) {
        void *p = sdv_egl_get_proc_address("glTexImage2D");
        memcpy(&sdv_real_glTexImage2D_raw, &p, sizeof p);
    }
    return sdv_real_glBindTexture && sdv_real_glTexImage2D_raw;
}

static void palette_stream_evict(RawTexEnt *e) {
    static const unsigned char blank[4] = {0, 0, 0, 0};
    if (!e || !e->stream_group || e->stream_evicted ||
        !palette_stream_resolve_gl()) return;
    sdv_real_glBindTexture(0x0DE1 /* TEXTURE_2D */, e->id);
    sdv_real_glTexImage2D_raw(0x0DE1, 0, 0x1908 /* RGBA */, 1, 1, 0,
                              0x1908, 0x1401 /* UBYTE */, blank);
    e->gpu_w = e->gpu_h = 1;
    e->stream_full = 0;
    e->stream_evicted = 1;
}

static void palette_stream_prune_players(unsigned keep) {
    if (!sb_palette_stream_enabled()) return;
    unsigned saved = raw_bound_tex2d();
    for (int i = 0; i < 1024; i++) {
        RawTexEnt *e = &raw_tex_tab[i];
        if (e->id && e->id != keep && e->stream_group == 1 &&
            !e->stream_full)
            palette_stream_evict(e);
    }
    if (sdv_real_glBindTexture) sdv_real_glBindTexture(0x0DE1, saved);
}

static void palette_stream_prune_preload(unsigned current) {
    if (!palette_players_pruned && palette_player_registered >= 11) {
        palette_players_pruned = 1;
        palette_stream_prune_players(0);
        fprintf(stderr,
                "[palette-stream] preload: %u jogadores referenciados e descarregados\n",
                palette_player_registered);
        if (sdv_real_glBindTexture)
            sdv_real_glBindTexture(0x0DE1, current);
    }
}

static unsigned char *palette_stream_read_xnb(const RawTexEnt *e) {
    if (!e || !e->source_path || !e->w || !e->h) return NULL;
    size_t bytes = (size_t)e->w * (size_t)e->h * 4;
    FILE *file = fopen(e->source_path, "rb");
    if (!file) return NULL;
    unsigned char magic[4];
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, "XNB", 3) != 0 ||
        fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    long offset = length >= 0 && (size_t)length >= bytes
        ? length - (long)bytes : -1;
    /* XNB Texture2D sem compressao: cabecalho curto + payload do unico mip. */
    if (offset < 48 || offset > 4096 || fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char *pixels = malloc(bytes);
    size_t got = pixels ? fread(pixels, 1, bytes, file) : 0;
    fclose(file);
    if (got != bytes) {
        free(pixels);
        return NULL;
    }
    return pixels;
}

static int palette_stream_page_in(RawTexEnt *e) {
    if (!e || !e->stream_group || e->stream_full ||
        !palette_stream_resolve_gl()) return e && e->stream_full;
    unsigned char *pixels = palette_stream_read_xnb(e);
    if (!pixels) {
        fprintf(stderr, "[palette-stream] FALHA lendo tex %u %s\n",
                e->id, e->source_path ? e->source_path : "?");
        return 0;
    }
    sdv_real_glTexImage2D_raw(0x0DE1, 0, 0x1908 /* RGBA */, e->w, e->h,
                              0, 0x1908, 0x1401 /* UBYTE */, pixels);
    free(pixels);
    e->gpu_w = e->w;
    e->gpu_h = e->h;
    e->stream_full = 1;
    e->stream_evicted = 0;
    e->use_clock = ++palette_stream_clock;
    fprintf(stderr, "[palette-stream] tex %u FULL %ux%u grupo=%u\n",
            e->id, e->w, e->h, e->stream_group);
    return 1;
}

static size_t palette_stream_group_bytes(int group) {
    size_t total = 0;
    for (int i = 0; i < 1024; i++) {
        RawTexEnt *e = &raw_tex_tab[i];
        if (e->id && e->stream_group == group && !e->stream_evicted)
            total += (size_t)e->gpu_w * e->gpu_h * 4;
    }
    return total;
}

static void palette_stream_enforce_cap(int group, unsigned keep) {
    /* Cada atlas grande de jogador ocupa 32 MiB em RGBA8. Quatro jogadores
     * simultaneos precisam de 128 MiB para nao alternar page-in/evict a cada
     * draw. No solo isto nao reserva memoria: somente o atlas realmente
     * bindado entra no grupo. Inimigos/chefes conservam o teto medido de 80. */
    size_t cap = (size_t)(group == 1 ? 128 : 80) * 1024 * 1024;
    unsigned saved = raw_bound_tex2d();
    while (palette_stream_group_bytes(group) > cap) {
        RawTexEnt *oldest = NULL;
        for (int i = 0; i < 1024; i++) {
            RawTexEnt *e = &raw_tex_tab[i];
            if (!e->id || e->id == keep || e->stream_group != group ||
                e->stream_evicted) continue;
            if (!oldest || e->use_clock < oldest->use_clock) oldest = e;
        }
        if (!oldest) break;
        palette_stream_evict(oldest);
    }
    if (sdv_real_glBindTexture) sdv_real_glBindTexture(0x0DE1, saved);
}

static void sdv_glBindTexture_palette(unsigned int target,
                                      unsigned int texture) {
    if (sdv_real_glBindTexture) sdv_real_glBindTexture(target, texture);
    if (target != 0x0DE1 || !texture || !sb_palette_stream_enabled()) return;
    RawTexEnt *e = raw_tex_find(texture, 0);
    if (!e || !e->stream_group) return;
    e->use_clock = ++palette_stream_clock;
    if (!e->stream_full && palette_stream_page_in(e)) {
        if (e->stream_group == 1) palette_stream_prune_players(e->id);
        palette_stream_enforce_cap(e->stream_group, e->id);
    }
}

static void sdv_glTexImage2D_raw(unsigned int target, int level, int ifmt,
        int w, int h, int border, unsigned int fmt, unsigned int type,
        const void *pix) {
    int m = sb_rawscale_min();
    unsigned int bound_id = 0;
    RawTexEnt *meta = NULL;
    if (target == 0x0DE1 && level == 0 && w > 0 && h > 0) {
        bound_id = raw_bound_tex2d();
        meta = raw_tex_find(bound_id, 1);
        if (meta) {
            int same_image = meta->id == bound_id && meta->w == w &&
                             meta->h == h;
            unsigned char palette_exact =
                same_image ? meta->palette_exact : 0;
            if (!same_image && jni_take_pending_paletted_texture())
                palette_exact = 1;
            meta->id = bound_id;
            meta->w = (unsigned short)w;
            meta->h = (unsigned short)h;
            meta->gpu_w = (unsigned short)w;
            meta->gpu_h = (unsigned short)h;
            meta->scaled = 0;
            meta->qualifies = 0;
            meta->palette_exact = palette_exact;
            if (pix && fmt == 0x1908 && type == 0x1401 &&
                sb_rgba8_looks_indexed((const unsigned char *)pix,
                                         (size_t)w * (size_t)h))
                meta->palette_exact = 1;
            if (pix && meta->palette_exact)
                palette_stream_attach(meta);
            if (pix && w >= sb_rawscale_min() &&
                h >= sb_rawscale_min()) {
                static unsigned traces;
                if (++traces <= 8) {
                    const unsigned char *p = (const unsigned char *)pix;
                    fprintf(stderr,
                            "[raw-detect] tex %u exact=%u bytes=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                            bound_id, meta->palette_exact,
                            p[0], p[1], p[2], p[3],
                            p[4], p[5], p[6], p[7]);
                }
            }
        }
    }
    if (m > 0 && target == 0x0DE1 && level == 0 && fmt == 0x1908 &&
        type == 0x1401 && w >= m && h >= m) {
        unsigned id = bound_id ? bound_id : raw_bound_tex2d();
        RawTexEnt *e = meta ? meta : raw_tex_find(id, 1);
        if (e) {
            /* Atlas NPOT de inimigo (Foot=2041x2033) chega linearizado pelo
             * Texture2DReader, mas o payload cru do XNB nao produz o mesmo
             * layout ao ser relido: cada sprite vira pedacos embaralhados.
             * O upload corrente ja contem os pixels corretos. Preserve-o em
             * full agora; o ContentManager libera o atlas ao trocar de fase.
             * Jogadores POT continuam no pager seletivo de disco. */
            if (pix && e->palette_exact && e->stream_group == 2)
                goto raw_full_size;
            /* Escala JA NA ALOCACAO (mesmo data==NULL): redefinir o storage
             * depois, no SubImage, deixava o frame builder do blob com write
             * lock orfao (glClear travava para sempre). O filtro acima exclui
             * os RTs widescreen 1920x1080 observados no boot. */
            int dw = e->palette_exact ? (w * 2) / 3 : w / 2;
            int dh = e->palette_exact ? (h * 2) / 3 : h / 2;
            unsigned char *half = NULL;
            uint16_t *half16 = NULL;
            if (pix) {
                half = malloc((size_t)dw * (size_t)dh * 4);
                if (!half)
                    goto raw_full_size;
                if (e->palette_exact)
                    sb_rgba8_resample_indexed((const unsigned char *)pix,
                                                w, h, half, dw, dh);
                else
                    sb_rgba8_halve((const unsigned char *)pix, w, h,
                                     half, dw, dh);
                if (!e->palette_exact && sb_tex16_enabled()) {
                    size_t px = (size_t)dw * (size_t)dh;
                    half16 = malloc(px * 2);
                    if (!half16) {
                        free(half);
                        goto raw_full_size;
                    }
                    sb_rgba8_to_4444(half, half16, px);
                }
            }
            e->id = id;
            e->w = (unsigned short)w;
            e->h = (unsigned short)h;
            e->gpu_w = (unsigned short)dw;
            e->gpu_h = (unsigned short)dh;
            e->qualifies = 1;
            e->scaled = 1;
            e->stream_full = 0;
            e->stream_evicted = 0;
            int use_4444 = !e->palette_exact && sb_tex16_enabled();
            int old_unpack = pix && use_4444
                ? sb_unpack_4444_begin() : -1;
            sdv_real_glTexImage2D_raw(
                target, 0, ifmt, dw, dh, border, fmt,
                use_4444 ? 0x8033 /* USHORT_4_4_4_4 */ : type,
                pix ? (half16 ? (const void *)half16 : (const void *)half)
                    : NULL);
            sb_unpack_4444_end(old_unpack);
            free(half16);
            free(half);
            if (pix && e->stream_group == 1)
                palette_stream_prune_preload(id);
            static unsigned n;
            if (++n <= 8 || n % 50 == 0 || e->palette_exact)
                fprintf(stderr,
                        "[rawscale] #%u tex %u %dx%d -> %dx%d%s%s\n",
                        n, id, w, h, dw, dh, pix ? " (data)" : " (alloc)",
                        e->palette_exact ? " palette8" :
                        (sb_tex16_enabled() ? " 4444" : ""));
            return;
        }
    }
raw_full_size:
    sdv_real_glTexImage2D_raw(target, level, ifmt, w, h, border, fmt, type,
                              pix);
    if (pix && meta && meta->stream_group) {
        meta->gpu_w = (unsigned short)w;
        meta->gpu_h = (unsigned short)h;
        meta->stream_full = 1;
        meta->stream_evicted = 0;
        if (meta->stream_group == 1)
            palette_stream_prune_preload(meta->id);
    }
}

/* Caminho de rastreamento puro: conserva exatamente o upload original e
 * guarda apenas a geometria necessaria para eleger o RenderTarget final.
 * Nao faz downscale, conversao, leitura de pixels nem envolve o hot path de
 * bind/draw. */
static void sdv_glTexImage2D_track(unsigned int target, int level, int ifmt,
        int w, int h, int border, unsigned int fmt, unsigned int type,
        const void *pix) {
    if (target == 0x0DE1 && level == 0 && w > 0 && h > 0 &&
        w <= 65535 && h <= 65535) {
        unsigned int id = raw_bound_tex2d();
        RawTexEnt *e = raw_tex_find(id, 1);
        if (e) {
            e->id = id;
            e->w = e->gpu_w = (unsigned short)w;
            e->h = e->gpu_h = (unsigned short)h;
            e->scaled = 0;
            e->qualifies = 0;
        }
    }
    sdv_real_glTexImage2D_raw(target, level, ifmt, w, h, border, fmt, type,
                              pix);
}
static void sdv_glTexSubImage2D_raw(unsigned int target, int level, int x,
        int y, int w, int h, unsigned int fmt, unsigned int type,
        const void *pix) {
    if (target == 0x0DE1 && pix && fmt == 0x1908 && type == 0x1401) {
        unsigned id = raw_bound_tex2d();
        RawTexEnt *e = raw_tex_find(id, 0);
        if (e && e->qualifies && e->scaled) {
                int dx = (int)((long long)x * e->gpu_w / e->w);
                int dy = (int)((long long)y * e->gpu_h / e->h);
                int dw = (int)((long long)w * e->gpu_w / e->w);
                int dh = (int)((long long)h * e->gpu_h / e->h);
                if (dw < 1) dw = 1;
                if (dh < 1) dh = 1;
                unsigned char *half = malloc((size_t)dw * (size_t)dh * 4);
                if (half) {
                    if (e->palette_exact)
                        sb_rgba8_resample_indexed(
                            (const unsigned char *)pix, w, h,
                            half, dw, dh);
                    else
                        sb_rgba8_halve((const unsigned char *)pix, w, h,
                                         half, dw, dh);
                    if (!e->palette_exact && sb_tex16_enabled()) {
                        size_t px = (size_t)dw * (size_t)dh;
                        uint16_t *half16 = malloc(px * 2);
                        if (!half16) {
                            free(half);
                            goto raw_subimage_full_size;
                        }
                        sb_rgba8_to_4444(half, half16, px);
                        int old_unpack = sb_unpack_4444_begin();
                        sdv_real_glTexSubImage2D(
                            target, level, dx, dy, dw, dh, fmt,
                            0x8033 /* USHORT_4_4_4_4 */, half16);
                        sb_unpack_4444_end(old_unpack);
                        free(half16);
                    } else {
                        sdv_real_glTexSubImage2D(target, level, dx, dy, dw, dh,
                                                 fmt, type, half);
                    }
                    free(half);
                    static unsigned pn;
                    if (++pn <= 8)
                        fprintf(stderr,
                                "[rawscale] subimage parcial %d,%d %dx%d em tex %u escalada\n",
                                x, y, w, h, id);
                    return;
                }
        }
    }
raw_subimage_full_size:
    sdv_real_glTexSubImage2D(target, level, x, y, w, h, fmt, type, pix);
}
static void sdv_glDeleteTextures_raw(int n, const unsigned int *ids) {
    for (int i = 0; ids && i < n; i++) {
        RawTexEnt *e = raw_tex_find(ids[i], 0);
        if (e) {
            if (e->stream_registered && e->stream_group == 1 &&
                palette_player_registered)
                palette_player_registered--;
            free(e->source_path);
            if (ids[i] < RAW_TEX_DIRECT_MAX && raw_tex_direct[ids[i]] == e)
                raw_tex_direct[ids[i]] = NULL;
            memset(e, 0, sizeof *e);
        }
    }
    if (sdv_real_glDeleteTextures) sdv_real_glDeleteTextures(n, ids);
}

static void sdv_glCompressedTexImage2D_astc(unsigned int target, int level,
        unsigned int internalformat, int width, int height, int border,
        int imageSize, const void *data) {
    static unsigned decoded, failed;
    int bx, by;
    unsigned int bound_id = target == 0x0DE1 ? raw_bound_tex2d() : 0;
    char asset_path[4096];
    int have_asset_path = level == 0 &&
        jni_copy_last_texture_asset_path(asset_path, sizeof asset_path);
    int stage_bg_quality = have_asset_path &&
        sb_keep_stage_bg_texture_full(asset_path, width, height);
    int boss_quality = have_asset_path && palette_players_pruned &&
        sb_keep_main_boss_texture_full(asset_path);
    int font_pending = level == 0 ? jni_take_pending_font_texture() : 0;
    RawTexEnt *bound_meta = raw_tex_find(bound_id, font_pending && bound_id);
    int font_quality = bound_meta ? bound_meta->font_quality : 0;
    if (font_pending) {
        font_quality = 1;
        if (bound_meta) {
            bound_meta->id = bound_id;
            bound_meta->font_quality = 1;
        }
    }
    if (astc_block_dims(internalformat, &bx, &by) == 0 && data &&
        width > 0 && height > 0) {
        /* Fork do jogo pode mapear o enum errado (quirk SOR4: fmt98 6x6 como
         * 5x5) — valida o payload e infere o bloco real pelo tamanho. */
        if (astc_expected_size(width, height, bx, by) != imageSize) {
            static const unsigned char dims[14][2] = {
                {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
                {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
            };
            int found = 0;
            for (int i = 0; i < 14; i++)
                if (astc_expected_size(width, height, dims[i][0], dims[i][1])
                        == imageSize) {
                    bx = dims[i][0]; by = dims[i][1]; found = 1; break;
                }
            fprintf(stderr,
                    "[astc] payload %d != esperado p/ fmt=0x%x %dx%d; %s %dx%d\n",
                    imageSize, internalformat, width, height,
                    found ? "inferido bloco" : "mantendo bloco", bx, by);
        }
        /* cache v2: pixels finais prontos -> upload direto, sem decode/halve */
        const int orig_w = width, orig_h = height;
        unsigned int tc2_flags = 0;
        {
            int m = sb_texscale_min();
            if (m > 0) tc2_flags |= 1u | ((unsigned)m << 8);
            if (sb_tex16_enabled()) tc2_flags |= 2u;
            if (sb_etc1_enabled()) tc2_flags |= 4u;
            if (sb_keep_menu_texture_full(orig_w, orig_h)) tc2_flags |= 8u;
            if (font_quality) tc2_flags |= 32u; /* fonte full-res 4444 */
            if (stage_bg_quality) tc2_flags |= 64u;
            if (boss_quality) tc2_flags |= 128u;
        }
        {
            int dw = 0, dh = 0;
            unsigned int gltype = 0;
            void *pix = nextos_tc2_load(data, imageSize, orig_w, orig_h,
                                        tc2_flags, &dw, &dh, &gltype);
            if (pix) {
                if (!sdv_real_glTexImage2D) {
                    void *p = sdv_egl_get_proc_address("glTexImage2D");
                    memcpy(&sdv_real_glTexImage2D, &p, sizeof p);
                }
                if (gltype == 0x8D64 /* GL_ETC1_RGB8_OES */ &&
                    sdv_real_glCompressedTexImage2D) {
                    int bytes = (int)ss_etc1_size(dw, dh);
                    sdv_real_glCompressedTexImage2D(target, level, gltype,
                                                     dw, dh, 0, bytes, pix);
                    free(pix);
                    ++decoded;
                    if (decoded <= 8 || decoded % 100 == 0)
                        fprintf(stderr, "[astc] tc2 ETC1 hit #%u %dx%d\n",
                                decoded, dw, dh);
                    return;
                }
                if (sdv_real_glTexImage2D && gltype != 0x8D64) {
                    int old_unpack = gltype == 0x8033
                        ? sb_unpack_4444_begin() : -1;
                    sdv_real_glTexImage2D(target, level, 0x1908, dw, dh, 0,
                                          0x1908, gltype, pix);
                    sb_unpack_4444_end(old_unpack);
                    free(pix);
                    ++decoded;
                    if (decoded <= 8 || decoded % 100 == 0)
                        fprintf(stderr, "[astc] tc2 hit #%u %dx%d\n",
                                decoded, dw, dh);
                    return;
                }
                free(pix);
            }
        }
        size_t rgba_size = (size_t)width * (size_t)height * 4;
        unsigned char *rgba = malloc(rgba_size);
        if (rgba && astc_decode_shim(rgba, (int)rgba_size, data, imageSize,
                                     width, height, bx, by) == 0) {
            int ts_min = sb_texscale_min();
            if (font_quality) {
                static unsigned full_fonts;
                if (++full_fonts <= 20)
                    fprintf(stderr, "[astc] fonte FULL %dx%d\n", width, height);
            } else if (stage_bg_quality) {
                static unsigned full_stage_bg;
                if (++full_stage_bg <= 32)
                    fprintf(stderr, "[astc] cenario FULL %dx%d %s\n",
                            width, height, asset_path);
            } else if (boss_quality) {
                static unsigned full_bosses;
                if (++full_bosses <= 16)
                    fprintf(stderr, "[astc] boss FULL %dx%d %s\n",
                            width, height, asset_path);
            } else if (ts_min > 0 &&
                !sb_keep_menu_texture_full(width, height) &&
                (long)width * height >= (long)ts_min * ts_min &&
                width >= 2 && height >= 2) {
                int dw = width / 2, dh = height / 2;
                unsigned char *half = malloc((size_t)dw * (size_t)dh * 4);
                if (half) {
                    sb_rgba8_halve(rgba, width, height, half, dw, dh);
                    free(rgba);
                    rgba = half;
                    width = dw;
                    height = dh;
                    static unsigned scaled;
                    if (++scaled <= 8 || scaled % 100 == 0)
                        fprintf(stderr, "[astc] texscale #%u -> %dx%d\n",
                                scaled, dw, dh);
                }
            }
            if (!sdv_real_glTexImage2D) {
                void *p = sdv_egl_get_proc_address("glTexImage2D");
                memcpy(&sdv_real_glTexImage2D, &p, sizeof p);
            }
            if (sdv_real_glTexImage2D) {
                size_t px = (size_t)width * (size_t)height;
                if (sb_etc1_enabled() &&
                    sb_rgba_is_opaque(rgba, px) &&
                    sdv_real_glCompressedTexImage2D) {
                    size_t etc1_size = ss_etc1_size(width, height);
                    unsigned char *etc1 = malloc(etc1_size);
                    if (etc1) {
                        ss_etc1_encode_rgba(rgba, width, height,
                                            (size_t)width * 4, etc1);
                        sdv_real_glCompressedTexImage2D(
                            target, level, 0x8D64 /* GL_ETC1_RGB8_OES */,
                            width, height, 0, (int)etc1_size, etc1);
                        nextos_tc2_store(data, imageSize, orig_w, orig_h,
                                         tc2_flags, width, height, 0x8D64,
                                         etc1, (int)etc1_size);
                        free(etc1);
                        free(rgba);
                        ++decoded;
                        if (decoded <= 8 || decoded % 100 == 0)
                            fprintf(stderr,
                                    "[astc] decode #%u %dx%d blk %dx%d lvl %d OK (ETC1)\n",
                                    decoded, width, height, bx, by, level);
                        return;
                    }
                }
                if (sb_tex16_enabled()) {
                    uint16_t *p16 = malloc(px * 2);
                    if (p16) {
                        sb_rgba8_to_4444(rgba, p16, px);
                        int old_unpack = sb_unpack_4444_begin();
                        sdv_real_glTexImage2D(target, level, 0x1908, width,
                                              height, 0, 0x1908,
                                              0x8033 /* USHORT_4_4_4_4 */, p16);
                        sb_unpack_4444_end(old_unpack);
                        nextos_tc2_store(data, imageSize, orig_w, orig_h,
                                         tc2_flags, width, height, 0x8033,
                                         p16, (int)(px * 2));
                        free(p16);
                        free(rgba);
                        ++decoded;
                        if (decoded <= 8 || decoded % 100 == 0)
                            fprintf(stderr,
                                    "[astc] decode #%u %dx%d blk %dx%d lvl %d OK (4444)\n",
                                    decoded, width, height, bx, by, level);
                        return;
                    }
                }
                sdv_real_glTexImage2D(target, level, 0x1908 /* GL_RGBA */,
                                      width, height, 0, 0x1908,
                                      0x1401 /* GL_UNSIGNED_BYTE */, rgba);
                nextos_tc2_store(data, imageSize, orig_w, orig_h, tc2_flags,
                                 width, height, 0x1401, rgba,
                                 (int)((size_t)width * height * 4));
                free(rgba);
                ++decoded;
                if (decoded <= 8 || decoded % 100 == 0)
                    fprintf(stderr,
                            "[astc] decode #%u %dx%d blk %dx%d lvl %d OK\n",
                            decoded, width, height, bx, by, level);
                return;
            }
        }
        free(rgba);
        if (++failed <= 8)
            fprintf(stderr, "[astc] decode FALHOU fmt=0x%x %dx%d blk %dx%d sz=%d\n",
                    internalformat, width, height, bx, by, imageSize);
        /* cai no real: o driver vai gerar GL_INVALID_ENUM, melhor que abortar */
    }
    if (sdv_real_glCompressedTexImage2D)
        sdv_real_glCompressedTexImage2D(target, level, internalformat, width,
                                        height, border, imageSize, data);
}

/* Partial update de textura comprimida ASTC nao suportado no caminho de
 * decode; loga para ficar visivel se algum conteudo usar. */
static void (*sdv_real_glCompressedTexSubImage2D)(unsigned int, int, int, int,
                                                  int, int, unsigned int, int,
                                                  const void *);
static void sdv_glCompressedTexSubImage2D_astc(unsigned int target, int level,
        int xoff, int yoff, int width, int height, unsigned int format,
        int imageSize, const void *data) {
    int bx, by;
    static unsigned warned;
    if (astc_block_dims(format, &bx, &by) == 0 && warned++ < 8)
        fprintf(stderr, "[astc] AVISO: CompressedTexSubImage2D ASTC nao tratado "
                        "(fmt=0x%x %dx%d @%d,%d)\n", format, width, height, xoff, yoff);
    if (sdv_real_glCompressedTexSubImage2D)
        sdv_real_glCompressedTexSubImage2D(target, level, xoff, yoff, width,
                                           height, format, imageSize, data);
}

void *sdv_dlsym(void *handle, const char *name) {
    if (handle == SB_EOS_HANDLE) {
        static unsigned logged;
        if (!name) return NULL;
        if (logged++ < 24)
            fprintf(stderr, "[eos-stub] dlsym %s\n", name);
        if (strcmp(name, "EOS_GetVersion") == 0)
            return (void *)&eos_stub_version;
        return (void *)&eos_stub_fail;
    }
    if (handle == SB_AAUDIO_HANDLE) return sb_aaudio_dlsym(name);
    if (handle == SB_LIBLOG_HANDLE) {
        if (!name) return NULL;
        if (strcmp(name, "__android_log_write") == 0)  return &__android_log_write;
        if (strcmp(name, "__android_log_print") == 0)  return &__android_log_print;
        if (strcmp(name, "__android_log_vprint") == 0) return &__android_log_vprint;
        if (strcmp(name, "__android_log_assert") == 0) return &__android_log_assert;
        return NULL;
    }
    if (handle == SB_LIBDL_HANDLE) {
        if (!name) return NULL;
        if (strcmp(name, "dlopen") == 0)  return &sdv_dlopen;
        if (strcmp(name, "dlsym") == 0)   return &sdv_dlsym;
        if (strcmp(name, "dlclose") == 0) return &sdv_dlclose;
        if (strcmp(name, "dlerror") == 0) return &dlerror;
        return NULL;
    }
    /* Nunca entregue um handle do loader custom ao ld.so da glibc: se o
     * simbolo estiver ausente, dlsym(handle_fake, ...) pode dereferenciar a
     * nossa struct como link_map e abortar. */
    void *p = sdv_so_is_handle(handle)
        ? sdv_so_dlsym(handle, name)
        : dlsym(handle, name);
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
        if (!p) {
            void *compat_p = bt_fbo_compat_resolve(
                name, NULL, sdv_fbo_core_resolve, NULL);
            if (compat_p) {
                fprintf(stderr, "[bt/fbo] alias %s -> %s\n", name,
                        bt_fbo_compat_core_name(name));
                p = compat_p;
            }
        }
    }
    /* O wrapper de glGetString sempre aplica a fachada FBO, mas só acrescenta
     * ASTC quando SB_ASTC é opt-in. As duas capacidades permanecem separadas. */
    if (name && p && strcmp(name, "glGetString") == 0) {
        memcpy(&sdv_real_glGetString, &p, sizeof p);
        return (void *)&sdv_glGetString_compat;
    }
    /* Hooks ASTC de upload (load-time only; ver bloco astc acima). */
    if (name && p && getenv("SB_ASTC")) {
        if (strcmp(name, "glCompressedTexImage2D") == 0) {
            memcpy(&sdv_real_glCompressedTexImage2D, &p, sizeof p);
            return (void *)&sdv_glCompressedTexImage2D_astc;
        }
        if (strcmp(name, "glCompressedTexSubImage2D") == 0) {
            memcpy(&sdv_real_glCompressedTexSubImage2D, &p, sizeof p);
            return (void *)&sdv_glCompressedTexSubImage2D_astc;
        }
    }
    /* Hooks RAW RGBA (load-time only; sem wrapper em bind/draw). O registro
     * geometrico do present permanece ativo mesmo quando o downscale esta
     * desligado: as duas responsabilidades nao podem voltar a se acoplar. */
    if (name && p && (sb_rawscale_min() > 0 ||
                      sb_fbo_tracking_enabled())) {
        if (strcmp(name, "glTexImage2D") == 0) {
            memcpy(&sdv_real_glTexImage2D_raw, &p, sizeof p);
            return sb_rawscale_min() > 0
                ? (void *)&sdv_glTexImage2D_raw
                : (void *)&sdv_glTexImage2D_track;
        }
        if (sb_rawscale_min() > 0 &&
            strcmp(name, "glTexSubImage2D") == 0) {
            memcpy(&sdv_real_glTexSubImage2D, &p, sizeof p);
            return (void *)&sdv_glTexSubImage2D_raw;
        }
        if (sb_rawscale_min() > 0 &&
            strcmp(name, "glDeleteTextures") == 0) {
            memcpy(&sdv_real_glDeleteTextures, &p, sizeof p);
            return (void *)&sdv_glDeleteTextures_raw;
        }
    }
    if (name && p && sb_palette_stream_enabled() &&
        strcmp(name, "glBindTexture") == 0) {
        memcpy(&sdv_real_glBindTexture, &p, sizeof sdv_real_glBindTexture);
        return (void *)&sdv_glBindTexture_palette;
    }
    /* Diagnostico opt-in e de baixa frequencia: registra somente os nomes de
     * FBO realmente gerados pelo jogo. Evita envolver glBind/draw no hot path
     * e permite ao bridge inspecionar os dois render targets do menu sem
     * tentar IDs arbitrarios no driver Mali. */
    if (name && p && sb_fbo_tracking_enabled()) {
        if (strcmp(name, "glGenFramebuffers") == 0 ||
            strcmp(name, "glGenFramebuffersEXT") == 0 ||
            strcmp(name, "glGenFramebuffersOES") == 0) {
            memcpy(&sdv_real_glGenFramebuffers, &p,
                   sizeof sdv_real_glGenFramebuffers);
            return (void *)&sdv_glGenFramebuffers_trace;
        }
        if (strcmp(name, "glDeleteFramebuffers") == 0 ||
            strcmp(name, "glDeleteFramebuffersEXT") == 0 ||
            strcmp(name, "glDeleteFramebuffersOES") == 0) {
            memcpy(&sdv_real_glDeleteFramebuffers, &p,
                   sizeof sdv_real_glDeleteFramebuffers);
            return (void *)&sdv_glDeleteFramebuffers_track;
        }
    }
    /* Os wrappers temporarios foram úteis no diagnóstico, mas a indireção
     * corrompe o driver Mali após alguns milhares de draws. Mantemos o código
     * como referência de investigação, sem qualquer caminho de ativação. */
    if (0 && p && name) {
#define SB_GL_TRACE_SYMBOL(symbol, storage, wrapper) do {                   \
        if (strcmp(name, symbol) == 0) {                                     \
            memcpy(&(storage), &p, sizeof(storage));                         \
            return &(wrapper);                                               \
        }                                                                    \
    } while (0)
        SB_GL_TRACE_SYMBOL("glViewport", sdv_real_glViewport,
                            sdv_glViewport_trace);
        SB_GL_TRACE_SYMBOL("glClearColor", sdv_real_glClearColor,
                            sdv_glClearColor_trace);
        SB_GL_TRACE_SYMBOL("glClear", sdv_real_glClear, sdv_glClear_trace);
        SB_GL_TRACE_SYMBOL("glUseProgram", sdv_real_glUseProgram,
                            sdv_glUseProgram_trace);
        SB_GL_TRACE_SYMBOL("glColorMask", sdv_real_glColorMask,
                            sdv_glColorMask_trace);
        SB_GL_TRACE_SYMBOL("glScissor", sdv_real_glScissor,
                            sdv_glScissor_trace);
        SB_GL_TRACE_SYMBOL("glEnable", sdv_real_glEnable,
                            sdv_glEnable_trace);
        SB_GL_TRACE_SYMBOL("glDisable", sdv_real_glDisable,
                            sdv_glDisable_trace);
        SB_GL_TRACE_SYMBOL("glBindFramebuffer", sdv_real_glBindFramebuffer,
                            sdv_glBindFramebuffer_trace);
        SB_GL_TRACE_SYMBOL("glBindFramebufferEXT", sdv_real_glBindFramebuffer,
                            sdv_glBindFramebuffer_trace);
        SB_GL_TRACE_SYMBOL("glBindFramebufferOES", sdv_real_glBindFramebuffer,
                            sdv_glBindFramebuffer_trace);
        SB_GL_TRACE_SYMBOL("glGenFramebuffers", sdv_real_glGenFramebuffers,
                            sdv_glGenFramebuffers_trace);
        SB_GL_TRACE_SYMBOL("glGenFramebuffersEXT", sdv_real_glGenFramebuffers,
                            sdv_glGenFramebuffers_trace);
        SB_GL_TRACE_SYMBOL("glGenFramebuffersOES", sdv_real_glGenFramebuffers,
                            sdv_glGenFramebuffers_trace);
        SB_GL_TRACE_SYMBOL("glFramebufferTexture2D",
                            sdv_real_glFramebufferTexture2D,
                            sdv_glFramebufferTexture2D_trace);
        SB_GL_TRACE_SYMBOL("glFramebufferTexture2DEXT",
                            sdv_real_glFramebufferTexture2D,
                            sdv_glFramebufferTexture2D_trace);
        SB_GL_TRACE_SYMBOL("glFramebufferTexture2DOES",
                            sdv_real_glFramebufferTexture2D,
                            sdv_glFramebufferTexture2D_trace);
        SB_GL_TRACE_SYMBOL("glFramebufferRenderbuffer",
                            sdv_real_glFramebufferRenderbuffer,
                            sdv_glFramebufferRenderbuffer_trace);
        SB_GL_TRACE_SYMBOL("glFramebufferRenderbufferEXT",
                            sdv_real_glFramebufferRenderbuffer,
                            sdv_glFramebufferRenderbuffer_trace);
        SB_GL_TRACE_SYMBOL("glCheckFramebufferStatus",
                            sdv_real_glCheckFramebufferStatus,
                            sdv_glCheckFramebufferStatus_trace);
        SB_GL_TRACE_SYMBOL("glCheckFramebufferStatusEXT",
                            sdv_real_glCheckFramebufferStatus,
                            sdv_glCheckFramebufferStatus_trace);
        SB_GL_TRACE_SYMBOL("glCheckFramebufferStatusOES",
                            sdv_real_glCheckFramebufferStatus,
                            sdv_glCheckFramebufferStatus_trace);
        SB_GL_TRACE_SYMBOL("glBindRenderbufferEXT",
                            sdv_real_glBindRenderbuffer,
                            sdv_glBindRenderbuffer_trace);
        SB_GL_TRACE_SYMBOL("glRenderbufferStorageEXT",
                            sdv_real_glRenderbufferStorage,
                            sdv_glRenderbufferStorage_trace);
        SB_GL_TRACE_SYMBOL("glDrawArrays", sdv_real_glDrawArrays,
                            sdv_glDrawArrays_trace);
        SB_GL_TRACE_SYMBOL("glDrawElements", sdv_real_glDrawElements,
                            sdv_glDrawElements_trace);
        SB_GL_TRACE_SYMBOL("glVertexAttribPointer",
                            sdv_real_glVertexAttribPointer,
                            sdv_glVertexAttribPointer_trace);
        SB_GL_TRACE_SYMBOL("glBindTexture", sdv_real_glBindTexture,
                            sdv_glBindTexture_trace);
        if (strcmp(name, "glGetError") == 0)
            memcpy(&sdv_real_glGetError, &p, sizeof(sdv_real_glGetError));
#undef SB_GL_TRACE_SYMBOL
    }
    return p;
}
/* dlopen: igual ao android_dlopen_ext — glibc primeiro, fallback p/ so-loader
 * (monodroid_dlopen chama dlopen direto p/ alguns .so Bionic como libaot-*). */
void *sdv_dlopen(const char *filename, int flag) {
    if (is_virtual_eos(filename)) {
        fprintf(stderr, "[dlopen] '%s' -> EOS stub virtual\n", filename);
        return SB_EOS_HANDLE;
    }
    if (is_virtual_aaudio(filename)) {
        int ok = sb_aaudio_available();
        fprintf(stderr, "[dlopen] '%s' -> libaaudio virtual (SDL) %s\n", filename,
                ok ? "OK" : "INDISPONIVEL");
        return ok ? SB_AAUDIO_HANDLE : NULL;
    }
    if (is_virtual_liblog(filename)) {
        fprintf(stderr, "[dlopen] '%s' -> liblog virtual\n", filename);
        return SB_LIBLOG_HANDLE;
    }
    if (is_virtual_libdl(filename)) {
        fprintf(stderr, "[dlopen] '%s' -> libdl virtual\n", filename);
        return SB_LIBDL_HANDLE;
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
    void *h = dlopen(filename, flag | RTLD_GLOBAL);
    if (h) return h;
    h = nx_dlopen_host_versioned(filename, flag | RTLD_GLOBAL);
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

/* ---- ALooper (NDK) ----
 * O libmonodroid do .NET for Android 10 chama ALooper_forThread/acquire/
 * addFd/removeFd dentro do Runtime_initInternal (bridge de timezone e de
 * eventos da UI thread). O build do scourgebringer (.NET 7) nao usava nenhuma
 * delas, entao os imports ficavam nao resolvidos e o PLT saltava para lixo:
 * era esse o SIGSEGV logo depois de "debug.mono.log". Aqui nao ha Looper do
 * Android — devolvemos um token estavel e ignoramos o registro de fds; quem
 * bombeia os eventos e o proprio loop do loader. */
static int g_fake_alooper_token;

static void *ALooper_forThread(void) { return &g_fake_alooper_token; }
static void ALooper_acquire(void *looper) { (void)looper; }
static void ALooper_release(void *looper) { (void)looper; }
static void *ALooper_prepare(int opts) { (void)opts; return &g_fake_alooper_token; }
static int ALooper_addFd(void *looper, int fd, int ident, int events,
                         void *callback, void *data) {
    (void)looper; (void)fd; (void)ident; (void)events; (void)callback; (void)data;
    return 1;
}
static int ALooper_removeFd(void *looper, int fd) {
    (void)looper; (void)fd;
    return 1;
}
static void ALooper_wake(void *looper) { (void)looper; }

DynLibFunction dynlib_functions[] = {
    {"ALooper_forThread",     (uintptr_t)&ALooper_forThread},
    {"ALooper_prepare",       (uintptr_t)&ALooper_prepare},
    {"ALooper_acquire",       (uintptr_t)&ALooper_acquire},
    {"ALooper_release",       (uintptr_t)&ALooper_release},
    {"ALooper_addFd",         (uintptr_t)&ALooper_addFd},
    {"ALooper_removeFd",      (uintptr_t)&ALooper_removeFd},
    {"ALooper_wake",          (uintptr_t)&ALooper_wake},
    {"__errno",               (uintptr_t)&sdv_errno_loc},
    {"__android_log_write",   (uintptr_t)&__android_log_write},
    {"__android_log_print",   (uintptr_t)&__android_log_print},
    {"__android_log_vprint",  (uintptr_t)&__android_log_vprint},
    {"__android_log_assert",  (uintptr_t)&__android_log_assert},
    {"__umask_chk",           (uintptr_t)&__umask_chk},
    {"android_dlopen_ext",    (uintptr_t)&android_dlopen_ext},
    {"dlopen",                (uintptr_t)&sdv_dlopen},
    {"dlsym",                 (uintptr_t)&sdv_dlsym},
    /* Sem esta entrada o dlclose ia direto para o ld.so da glibc, que trata o
     * handle como link_map: o FMOD fecha o handle virtual de libaaudio.so no
     * teardown e isso derrubava o processo dentro do ld-linux. */
    {"dlclose",               (uintptr_t)&sdv_dlclose},
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
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
