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
#include <png.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <dlfcn.h>

#include "egl_shim.h"
#include "jni_shim.h"
#include "util.h"

/* Resolucao DINAMICA (qualquer device): desktop mode do SDL com fallback
 * 1280x720. Exportada p/ imports.c (ANativeWindow_getWidth/Height — o que o
 * JOGO le) e android_shim.c (clamp do cursor). */
int dys_screen_w = 1280, dys_screen_h = 720;
#define SCREEN_WIDTH dys_screen_w
#define SCREEN_HEIGHT dys_screen_h

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
static volatile int g_intro_video_block;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_intro_video_begin(void) {
  g_intro_video_block = 1;
  fprintf(stderr, "[intro] GL swap blocked (fb0 = ffmpeg only)\n");
}

void egl_shim_intro_video_end(void) {
  g_intro_video_block = 0;
  egl_shim_ensure_current();
  fprintf(stderr, "[intro] GL present restored\n");
}

static int intro_blocks_present(void) {
  return g_intro_video_block || gtalcs2_intro_owns_screen();
}

static volatile int g_lcs_loading_visible;
static volatile int g_lcs_loading_bar_active;
static volatile int g_lcs_loading_hold_frames;
static volatile int g_lcs_loading_hack_active;
static float g_lcs_loading_progress;
static unsigned g_lcs_loading_serial;
static int g_lcs_loading_index = 1;
static uint32_t g_lcs_loading_rng = 0x4c435332u;
static uint32_t g_lcs_loading_hack_last_ms;
static volatile int g_loading_ui_armed;
static int g_lcs_loading_session_idx;
static int g_lcs_loading_session_presented;
static void lcs_loading_present_now(void);

static int loading_gate_on(void) {
  const char *g = getenv("GTALCS2_LOADING_GATE");
  if (!g || !*g)
    return 1;
  return g[0] != '0';
}

static int loading_ui_should_draw(void) {
  if (!loading_gate_on())
    return 1;
  return g_loading_ui_armed;
}

static void loading_maybe_present(void) {
  if (loading_ui_should_draw())
    lcs_loading_present_now();
  else if (getenv("GTALCS2_LOADING_VERBOSE"))
    fprintf(stderr, "[loading] suppressed (boot gate)\n");
}

static uint32_t lcs_loading_rand(void) {
  uint32_t x = g_lcs_loading_rng;
  if (!x)
    x = 0x4c435332u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_lcs_loading_rng = x;
  return x;
}

static int lcs_loading_forced_index(void) {
  const char *ei = getenv("GTALCS2_LOADING_INDEX");
  if (!ei || !*ei)
    return 0;
  int idx = atoi(ei);
  return (idx >= 1 && idx <= 4) ? idx : 1;
}

static int lcs_loading_random_index(void) {
  int forced = lcs_loading_forced_index();
  if (forced)
    return forced;
  g_lcs_loading_rng ^= (uint32_t)SDL_GetTicks() + 0x9e3779b9u + (g_lcs_loading_serial << 6);
  return 1 + (int)(lcs_loading_rand() % 4u);
}

static void loading_session_reset(void) {
  g_lcs_loading_session_idx = 0;
  g_lcs_loading_session_presented = 0;
}

static int loading_bar_img3_on(void) {
  const char *g = getenv("GTALCS2_LOADING_BAR_IMG3");
  return g && g[0] == '1';
}

static int loading_session_index(void) {
  if (!g_lcs_loading_session_idx)
    g_lcs_loading_session_idx = lcs_loading_random_index();
  return g_lcs_loading_session_idx;
}

static int loading_pick_index(int for_bar) {
  int forced = lcs_loading_forced_index();
  if (forced)
    return forced;
  if (loading_ui_should_draw()) {
    if (for_bar && loading_bar_img3_on())
      return 3;
    return loading_session_index();
  }
  return for_bar ? 3 : lcs_loading_random_index();
}

