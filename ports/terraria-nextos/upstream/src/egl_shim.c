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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "egl_shim.h"
#include "android_compat.h"
#include "util.h"

static int g_screen_width = 640;
static int g_screen_height = 480;
static int g_alpha_size;
static int g_depth_size = 24;
static int g_stencil_size = 8;

/* --- libEGL REAL (Valhall) p/ as queries de config do gles-api-check do Cuphead ---
 * O shim fakeia a maior parte do EGL (a janela/contexto vêm do SDL3/kmsdrm), mas o
 * Cuphead ENUMERA configs (eglChooseConfig + eglGetConfigAttrib) p/ validar GLES3.
 * Dados falsos -> ele não acha config boa, retenta e ABORTA (SIGTRAP). Solução:
 * pegar o EGLDisplay REAL que o SDL já inicializou (eglGetCurrentDisplay após o
 * contexto current) e delegar Choose/GetConfigAttrib pro libEGL real -> Unity vê
 * uma config ES3 de verdade. */
static void *(*r_eglGetCurrentDisplay)(void);
static unsigned (*r_eglChooseConfig)(void *, const int *, void **, int, int *);
static unsigned (*r_eglGetConfigAttrib)(void *, void *, int, int *);
static void *(*r_eglGetCurrentSurface)(int);
static void *(*r_eglCreatePbufferSurface)(void *, void *, const int *);
static unsigned (*r_eglMakeCurrent)(void *, void *, void *, void *);
static void *(*r_eglCreateContext)(void *, void *, void *, const int *);
/* dono da WINDOW é por-THREAD (a thread de render do Unity); as demais threads usam
 * um contexto real próprio (compartilhado com o share-root) num pbuffer. */
static pthread_t g_owner_thread;
static int g_have_owner = 0;
static pthread_mutex_t g_owner_mtx2 = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *tls_real_ctx = NULL;   /* contexto real próprio da thread (pbuffer) */
static _Thread_local int tls_is_window = 0;       /* esta thread renderiza na window? */
static void *g_real_dpy = NULL;   /* EGLDisplay real do SDL */
static void *g_real_cfg = NULL;   /* EGLConfig real ES3 */
static void *g_win_surf = NULL;   /* EGLSurface REAL da window do SDL */
static void *g_pbuf = NULL;       /* pbuffer REAL p/ contextos worker (uploads) */
/* Dono da surface da window: o 1º contexto que pede MakeCurrent(window) fica com a
 * window; os demais (worker do Unity) recebem o pbuffer -> sem EGL_BAD_ACCESS
 * (2 contextos NÃO podem compartilhar a mesma surface). Compartilham recursos, então
 * o upload do worker vale pro render. */
static SDL_GLContext g_window_owner = NULL;

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  EGLSurface current_draw;
  EGLSurface current_read;
  int id;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;
static int frame_count = 0;
static int next_context_id = 1;
/* Terraria's Android renderer is GLES2. A diagnostic override may request ES3,
 * with ES2 retained as the compatibility fallback. */
static int g_es_major = 0;
static int g_es_minor = 0;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

static int positive_env(const char *name) {
  const char *value = getenv(name);
  if (!value || !*value) return 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  return end != value && parsed > 0 && parsed < 32768 ? (int)parsed : 0;
}

static int context_is_gles(void) {
  const unsigned char *(*get_string)(unsigned int) =
      (const unsigned char *(*)(unsigned int))SDL_GL_GetProcAddress("glGetString");
  const char *version = get_string ? (const char *)get_string(GL_VERSION) : NULL;
  return !version || strstr(version, "OpenGL ES") != NULL;
}

static void egl_set_ctx_attrs(int major, int minor, int alpha, int depth, int stencil) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

