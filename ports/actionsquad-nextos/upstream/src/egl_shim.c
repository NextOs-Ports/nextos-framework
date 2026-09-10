#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "egl_shim.h"
#include "util.h"
#include "nxgl_frame_proof_adapter.h"

extern int as_retry_sdl_provider(const char *stage);

/* Resolucao DINAMICA (qualquer device): desktop mode do SDL com fallback
 * 1280x720. Exportada p/ imports.c (ANativeWindow_getWidth/Height — o que o
 * JOGO le) e android_shim.c (clamp do cursor). */
int coi_screen_w = 1280, coi_screen_h = 720;
#define SCREEN_WIDTH coi_screen_w
#define SCREEN_HEIGHT coi_screen_h

/* A engine (bionic) lê a stack-canary de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD).
 * Sob glibc esse offset colide com uma TLS var que o Mali/SDL escreve no
 * MakeCurrent/CreateContext -> a canary "muda" no meio da função -> stack smash
 * FALSO-POSITIVO. Salvamos/restauramos tpidr+0x28 ao redor das chamadas SDL_GL
 * p/ a engine ver o guard ESTÁVEL. */
static int gl_makecurrent(SDL_Window *w, SDL_GLContext c) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  int (*f)(SDL_Window *, SDL_GLContext) = &SDL_GL_MakeCurrent;
  int r = f(w, c);
  *(unsigned long *)(tp + 0x28) = g;
  return r;
}
static SDL_GLContext gl_createcontext(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GLContext (*f)(SDL_Window *) = &SDL_GL_CreateContext;
  SDL_GLContext c = f(w);
  *(unsigned long *)(tp + 0x28) = g;
  return c;
}

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  int swapint_applied;
  int id;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;
static int frame_count = 0;
static int next_context_id = 1;