void gtalcs2_loading_arm_ui(void) {
  if (g_loading_ui_armed)
    return;
  g_loading_ui_armed = 1;
  loading_session_reset();
  fprintf(stderr, "[loading] ui armed session img=%d\n", loading_session_index());
}

void gtalcs2_loading_show(void) {
  g_lcs_loading_visible = 1;
  g_lcs_loading_bar_active = 0;
  g_lcs_loading_progress = 0.02f;
  g_lcs_loading_hold_frames = 0;
  g_lcs_loading_serial++;
  g_lcs_loading_index = loading_pick_index(0);
  if (loading_ui_should_draw() && !g_lcs_loading_session_presented) {
    loading_maybe_present();
    g_lcs_loading_session_presented = 1;
  }
  fprintf(stderr, "[loading] show img=%d\n", g_lcs_loading_index);
}

void gtalcs2_loading_hide(void) {
  int had_bar = g_lcs_loading_bar_active;
  g_lcs_loading_visible = 0;
  g_lcs_loading_bar_active = had_bar ? 1 : 0;
  if (had_bar)
    g_lcs_loading_progress = 1.0f;
  g_lcs_loading_hack_active = 0;
  g_lcs_loading_hold_frames = had_bar ? 900 : 2;
  if (loading_gate_on() && !g_loading_ui_armed)
    g_lcs_loading_hold_frames = 0;
  loading_maybe_present();
  fprintf(stderr, "[loading] hide\n");
  loading_session_reset();
}

void gtalcs2_loading_update_bar(float progress) {
  if (progress > 1.0f)
    progress *= 0.01f;
  if (progress < 0.0f)
    progress = 0.0f;
  if (progress > 1.0f)
    progress = 1.0f;
  g_lcs_loading_visible = 1;
  g_lcs_loading_bar_active = 1;
  g_lcs_loading_index = loading_pick_index(1);
  if (progress <= 0.001f && g_lcs_loading_progress > 0.75f)
    progress = g_lcs_loading_progress;
  g_lcs_loading_progress = progress;
  g_lcs_loading_hold_frames = 0;
  loading_maybe_present();
  static int n = 0;
  if (n < 160) {
    fprintf(stderr, "[loading] bar %.3f\n", progress);
    n++;
  }
}

void gtalcs2_loading_progress_hack(void) {
  gtalcs2_loading_arm_ui();
  g_lcs_loading_visible = 1;
  g_lcs_loading_bar_active = 1;
  g_lcs_loading_hack_active = 1;
  g_lcs_loading_index = loading_pick_index(1);
  g_lcs_loading_hack_last_ms = 0;
  g_lcs_loading_progress = 0.0f;
  g_lcs_loading_hold_frames = 0;
  loading_maybe_present();
  static int n = 0;
  if (n < 16) {
    fprintf(stderr, "[loading] bar hack %.3f\n", g_lcs_loading_progress);
    n++;
  }
}