void egl_shim_create_window(void) {
  int width = positive_env("TER_SCREEN_W");
  int height = positive_env("TER_SCREEN_H");
  if (!width) width = positive_env("TER_SCREEN_WIDTH");
  if (!height) height = positive_env("TER_SCREEN_HEIGHT");
  if (width <= 0 || height <= 0) {
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
      width = mode.w;
      height = mode.h;
    }
  }
  if (width <= 0 || height <= 0) { width = 640; height = 480; }

  int want_major = 2, want_minor = 0;
  const char *envm = getenv("CUP_GLES_MAJOR");
  if (envm && (envm[0] == '2' || envm[0] == '3')) want_major = envm[0] - '0';
  int versions[2] = {want_major, 2};
  int version_count = want_major == 2 ? 1 : 2;
  static const struct { int alpha, depth, stencil; } formats[] = {
    {8, 24, 8}, {0, 24, 8}, {8, 16, 0}, {0, 16, 0}, {0, 0, 0}
  };

  int portable_provider_active = 0;
  for (int provider_attempt = 0;
       provider_attempt < 2 && !egl_share_root;
       ++provider_attempt) {
    if (provider_attempt == 1) {
      if (!ter_allow_portable_provider_retry(
              getenv("SDL_VIDEO_EGL_DRIVER"),
              getenv("SDL_VIDEO_GL_DRIVER"), 1))
        break;

      /* SDL caches provider discovery in its video subsystem. The first
       * attempt has already destroyed every failed window, so a complete
       * video teardown here preserves the lifecycle and makes the retry real.
       * This is the same measured fallback proven by canonical NXSplash. */
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      if (setenv("SDL_VIDEO_EGL_DRIVER", "libEGL.so", 1) != 0 ||
          setenv("SDL_VIDEO_GL_DRIVER", "libGLESv2.so", 1) != 0) {
        unsetenv("SDL_VIDEO_EGL_DRIVER");
        unsetenv("SDL_VIDEO_GL_DRIVER");
        (void)SDL_InitSubSystem(SDL_INIT_VIDEO);
        break;
      }
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        debugPrintf("egl_shim: portable provider video reinit falhou: %s\n",
                    SDL_GetError());
        unsetenv("SDL_VIDEO_EGL_DRIVER");
        unsetenv("SDL_VIDEO_GL_DRIVER");
        (void)SDL_InitSubSystem(SDL_INIT_VIDEO);
        break;
      }
      portable_provider_active = 1;
      debugPrintf("egl_shim: retrying portable EGL/GLES provider names\n");
    }

    for (size_t f = 0;
         f < sizeof(formats) / sizeof(formats[0]) && !egl_share_root;
         f++) {
      egl_set_ctx_attrs(versions[0], 0, formats[f].alpha,
                        formats[f].depth, formats[f].stencil);
      egl_window = SDL_CreateWindow(
          PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
          width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
      if (!egl_window) {
        debugPrintf("egl_shim: janela a%d d%d s%d falhou: %s\n",
                    formats[f].alpha, formats[f].depth, formats[f].stencil,
                    SDL_GetError());
        continue;
      }
      for (int v = 0; v < version_count && !egl_share_root; v++) {
        egl_set_ctx_attrs(versions[v], 0, formats[f].alpha,
                          formats[f].depth, formats[f].stencil);
        egl_share_root = SDL_GL_CreateContext(egl_window);
        if (egl_share_root && !context_is_gles()) {
          debugPrintf("egl_shim: backend devolveu OpenGL desktop; rejeitado\n");
          SDL_GL_DeleteContext(egl_share_root);
          egl_share_root = NULL;
        }
        if (egl_share_root) {
          g_es_major = versions[v];
          g_es_minor = 0;
          g_alpha_size = formats[f].alpha;
          g_depth_size = formats[f].depth;
          g_stencil_size = formats[f].stencil;
        }
      }
      if (!egl_share_root) {
        SDL_DestroyWindow(egl_window);
        egl_window = NULL;
      }
    }
  }
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    if (portable_provider_active) {
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      unsetenv("SDL_VIDEO_EGL_DRIVER");
      unsetenv("SDL_VIDEO_GL_DRIVER");
      (void)SDL_InitSubSystem(SDL_INIT_VIDEO);
    }
    return;
  }
  if (portable_provider_active)
    debugPrintf("egl_shim: recovered with portable EGL/GLES provider names\n");
  SDL_GL_GetDrawableSize(egl_window, &g_screen_width, &g_screen_height);
  if (g_screen_width <= 0 || g_screen_height <= 0) {
    g_screen_width = width;
    g_screen_height = height;
  }
  char sw[16], sh[16];
  snprintf(sw, sizeof(sw), "%d", g_screen_width);
  snprintf(sh, sizeof(sh), "%d", g_screen_height);
  setenv("TER_SCREEN_W", sw, 1);
  setenv("TER_SCREEN_H", sh, 1);
  SDL_GL_SetSwapInterval(1);

  const unsigned char *(*get_string)(unsigned int) =
      (const unsigned char *(*)(unsigned int))SDL_GL_GetProcAddress("glGetString");
  debugPrintf("egl_shim: SDL %s ES%d a%d/d%d/s%d drawable=%dx%d\n",
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
              g_es_major, g_alpha_size, g_depth_size, g_stencil_size,
              g_screen_width, g_screen_height);
  if (get_string) {
    debugPrintf("egl_shim: GL_VENDOR=%s\n", get_string(GL_VENDOR));
    debugPrintf("egl_shim: GL_RENDERER=%s\n", get_string(GL_RENDERER));
    debugPrintf("egl_shim: GL_VERSION=%s\n", get_string(GL_VERSION));
  }

  SDL_GL_MakeCurrent(egl_window, NULL);
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

  int ret;
  if (g_real_dpy && r_eglMakeCurrent && g_win_surf) {
    int is_main = (syscall(SYS_gettid) == getpid()) && !getenv("CUP_MAINGL");
    int am_owner = !is_main && g_have_owner && pthread_equal(g_owner_thread, pthread_self());
    void *use_ctx = (void *)ctx->sdl_context, *surf;
    if (am_owner) { surf = g_win_surf; tls_is_window = 1; }
    else {
      if (!tls_real_ctx && r_eglCreateContext) {
        int ca[] = { 0x3098, g_es_major, 0x3038 };
        tls_real_ctx = r_eglCreateContext(g_real_dpy, g_real_cfg, egl_share_root, ca);
      }
      if (tls_real_ctx) use_ctx = tls_real_ctx;
      surf = g_pbuf ? g_pbuf : g_win_surf; tls_is_window = 0;
    }
    ret = r_eglMakeCurrent(g_real_dpy, surf, surf, use_ctx) ? 0 : -1;
  } else {
    ret = SDL_GL_MakeCurrent(egl_window, ctx->sdl_context);
  }
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
  if (configs && config_size > 0)
    configs[0] = g_real_cfg ? (EGLConfig)g_real_cfg : (EGLConfig)strdup("config");
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
  /* mesma versão ES que o root (ES3 no Valhall, ES2 fallback) */
  if (g_es_major)
    egl_set_ctx_attrs(g_es_major, g_es_minor, g_alpha_size,
                      g_depth_size, g_stencil_size);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  int root_bind = egl_share_root
                      ? SDL_GL_MakeCurrent(egl_window, egl_share_root) : 0;
  c->sdl_context = SDL_GL_CreateContext(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  int release = SDL_GL_MakeCurrent(egl_window, NULL);
  pthread_mutex_unlock(&egl_context_create_mutex);

  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    free(c);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d tid=%ld "
              "root_bind=%d release=%d current=%p]\n",
              share_context, c, c->id, (long)syscall(SYS_gettid),
              root_bind, release, SDL_GL_GetCurrentContext());
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static _Thread_local int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (g_real_dpy && r_eglMakeCurrent && g_win_surf)
      r_eglMakeCurrent(g_real_dpy, NULL, NULL, NULL);  /* release real */
    else if (egl_window)
      SDL_GL_MakeCurrent(egl_window, NULL);
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  /* Android EGL returns the exact surface handles bound by eglMakeCurrent.
     Unity compares their identity before Present; returning a string literal
     with the same contents makes it silently skip eglSwapBuffers. */
  context->current_draw = draw;
  context->current_read = read;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  /* Caminho EGL REAL (multi-contexto do Unity): o 1º contexto a pedir a WINDOW vira
     dono e usa a surface real da window (renderiza/swap); os outros recebem o pbuffer
     (worker de upload, recursos compartilhados) -> elimina o EGL_BAD_ACCESS de 2
     contextos na mesma surface. Usado quando temos display+surfaces reais. */
  if (g_real_dpy && r_eglMakeCurrent && g_win_surf) {
    /* dono da window = a RENDER THREAD do Unity (GfxDeviceWorker), que faz o GL+present.
       A main thread (tid==pid) só submete comandos → recebe pbuffer (contexto próprio,
       recursos compartilhados). Sem isto a main pegava a window e o worker o pbuffer →
       worker não apresentava → livelock no nativeRender. CUP_MAINGL força a main na window. */
    int is_main = (syscall(SYS_gettid) == getpid()) && !getenv("CUP_MAINGL");
    pthread_mutex_lock(&g_owner_mtx2);
    if (is_window && !is_main && !g_have_owner) { g_have_owner = 1; g_owner_thread = pthread_self(); }
    int am_owner = !is_main && g_have_owner && pthread_equal(g_owner_thread, pthread_self());
    pthread_mutex_unlock(&g_owner_mtx2);

    void *use_ctx, *surf;
    if (am_owner) {
      use_ctx = (void *)context->sdl_context;   /* contexto real do jogo na window */
      surf = g_win_surf;
      tls_is_window = 1;
    } else {
      /* contexto real PRÓPRIO desta thread (compartilha recursos c/ o share-root) */
      if (!tls_real_ctx && r_eglCreateContext) {
        int ca[] = { 0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, g_es_major, 0x3038 };
        tls_real_ctx = r_eglCreateContext(g_real_dpy, g_real_cfg, egl_share_root, ca);
      }
      use_ctx = tls_real_ctx ? tls_real_ctx : (void *)context->sdl_context;
      surf = g_pbuf ? g_pbuf : g_win_surf;
      tls_is_window = 0;
    }
    unsigned ok = r_eglMakeCurrent(g_real_dpy, surf, surf, use_ctx);
    if (ok) {
      has_real_gl = 1;
      static _Thread_local int acq_log = 0;
      if (acq_log < 6) {
        debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] REAL OK [ctx_id=%d] %s\n",
                    mc, (unsigned long)pthread_self(), context->id,
                    am_owner ? "WIN(owner)" : "PBUF(worker)");
        acq_log++;
      }
      return EGL_TRUE;
    }
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] REAL FAILED [ctx_id=%d] %s\n",
                mc, (unsigned long)pthread_self(), context->id,
                am_owner ? "WIN" : "PBUF");
    return EGL_TRUE;
  }

  int ret = SDL_GL_MakeCurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%ld/%lx] SDL FAILED "
                "[ctx_id=%d target=%p current=%p]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (long)syscall(SYS_gettid), (unsigned long)pthread_self(),
                context->id, context->sdl_context,
                SDL_GL_GetCurrentContext(), SDL_GetError());
    return EGL_FALSE;
  }

  return EGL_TRUE;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && (tls_is_window || (current_context && !current_context->is_pbuffer))) {
    { extern void ter_before_present(void); ter_before_present(); }
    SDL_GL_SwapWindow(egl_window);
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
  if (attribute == 0x3057 && value) *value = g_screen_width;
  else if (attribute == 0x3056 && value) *value = g_screen_height;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  /* An explicit raw-EGL diagnostic path may supply a real config. Normal public
     operation reports the SDL context that was actually created. */
  if (g_real_dpy && g_real_cfg && r_eglGetConfigAttrib &&
      r_eglGetConfigAttrib(g_real_dpy, g_real_cfg, attribute, value))
    return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 24 + g_alpha_size; break; /* EGL_BUFFER_SIZE */
  case 0x3021: *value = g_alpha_size; break;      /* EGL_ALPHA_SIZE */
  case 0x3022: *value = 8; break;   /* EGL_BLUE_SIZE */
  case 0x3023: *value = 8; break;   /* EGL_GREEN_SIZE */
  case 0x3024: *value = 8; break;   /* EGL_RED_SIZE */
  case 0x3025: *value = g_depth_size; break;
  case 0x3026: *value = g_stencil_size; break;
  case 0x3027: *value = 0x3038; break; /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = 1; break;   /* EGL_CONFIG_ID */
  case 0x3033: *value = 0x0005; break; /* EGL_SURFACE_TYPE = WINDOW|PBUFFER */
  case 0x3040: *value = g_es_major >= 3 ? 0x40 : 0x04; break;
  case 0x3042: *value = g_es_major >= 3 ? 0x40 : 0x04; break;
  case 0x3039: *value = 0x308E; break; /* EGL_COLOR_BUFFER_TYPE = EGL_RGB_BUFFER */
  case 0x3032: *value = 0; break;   /* EGL_NATIVE_RENDERABLE */
  default:
    debugPrintf("egl_shim: GetConfigAttrib(0x%x) -> 0 (default)\n", attribute);
    *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
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
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  if (!current_context)
    return EGL_NO_SURFACE;
  return readdraw == 0x305A /* EGL_READ */
             ? current_context->current_read
             : current_context->current_draw;
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