static _egl_context *current_context = NULL;
static _egl_context *last_context = NULL;
static int has_real_gl = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_create_window(void) {
  /* resolucao nativa do device (TV 1080p, handheld 480p...) c/ fallback 720p */
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    coi_screen_w = dm.w; coi_screen_h = dm.h;
    debugPrintf("egl_shim: desktop mode %dx%d\n", dm.w, dm.h);
  }
  { const char *e = getenv("COI_RES"); int w, h; /* override opcional */
    if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      coi_screen_w = w; coi_screen_h = h;
      debugPrintf("egl_shim: COI_RES override %dx%d\n", w, h);
    } }
  /* ===== ESCADA DE CONFIG GL (receita bully2 multi-CFW) =====
   * Uma so config quebrava em CFW com EGL exigente: mali fbdev antigo prefere
   * alpha8; PowerVR (TrimUI) so tem depth16; mesa/panfrost pode devolver
   * EGL_BAD_CONFIG ou um contexto DESKTOP-GL (lição sonic4: rejeitar!).
   * Tentamos combos ate um dar contexto ES valido. GLVER=3 tenta ES3 primeiro
   * e cai pra ES2; GLVER=2 tenta ES2 primeiro e sobe pra ES3 em ultimo caso. */
  { const char *gv = getenv("COI_GLVER");
    int want3 = (gv && gv[0] == '3');
    struct { int major, alpha, depth, stencil; } ladder[10]; int ln = 0;
    int majors[2]; majors[0] = want3 ? 3 : 2; majors[1] = want3 ? 2 : 3;
    for (int m = 0; m < 2; m++) {
      /* ALPHA 8 PRIMEIRO (licao Horizon v1.0.3 / Oceanhorn v1.0.2): pedindo
       * alpha=0 o Mesa/Panfrost devolve o primeiro match, que e' RGBX8888, e a
       * engine — que compoe UI e fade com alpha de framebuffer — desenha por
       * cima de lixo ou em preto. O config EXATO concedido e' logado abaixo,
       * porque "pedi RGBA8888" nao e' a mesma coisa que "recebi RGBA8888". */
      /* COI_EGL_ALPHA=0 inverte a ordem: override MANUAL, so' para diagnostico
       * de firmware novo (nunca um perfil por nome de aparelho). */
      int a_first = (getenv("COI_EGL_ALPHA") && getenv("COI_EGL_ALPHA")[0] == '0') ? 0 : 8;
      ladder[ln].major = majors[m]; ladder[ln].alpha = a_first;     ladder[ln].depth = 24; ladder[ln].stencil = 8; ln++;
      ladder[ln].major = majors[m]; ladder[ln].alpha = 8 - a_first; ladder[ln].depth = 24; ladder[ln].stencil = 8; ln++;
      ladder[ln].major = majors[m]; ladder[ln].alpha = 0; ladder[ln].depth = 16; ladder[ln].stencil = 8; ln++;
      ladder[ln].major = majors[m]; ladder[ln].alpha = 0; ladder[ln].depth = 16; ladder[ln].stencil = 0; ln++;
    }
    for (int i = 0; i < ln && !egl_share_root; i++) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, ladder[i].major);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, ladder[i].alpha);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, ladder[i].depth);
      SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, ladder[i].stencil);
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      if (egl_window) { SDL_DestroyWindow(egl_window); egl_window = NULL; }
      egl_window = SDL_CreateWindow(
          PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
          SCREEN_WIDTH, SCREEN_HEIGHT,
          SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
      if (!egl_window) {
        fprintf(stderr, "[egl_shim] rung %d (ES%d a%d d%d s%d): janela FALHOU: %s\n",
                i, ladder[i].major, ladder[i].alpha, ladder[i].depth, ladder[i].stencil, SDL_GetError());
        continue;
      }
      egl_share_root = gl_createcontext(egl_window);
      if (!egl_share_root) {
        fprintf(stderr, "[egl_shim] rung %d (ES%d a%d d%d s%d): contexto FALHOU: %s\n",
                i, ladder[i].major, ladder[i].alpha, ladder[i].depth, ladder[i].stencil, SDL_GetError());
        continue;
      }
      /* rejeita contexto DESKTOP-GL (mesa pode ignorar o profile ES) */
      gl_makecurrent(egl_window, egl_share_root);
      { const GLubyte *(*gs)(unsigned) = (void *)SDL_GL_GetProcAddress("glGetString");
        const char *ver = gs ? (const char *)gs(0x1F02) : NULL;
        if (!ver || strncmp(ver, "OpenGL ES", 9) != 0) {
          fprintf(stderr, "[egl_shim] rung %d: contexto NAO-ES ('%s') -> rejeitado\n",
                  i, ver ? ver : "null");
          SDL_GL_DeleteContext(egl_share_root); egl_share_root = NULL;
          continue;
        }
        fprintf(stderr, "[egl_shim] window %dx%d created (driver=%s, rung %d ES%d a%d d%d s%d)\n",
                SCREEN_WIDTH, SCREEN_HEIGHT, SDL_GetCurrentVideoDriver(),
                i, ladder[i].major, ladder[i].alpha, ladder[i].depth, ladder[i].stencil);
        fprintf(stderr, "[egl_shim] GL_VERSION='%s' RENDERER='%s' VENDOR='%s'\n",
                ver, gs ? (const char *)gs(0x1F01) : "?", gs ? (const char *)gs(0x1F00) : "?");
        /* CONFIG CONCEDIDO, nao o pedido: em Mesa/Panfrost o driver escolhe o
         * primeiro visual compativel e pode entregar 0 bits de alpha mesmo com
         * o pedido em 8. Sem esta linha no log, um relato de "tela lavada" ou
         * "UI transparente" em firmware que nao temos e' indistinguivel de bug
         * de engine — com ela, o diagnostico e' de uma olhada. */
        int gr = -1, gg = -1, gb = -1, ga = -1, gd = -1, gsz = -1, gdb = -1;
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &gr);
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &gg);
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &gb);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &ga);
        SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &gd);
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &gsz);
        SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &gdb);
        fprintf(stderr,
                "[egl_shim] EGLConfig CONCEDIDO: R%d G%d B%d A%d depth=%d "
                "stencil=%d double=%d (pedido A%d d%d s%d) -> %s\n",
                gr, gg, gb, ga, gd, gsz, gdb, ladder[i].alpha, ladder[i].depth,
                ladder[i].stencil,
                (gr >= 8 && gg >= 8 && gb >= 8 && ga >= 8) ? "RGBA8888"
                : (gr >= 8 && gg >= 8 && gb >= 8)          ? "RGBX8888 (sem alpha)"
                                                           : "config reduzido");
      }
    }
  }
  if (!egl_window || !egl_share_root) {
    fprintf(stderr, "[egl_shim] TODAS as configs GL falharam: %s\n", SDL_GetError());
    as_retry_sdl_provider("window-context");
    return;
  }
  fprintf(stderr, "[egl_shim] GL share-root context OK\n");

  /* O DRAWABLE REAL É A AUTORIDADE (regra multi-device). O tamanho pedido no
   * SDL_CreateWindow é só um pedido: em KMSDRM o driver escolhe um modo do
   * conector, em fbdev o pan pode ser metade do virtual_size, e em HiDPI o
   * drawable é maior que a janela. O jogo lê este par por
   * ANativeWindow_getWidth/Height — se ele divergir do buffer de verdade, o
   * viewport e o mapeamento de toque saem errados no device inteiro. */
  {
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(egl_window, &dw, &dh);
    if (dw > 0 && dh > 0 && (dw != coi_screen_w || dh != coi_screen_h)) {
      fprintf(stderr, "[egl_shim] real drawable %dx%d (requested %dx%d) -> adopting drawable\n",
              dw, dh, coi_screen_w, coi_screen_h);
      coi_screen_w = dw;
      coi_screen_h = dh;
    } else if (dw > 0 && dh > 0) {
      fprintf(stderr, "[egl_shim] drawable real %dx%d confirmado\n", dw, dh);
    }
  }
  /* COI_SWAPINT no contexto novo (a engine pode nunca chamar
   * eglSwapInterval; default SDL=vsync 1 + limiter da engine = 30fps). */
  {
    const char *f = getenv("COI_SWAPINT");
    if (f) {
      SDL_GL_SetSwapInterval(atoi(f));
      debugPrintf("egl_shim: swap interval forçado=%d\n", atoi(f));
    }
  }

  gl_makecurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  if (has_real_gl)
    return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (!egl_window || !ctx || !ctx->sdl_context)
    return 0;

  int ret = gl_makecurrent(egl_window, ctx->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    current_context = ctx;
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), ctx->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), ctx->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_share_root) {
    SDL_GL_DeleteContext(egl_share_root);
    egl_share_root = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  debugPrintf("egl_shim: eglChooseConfig()\n");
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)strdup("config");
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c)
    return EGL_NO_CONTEXT;

  pthread_mutex_lock(&egl_context_create_mutex);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  if (egl_share_root)
    gl_makecurrent(egl_window, egl_share_root);
  c->sdl_context = gl_createcontext(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  gl_makecurrent(egl_window, NULL);
  pthread_mutex_unlock(&egl_context_create_mutex);

  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    free(c);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d]\n",
              share_context, c, c->id);
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (egl_window) {
      gl_makecurrent(egl_window, NULL);
      /* debugPrintf("egl_shim: GL released [tid=%lx] reason=eglMakeCurrent(NULL)\n",
                    (unsigned long)pthread_self()); */
    }
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  int ret = gl_makecurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    /* COI_SWAPINT: intervalo é estado por-contexto; aplica 1x em cada */
    {
      static const char *si = (const char *)-1;
      if (si == (const char *)-1) si = getenv("COI_SWAPINT");
      if (si && !context->swapint_applied) {
        context->swapint_applied = 1;
        SDL_GL_SetSwapInterval(atoi(si));
      }
    }
    static int acq_log = 0;
    if (acq_log < 20 || mc % 500 == 0) {
      //debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] ACQUIRED [ctx_id=%d]\n",
      //            mc, is_window ? "WINDOW" : "PBUFFER",
      //            (unsigned long)pthread_self(), context->id);
      acq_log++;
    }
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED [ctx_id=%d]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (unsigned long)pthread_self(), context->id, SDL_GetError());
  }

  return EGL_TRUE;
}