void egl_shim_create_window(void) {
  /* resolucao nativa do device (TV 1080p, handheld 480p...) c/ fallback 720p */
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    dys_screen_w = dm.w; dys_screen_h = dm.h;
    debugPrintf("egl_shim: desktop mode %dx%d\n", dm.w, dm.h);
  }
  { const char *e = getenv("DYSMANTLE_RES"); int w, h; /* override opcional */
    if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      dys_screen_w = w; dys_screen_h = h;
      debugPrintf("egl_shim: DYSMANTLE_RES override %dx%d\n", w, h);
    } }
  /* ===== ESCADA DE CONFIG GL (receita bully2 multi-CFW) =====
   * Uma so config quebrava em CFW com EGL exigente: mali fbdev antigo prefere
   * alpha8; PowerVR (TrimUI) so tem depth16; mesa/panfrost pode devolver
   * EGL_BAD_CONFIG ou um contexto DESKTOP-GL (lição sonic4: rejeitar!).
   * Tentamos combos ate um dar contexto ES valido. GLVER=3 tenta ES3 primeiro
   * e cai pra ES2; GLVER=2 tenta ES2 primeiro e sobe pra ES3 em ultimo caso. */
  { const char *gv = getenv("DYSMANTLE_GLVER");
    int want3 = (gv && gv[0] == '3');
    struct { int major, alpha, depth, stencil; } ladder[10]; int ln = 0;
    int majors[2]; majors[0] = want3 ? 3 : 2; majors[1] = want3 ? 2 : 3;
    for (int m = 0; m < 2; m++) {
      ladder[ln].major = majors[m]; ladder[ln].alpha = 0; ladder[ln].depth = 24; ladder[ln].stencil = 8; ln++;
      ladder[ln].major = majors[m]; ladder[ln].alpha = 8; ladder[ln].depth = 24; ladder[ln].stencil = 8; ln++;
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
        fprintf(stderr, "[egl_shim] janela %dx%d criada (driver=%s, rung %d ES%d a%d d%d s%d)\n",
                SCREEN_WIDTH, SCREEN_HEIGHT, SDL_GetCurrentVideoDriver(),
                i, ladder[i].major, ladder[i].alpha, ladder[i].depth, ladder[i].stencil);
        fprintf(stderr, "[egl_shim] GL_VERSION='%s' RENDERER='%s' VENDOR='%s'\n",
                ver, gs ? (const char *)gs(0x1F01) : "?", gs ? (const char *)gs(0x1F00) : "?");
      }
    }
  }
  if (!egl_window || !egl_share_root) {
    fprintf(stderr, "[egl_shim] TODAS as configs GL falharam: %s\n", SDL_GetError());
    return;
  }
  fprintf(stderr, "[egl_shim] GL share-root context OK\n");
  /* DYSMANTLE_SWAPINT no contexto novo (a engine pode nunca chamar
   * eglSwapInterval; default SDL=vsync 1 + limiter da engine = 30fps). */
  {
    const char *f = getenv("DYSMANTLE_SWAPINT");
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

  /* MODELO BULLY: UM único contexto GL real (o da janela SDL). Todos os "contextos"
   * que o jogo cria mapeiam para egl_share_root -> o jogo renderiza direto no surface
   * fbdev da janela, e o present (raw eglSwapBuffers/SwapWindow) mostra o que ele
   * desenhou. Criar um 2º contexto SDL no Mali fbdev dava surface separada do fbdev
   * (render invisível no scanout). */
  c->sdl_context = egl_share_root;
  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext sem share_root!\n");
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
    /* DYSMANTLE_SWAPINT: intervalo é estado por-contexto; aplica 1x em cada */
    {
      static const char *si = (const char *)-1;
      if (si == (const char *)-1) si = getenv("DYSMANTLE_SWAPINT");
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

/* screenshot sob demanda (receita Bully): `touch /dev/shm/dys_shot` ->
 * RGBA cru do backbuffer em /dev/shm/dys_shot.raw + .txt WxH (flip vertical
 * na conversao). Roda na thread de render, custo zero sem o trigger. */
static void dys_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15) return;
  if (access("/dev/shm/dys_shot", F_OK) != 0) return;
  unlink("/dev/shm/dys_shot");
  GLint vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/dys_shot.raw", "wb");
  if (o) { fwrite(buf, 1, (size_t)w * h * 4, o); fclose(o); }
  FILE *t = fopen("/dev/shm/dys_shot.txt", "w");
  if (t) { fprintf(t, "%d %d\n", w, h); fclose(t); }
  free(buf);
  debugPrintf("[shot] %dx%d salvo\n", w, h);
}

/* ===== STACK SHRINK (RAM): o [stack] da thread principal cresce com recursao
 * funda da engine (ate ~131MB RSS medidos) e as paginas ficam residentes PRA
 * SEMPRE. Abaixo do SP atual a memoria e MORTA por definicao -> madvise
 * DONTNEED devolve as paginas ao kernel (re-toque = zero-fill, inofensivo).
 * Roda a cada ~900 frames, SO na thread principal. DYSMANTLE_NO_STACK_SHRINK=1 desliga. */
static void dys_stack_shrink(void) {
  static int mode = -1;          /* -1=probe, 0=off, 1=on */
  static uintptr_t st_lo = 0, st_hi = 0;
  if (mode == 0) return;
  if (mode < 0) {
    if (getenv("DYSMANTLE_NO_STACK_SHRINK")) { mode = 0; return; }
    if ((pid_t)syscall(SYS_gettid) != getpid()) { mode = 0; return; }  /* so main */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { mode = 0; return; }
    char ln[256];
    while (fgets(ln, sizeof ln, f))
      if (strstr(ln, "[stack]")) { sscanf(ln, "%lx-%lx", &st_lo, &st_hi); break; }
    fclose(f);
    if (!st_lo || !st_hi) { mode = 0; return; }
    mode = 1;
    fprintf(stderr, "[STACKSHRINK] [stack]=%lx-%lx (%lu MB reservado)\n",
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
    if (n < 4 || getenv("DYSMANTLE_PAGELOG"))
      { fprintf(stderr, "[STACKSHRINK] liberou %zu MB abaixo do SP\n", len >> 20); n++; }
  }
}

typedef struct {
  GLint enabled;
  GLint size;
  GLint type;
  GLint normalized;
  GLint stride;
  GLint buffer;
  void *pointer;
} VertexAttribState;

static GLuint lcs_loading_tex[5];
static int lcs_loading_tex_w[5], lcs_loading_tex_h[5];
static GLuint lcs_loading_prog_tex, lcs_loading_prog_col;
static GLint lcs_loading_tex_pos = -1, lcs_loading_tex_uv = -1, lcs_loading_tex_sampler = -1;
static GLint lcs_loading_col_pos = -1, lcs_loading_col_color = -1;
static int lcs_loading_assets_missing_logged;

static GLuint lcs_compile_shader(GLenum type, const char *src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    GLsizei n = 0;
    glGetShaderInfoLog(sh, sizeof log, &n, log);
    fprintf(stderr, "[loading] shader compile failed: %.*s\n", (int)n, log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static GLuint lcs_link_program(const char *vs_src, const char *fs_src) {
  GLuint vs = lcs_compile_shader(GL_VERTEX_SHADER, vs_src);
  GLuint fs = lcs_compile_shader(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    GLsizei n = 0;
    glGetProgramInfoLog(prog, sizeof log, &n, log);
    fprintf(stderr, "[loading] program link failed: %.*s\n", (int)n, log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

static int lcs_loading_init_programs(void) {
  if (lcs_loading_prog_tex && lcs_loading_prog_col)
    return 1;

  static const char *vs_tex =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main(){ v_uv=a_uv; gl_Position=vec4(a_pos,0.0,1.0); }\n";
  static const char *fs_tex =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main(){ gl_FragColor=texture2D(u_tex,v_uv); }\n";
  static const char *vs_col =
    "attribute vec2 a_pos;\n"
    "void main(){ gl_Position=vec4(a_pos,0.0,1.0); }\n";
  static const char *fs_col =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main(){ gl_FragColor=u_color; }\n";

  lcs_loading_prog_tex = lcs_link_program(vs_tex, fs_tex);
  lcs_loading_prog_col = lcs_link_program(vs_col, fs_col);
  if (!lcs_loading_prog_tex || !lcs_loading_prog_col)
    return 0;

  lcs_loading_tex_pos = glGetAttribLocation(lcs_loading_prog_tex, "a_pos");
  lcs_loading_tex_uv = glGetAttribLocation(lcs_loading_prog_tex, "a_uv");
  lcs_loading_tex_sampler = glGetUniformLocation(lcs_loading_prog_tex, "u_tex");
  lcs_loading_col_pos = glGetAttribLocation(lcs_loading_prog_col, "a_pos");
  lcs_loading_col_color = glGetUniformLocation(lcs_loading_prog_col, "u_color");
  return lcs_loading_tex_pos >= 0 && lcs_loading_tex_uv >= 0 && lcs_loading_col_pos >= 0;
}

static unsigned char *lcs_load_png_rgba(const char *path, int *out_w, int *out_h) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  unsigned char sig[8];
  if (fread(sig, 1, sizeof sig, fp) != sizeof sig || png_sig_cmp(sig, 0, sizeof sig)) {
    fclose(fp);
    return NULL;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(fp);
    return NULL;
  }
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    return NULL;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return NULL;
  }

  png_init_io(png, fp);
  png_set_sig_bytes(png, sizeof sig);
  png_read_info(png, info);

  int w = (int)png_get_image_width(png, info);
  int h = (int)png_get_image_height(png, info);
  png_byte color = png_get_color_type(png, info);
  png_byte depth = png_get_bit_depth(png, info);

  if (depth == 16)
    png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  if (!(color & PNG_COLOR_MASK_ALPHA))
    png_set_filler(png, 0xff, PNG_FILLER_AFTER);

  png_read_update_info(png, info);
  size_t rowbytes = png_get_rowbytes(png, info);
  unsigned char *rgba = malloc(rowbytes * (size_t)h);
  png_bytep *rows = malloc(sizeof(png_bytep) * (size_t)h);
  if (!rgba || !rows) {
    free(rows);
    free(rgba);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return NULL;
  }

  for (int y = 0; y < h; y++)
    rows[y] = rgba + (size_t)(h - 1 - y) * rowbytes;
  png_read_image(png, rows);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);

  *out_w = w;
  *out_h = h;
  return rgba;
}

static int lcs_loading_find_png(int idx, char *out, size_t out_sz) {
  const char *forced = getenv("GTALCS2_LOADING_PNG");
  if (forced && *forced && access(forced, R_OK) == 0) {
    snprintf(out, out_sz, "%s", forced);
    return 1;
  }

  if (idx < 1 || idx > 4)
    idx = 1;

  const char *roots[] = {
    "loading",
    "res/drawable",
    "assets/loading",
    ".",
    NULL
  };
  for (int i = 0; roots[i]; i++) {
    snprintf(out, out_sz, "%s/loading_screen_%d.png", roots[i], idx);
    if (access(out, R_OK) == 0)
      return 1;
  }
  if (idx != 1) {
    for (int i = 0; roots[i]; i++) {
      snprintf(out, out_sz, "%s/loading_screen_1.png", roots[i]);
      if (access(out, R_OK) == 0)
        return 1;
    }
  }
  return 0;
}

static int lcs_loading_ensure_texture(int idx) {
  if (idx < 1 || idx > 4)
    idx = 1;
  if (lcs_loading_tex[idx])
    return 1;

  char path[512];
  if (!lcs_loading_find_png(idx, path, sizeof path)) {
    if (!lcs_loading_assets_missing_logged) {
      fprintf(stderr, "[loading] loading_screen_*.png ausente; usando fallback sem imagem\n");
      lcs_loading_assets_missing_logged = 1;
    }
    return 0;
  }

  int w = 0, h = 0;
  unsigned char *rgba = lcs_load_png_rgba(path, &w, &h);
  if (!rgba || w <= 0 || h <= 0) {
    fprintf(stderr, "[loading] falha lendo %s\n", path);
    free(rgba);
    return 0;
  }

  GLint old_unpack_alignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack_alignment);

  glGenTextures(1, &lcs_loading_tex[idx]);
  glBindTexture(GL_TEXTURE_2D, lcs_loading_tex[idx]);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack_alignment);
  GLenum err = glGetError();
  free(rgba);
  if (err) {
    fprintf(stderr, "[loading] upload texture err=0x%x (%s)\n", err, path);
    glDeleteTextures(1, &lcs_loading_tex[idx]);
    lcs_loading_tex[idx] = 0;
    return 0;
  }

  lcs_loading_tex_w[idx] = w;
  lcs_loading_tex_h[idx] = h;
  fprintf(stderr, "[loading] textura%d %s %dx%d\n", idx, path, w, h);
  return 1;
}

static void lcs_save_attrib(GLuint idx, VertexAttribState *s) {
  glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->enabled);
  glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s->size);
  glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s->type);
  glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s->normalized);
  glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s->stride);
  glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s->buffer);
  glGetVertexAttribPointerv(idx, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s->pointer);
}