/* screenshot sob demanda (receita Bully): `touch /dev/shm/coi_shot` ->
 * RGBA cru do backbuffer em /dev/shm/coi_shot.raw + .txt WxH (flip vertical
 * na conversao). Roda na thread de render, custo zero sem o trigger. */
static void coi_maybe_screenshot(void) {
  /* GATE OFF NO RELEASE, pelo mesmo motivo dos injetores de input: no binario
   * publico isto seria um access() por frame para sempre e, pior, um /dev/shm
   * com permissao frouxa viraria um jeito de qualquer processo dumpar a tela do
   * jogo do usuario. Ferramenta de bancada: COI_DEBUG_INJECT=1. */
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("COI_DEBUG_INJECT") ? 1 : 0;
  if (!enabled) return;
  static int chk = 0;
  if (++chk % 15) return;
  if (access("/dev/shm/coi_shot", F_OK) != 0) return;
  unlink("/dev/shm/coi_shot");
  GLint vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/coi_shot.raw", "wb");
  if (o) { fwrite(buf, 1, (size_t)w * h * 4, o); fclose(o); }
  FILE *t = fopen("/dev/shm/coi_shot.txt", "w");
  if (t) { fprintf(t, "%d %d\n", w, h); fclose(t); }
  free(buf);
  debugPrintf("[shot] %dx%d salvo\n", w, h);
}

/* ===== STACK SHRINK (RAM): o [stack] da thread principal cresce com recursao
 * funda da engine (ate ~131MB RSS medidos) e as paginas ficam residentes PRA
 * SEMPRE. Abaixo do SP atual a memoria e MORTA por definicao -> madvise
 * DONTNEED devolve as paginas ao kernel (re-toque = zero-fill, inofensivo).
 * Roda a cada ~900 frames, SO na thread principal. COI_NO_STACK_SHRINK=1 desliga. */