static void lcs_restore_attrib(GLuint idx, const VertexAttribState *s) {
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->buffer);
  glVertexAttribPointer(idx, s->size, (GLenum)s->type, (GLboolean)s->normalized,
                        s->stride, s->pointer);
  if (s->enabled)
    glEnableVertexAttribArray(idx);
  else
    glDisableVertexAttribArray(idx);
}

static void lcs_draw_color_rect(float x0, float y0, float x1, float y1,
                                float r, float g, float b, float a) {
  GLfloat v[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
  glUseProgram(lcs_loading_prog_col);
  glUniform4f(lcs_loading_col_color, r, g, b, a);
  glEnableVertexAttribArray((GLuint)lcs_loading_col_pos);
  glVertexAttribPointer((GLuint)lcs_loading_col_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void lcs_draw_loading_texture(const GLint vp[4], int idx) {
  if (idx < 1 || idx > 4)
    idx = 1;
  if (!lcs_loading_ensure_texture(idx))
    return;

  float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
  float screen_aspect = vp[3] > 0 ? (float)vp[2] / (float)vp[3] : 16.0f / 9.0f;
  float img_aspect = lcs_loading_tex_h[idx] > 0 ? (float)lcs_loading_tex_w[idx] / (float)lcs_loading_tex_h[idx] : 4.0f / 3.0f;
  if (screen_aspect > img_aspect) {
    float visible = img_aspect / screen_aspect;
    float crop = (1.0f - visible) * 0.5f;
    v0 = crop;
    v1 = 1.0f - crop;
  } else if (screen_aspect < img_aspect) {
    float visible = screen_aspect / img_aspect;
    float crop = (1.0f - visible) * 0.5f;
    u0 = crop;
    u1 = 1.0f - crop;
  }

  GLfloat v[] = {
    -1.0f, -1.0f, u0, v0,
     1.0f, -1.0f, u1, v0,
    -1.0f,  1.0f, u0, v1,
     1.0f,  1.0f, u1, v1
  };

  glUseProgram(lcs_loading_prog_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, lcs_loading_tex[idx]);
  glUniform1i(lcs_loading_tex_sampler, 0);
  glEnableVertexAttribArray((GLuint)lcs_loading_tex_pos);
  glEnableVertexAttribArray((GLuint)lcs_loading_tex_uv);
  glVertexAttribPointer((GLuint)lcs_loading_tex_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), v);
  glVertexAttribPointer((GLuint)lcs_loading_tex_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), v + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int lcs_backbuffer_has_visible_content(const GLint vp[4]) {
  if (vp[2] <= 0 || vp[3] <= 0)
    return 0;

  GLint old_pack = 4;
  glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);

  int hits = 0;
  for (int yy = 1; yy <= 3; yy++) {
    for (int xx = 1; xx <= 3; xx++) {
      int x = vp[0] + (vp[2] * xx) / 4;
      int y = vp[1] + (vp[3] * yy) / 4;
      GLubyte px[4] = {0, 0, 0, 0};
      glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      int rgb = (int)px[0] + (int)px[1] + (int)px[2];
      if (px[3] > 8 && rgb > 54)
        hits++;
    }
  }

  glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
  return hits >= 2;
}

static void lcs_loading_apply_progress_hack(void) {
  if (!g_lcs_loading_hack_active || !g_lcs_loading_visible)
    return;

  uint32_t now = SDL_GetTicks();
  if (g_lcs_loading_hack_last_ms && now - g_lcs_loading_hack_last_ms < 250)
    return;
  g_lcs_loading_hack_last_ms = now ? now : 1;

  float p = g_lcs_loading_progress;
  p += (1.0f - p) * 0.025f;
  if (p > 0.95f)
    p = 0.95f;

  /* Android atualiza a barra em cerca de 60% dos passos de 250 ms. */
  if ((lcs_loading_rand() % 1000u) > 400u)
    if (p > g_lcs_loading_progress)
      g_lcs_loading_progress = p;

  g_lcs_loading_bar_active = 1;
  g_lcs_loading_index = loading_pick_index(1);
}

static void lcs_loading_render(void) {
  if (getenv("GTALCS2_LOADING_OFF"))
    return;
  if (loading_gate_on() && !g_loading_ui_armed)
    return;
  int holding_after_hide = !g_lcs_loading_visible;
  if (holding_after_hide) {
    if (g_lcs_loading_hold_frames <= 0)
      return;
    g_lcs_loading_hold_frames--;
  }
  if (!has_real_gl || !current_context || current_context->is_pbuffer)
    return;
  lcs_loading_apply_progress_hack();
  if (!lcs_loading_init_programs())
    return;

  GLint old_prog = 0, old_active = 0, old_tex = 0, old_tex0 = 0, old_array = 0, old_fb = 0;
  GLint old_viewport[4] = {0, 0, 0, 0};
  GLboolean old_blend = glIsEnabled(GL_BLEND);
  GLboolean old_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean old_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLint old_src_rgb = 0, old_dst_rgb = 0, old_src_a = 0, old_dst_a = 0;
  GLint old_eq_rgb = 0, old_eq_a = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex0);
  glActiveTexture((GLenum)old_active);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  glGetIntegerv(GL_BLEND_SRC_RGB, &old_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &old_dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &old_src_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &old_dst_a);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &old_eq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &old_eq_a);

  VertexAttribState pos_state = {0}, uv_state = {0}, col_state = {0};
  if (lcs_loading_tex_pos >= 0)
    lcs_save_attrib((GLuint)lcs_loading_tex_pos, &pos_state);
  if (lcs_loading_tex_uv >= 0)
    lcs_save_attrib((GLuint)lcs_loading_tex_uv, &uv_state);
  if (lcs_loading_col_pos >= 0 && lcs_loading_col_pos != lcs_loading_tex_pos &&
      lcs_loading_col_pos != lcs_loading_tex_uv)
    lcs_save_attrib((GLuint)lcs_loading_col_pos, &col_state);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  int draw_loading = 1;
  if (holding_after_hide && lcs_backbuffer_has_visible_content(old_viewport)) {
    g_lcs_loading_hold_frames = 0;
    draw_loading = 0;
  }

  if (draw_loading) {
    lcs_draw_color_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.02f, 0.02f, 0.025f, 1.0f);
    lcs_draw_loading_texture(old_viewport, g_lcs_loading_index);

    if (g_lcs_loading_bar_active) {
      float p = g_lcs_loading_progress;
      if (p < 0.0f) p = 0.0f;
      if (p > 1.0f) p = 1.0f;

      float y0 = -0.875f, y1 = -0.835f;
      float x0 = -0.50f, x1 = 0.50f;
      float pad = 0.010f;
      lcs_draw_color_rect(x0 - pad, y0 - pad, x1 + pad, y1 + pad, 0.0f, 0.0f, 0.0f, 0.68f);
      lcs_draw_color_rect(x0, y0, x1, y1, 0.08f, 0.04f, 0.035f, 0.92f);
      lcs_draw_color_rect(x0, y0, x0 + (x1 - x0) * p, y1, 0.78f, 0.04f, 0.025f, 1.0f);
    }
  }

  if (lcs_loading_col_pos >= 0 && lcs_loading_col_pos != lcs_loading_tex_pos &&
      lcs_loading_col_pos != lcs_loading_tex_uv)
    lcs_restore_attrib((GLuint)lcs_loading_col_pos, &col_state);
  if (lcs_loading_tex_uv >= 0)
    lcs_restore_attrib((GLuint)lcs_loading_tex_uv, &uv_state);
  if (lcs_loading_tex_pos >= 0)
    lcs_restore_attrib((GLuint)lcs_loading_tex_pos, &pos_state);

  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_array);
  glUseProgram((GLuint)old_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex0);
  glActiveTexture((GLenum)old_active);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
  glBlendEquationSeparate((GLenum)old_eq_rgb, (GLenum)old_eq_a);
  glBlendFuncSeparate((GLenum)old_src_rgb, (GLenum)old_dst_rgb, (GLenum)old_src_a, (GLenum)old_dst_a);
  if (old_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
  if (old_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (old_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

static void lcs_loading_present_now(void) {
  static int presenting;
  if (presenting || getenv("GTALCS2_LOADING_OFF") || intro_blocks_present())
    return;
  if (!egl_window || !has_real_gl || !current_context || current_context->is_pbuffer)
    return;
  if (!SDL_GL_GetCurrentContext() || SDL_GL_GetCurrentWindow() != egl_window)
    return;

  presenting = 1;
  lcs_loading_render();
  SDL_GL_SwapWindow(egl_window);
  presenting = 0;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;
  if (intro_blocks_present())
    return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    { static int n = 0; if (getenv("GTACTW_GLDBG") && n < 6) {
        GLint fb = -1, vp[4] = {0,0,0,0};
        glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fb);
        glGetIntegerv(GL_VIEWPORT, vp);
        fprintf(stderr, "[GLDBG] SWAP #%d fbo=%d viewport=%d,%d %dx%d ctx=%d tid=%ld\n",
                n, fb, vp[0], vp[1], vp[2], vp[3], current_context->id,
                (long)syscall(SYS_gettid));
        n++;
    } }
    /* TESTE: pinta o backbuffer de vermelho antes de apresentar. Se a tela fica
     * vermelha, present+contexto OK e o problema é onde o jogo desenha. */
    if (getenv("GTACTW_GLTEST")) {
      void (*cc)(float,float,float,float) = (void*)SDL_GL_GetProcAddress("glClearColor");
      void (*cl)(unsigned) = (void*)SDL_GL_GetProcAddress("glClear");
      if (cc) cc(1.0f, 0.0f, 0.0f, 1.0f);
      if (cl) cl(0x4000 /*GL_COLOR_BUFFER_BIT*/);
    }
    lcs_loading_render();
    dys_maybe_screenshot();
    { static unsigned fs = 0; if ((++fs % 900) == 0) dys_stack_shrink(); }
    /* APRESENTAÇÃO: neste Mali-450/Amlogic-old o SDL_GL_SwapWindow é que CHEGA ao
     * /dev/fb0 (teste do clear vermelho confirmou); o eglSwapBuffers cru NÃO presenta.
     * (No R36S/KMSDRM do Bully era o oposto — por isso fica device-específico.) */
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
    /* NÃO deletar sdl_context: é o egl_share_root compartilhado (modelo Bully). */
    if (current_context == context) current_context = NULL;
    if (last_context == context) last_context = NULL;
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
  extern void *dysmantle_gl_proc_override(const char *name);
  void *ov = dysmantle_gl_proc_override(procname);
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
  /* DYSMANTLE_SWAPINT força o intervalo (teste do double-pacing: engine dorme
   * ~16ms + vsync = 2 períodos = trava em 30fps; =0 deixa a engine ditar). */
  const char *f = getenv("DYSMANTLE_SWAPINT");
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