static void coi_stack_shrink(void) {
  static int mode = -1;          /* -1=probe, 0=off, 1=on */
  static uintptr_t st_lo = 0, st_hi = 0;
  if (mode == 0) return;
  if (mode < 0) {
    if (getenv("COI_NO_STACK_SHRINK")) { mode = 0; return; }
    if ((pid_t)syscall(SYS_gettid) != getpid()) { mode = 0; return; }  /* so main */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { mode = 0; return; }
    char ln[256];
    while (fgets(ln, sizeof ln, f))
      if (strstr(ln, "[stack]")) { sscanf(ln, "%lx-%lx", &st_lo, &st_hi); break; }
    fclose(f);
    if (!st_lo || !st_hi) { mode = 0; return; }
    mode = 1;
    fprintf(stderr, "[STACKSHRINK] [stack]=%lx-%lx (%lu MB reserved)\n",
            st_lo, st_hi, (st_hi - st_lo) >> 20);
  }
  uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  if (sp <= st_lo || sp >= st_hi) return;
  uintptr_t margin = 2u * 1024 * 1024;                 /* 2MB de folga abaixo do SP */
  uintptr_t end = (sp - margin) & ~0xFFFUL;
  if (end <= st_lo) return;
  size_t len = end - st_lo;
  if (len < 4u * 1024 * 1024) return;                  /* nao vale a syscall <4MB */
  if (madvise((void *)st_lo, len, MADV_DONTNEED) == 0) {
    static int n = 0;
    if (n < 4 || getenv("COI_PAGELOG"))
      { fprintf(stderr, "[STACKSHRINK] released %zu MB below the SP\n", len >> 20); n++; }
  }
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    coi_maybe_screenshot();
    { static unsigned fs = 0; if ((++fs % 900) == 0) coi_stack_shrink(); }
    /* Framework frame proof, immediately before the real present: three
     * samples over the run; the first can still be a legitimately black title
     * card. */
    {
      static unsigned long proof_frame;
      proof_frame++;
      if (proof_frame == 300 || proof_frame == 600 || proof_frame == 900) {
        nxgl_frame_proof_sample(0, 0);
        nxgl_frame_proof_publish();
      }
    }
    SDL_GL_SwapWindow(egl_window);
    /* [PERF] frame-time entre swaps; relatório a cada ~5s (diagnóstico do lag;
     * custo: 1 clock_gettime/frame + 1 fprintf/5s). */
    {
      static struct timespec last = {0, 0};
      static double sum = 0, mx = 0;
      static unsigned n = 0, s20 = 0, s40 = 0;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (last.tv_sec) {
        double ms = (now.tv_sec - last.tv_sec) * 1e3 +
                    (now.tv_nsec - last.tv_nsec) / 1e6;
        sum += ms; n++;
        if (ms > mx) mx = ms;
        if (ms > 20) s20++;
        if (ms > 40) s40++;
        if (sum >= 5000) {
          fprintf(stderr, "[PERF] fps=%.1f avg=%.1fms max=%.0fms >20ms=%u >40ms=%u\n",
                  n * 1000.0 / sum, sum / n, mx, s20, s40);
          sum = 0; n = 0; mx = 0; s20 = 0; s40 = 0;
        }
      }
      last = now;
    }
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      //debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
      //            fc, (unsigned long)pthread_self());
    }
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) {
    if (context->sdl_context)
      SDL_GL_DeleteContext(context->sdl_context);
    free(context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value) *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  debugPrintf("egl_shim: eglGetConfigAttrib(attr=0x%x)\n", attribute);
  if (!value) return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 8; break;
  case 0x3021: *value = 8; break;
  case 0x3022: *value = 8; break;
  case 0x3023: *value = 0; break;
  case 0x3025: *value = 24; break;
  case 0x3026: *value = 8; break;
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  /* Override GL: a engine resolve glGetString via procaddress; devolvemos NOSSA
   * versão (strings curtas) p/ evitar stack-smash com a lista de extensões do Mali. */
  extern void *coi_gl_proc_override(const char *name);
  void *ov = coi_gl_proc_override(procname);
  if (ov) { debugPrintf("egl_shim: proc override %s\n", procname); return ov; }

  void *ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case 0x3053: return "NextOS";      /* EGL_VENDOR */
  case 0x3054: return "1.4 NextOS";  /* EGL_VERSION */
  case 0x3055: return "";            /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";   /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  /* COI_SWAPINT força o intervalo (teste do double-pacing: engine dorme
   * ~16ms + vsync = 2 períodos = trava em 30fps; =0 deixa a engine ditar). */
  const char *f = getenv("COI_SWAPINT");
  if (f) interval = atoi(f);
  debugPrintf("egl_shim: SwapInterval(%d)%s\n", (int)interval, f ? " [forçado]" : "");
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  (void)readdraw;
  return (EGLSurface)"window";
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
