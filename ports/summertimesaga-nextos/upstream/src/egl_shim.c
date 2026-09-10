#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (adaptive OpenGL ES)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>

#include "egl_shim.h"
#include "util.h"

/* Resolucao DINAMICA (qualquer device): desktop mode do SDL com fallback
 * 1280x720. Exportada p/ imports.c (ANativeWindow_getWidth/Height — o que o
 * JOGO le) e android_shim.c (clamp do cursor). */
int summertime_screen_w = 1280, summertime_screen_h = 720;
#define SCREEN_WIDTH summertime_screen_w
#define SCREEN_HEIGHT summertime_screen_h

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
static int g_es_major = 0;
static int g_es_minor = 0;
static int g_depth_size = 24;
static int g_stencil_size = 8;

static _egl_context *current_context = NULL;
static _egl_context *last_context = NULL;
static int has_real_gl = 0;

extern void summertime_gl_debug_frame(void);
extern void *summertime_gl_lookup(const char *name);
extern volatile float summertime_cursor_x;
extern volatile float summertime_cursor_y;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

static int ss_env_on(const char *name) {
  const char *value = getenv(name);
  return value && *value && strcmp(value, "0") != 0 &&
         strcasecmp(value, "false") != 0 &&
         strcasecmp(value, "no") != 0 &&
         strcasecmp(value, "off") != 0;
}

static void ss_env_default(const char *name, const char *value) {
  const char *current = getenv(name);
  if (!current || !*current)
    setenv(name, value, 1);
}

static long ss_memtotal_kb(void) {
  FILE *file = fopen("/proc/meminfo", "r");
  if (!file)
    return 0;
  char line[256];
  long value = 0;
  while (fgets(line, sizeof(line), file)) {
    if (sscanf(line, "MemTotal: %ld kB", &value) == 1)
      break;
  }
  fclose(file);
  return value > 0 ? value : 0;
}

static int ss_context_is_gles(void) {
  const char *version = (const char *)glGetString(GL_VERSION);
  return !version || strstr(version, "OpenGL ES") != NULL;
}

static void ss_set_context_attributes(int major, int minor, int depth,
                                      int stencil) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

/*
 * SOR4/Horizon Chase profile rule: choose from measured GLES and physical
 * memory, never from a device name. Individual variables remain engineering
 * overrides; defaults are only filled when the launcher did not set them.
 */
static void ss_apply_runtime_profile(void) {
  long mem_kb = ss_memtotal_kb();
  GLint gl_max_texture = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max_texture);

  int longest = SCREEN_WIDTH > SCREEN_HEIGHT ? SCREEN_WIDTH : SCREEN_HEIGHT;
  int full_cap = longest > 2048 ? longest : 2048;
  if (gl_max_texture > 0 && full_cap > gl_max_texture)
    full_cap = gl_max_texture;
  if (full_cap < 1280)
    full_cap = 1280;

  /* A compressao/conversao de textura e ditada pela MEMORIA fisica, nao
   * pela versao de GLES (um handheld ES3 de 640MB precisa do caminho ETC1
   * tanto quanto o Mali-450 de 1GB). A versao de GLES so define o teto. */
  const char *profile;
  if (g_es_major >= 3 && mem_kb >= 1700000) {
    profile = "gles3-high";
    char cap[32];
    snprintf(cap, sizeof(cap), "%d", full_cap);
    ss_env_default("SUMMERTIME_MAX_TEX", cap);
    ss_env_default("SUMMERTIME_TEXDS_MAX", cap);
    ss_env_default("SUMMERTIME_ETC1", "0");
    ss_env_default("SUMMERTIME_PREMUL_PATH", "0");
    ss_env_default("SUMMERTIME_TEX16", "0");
    ss_env_default("SUMMERTIME_GC_EVERY", "0");
  } else if (mem_kb > 0 && mem_kb < 700000) {
    /* 640MB (R36-clones): abaixo ate do Mali-450 de 1GB. Caps do caminho
     * validado do Mali (1024) + cache menor, senao a primeira cena da
     * historia leva o aparelho a espiral de swap. */
    profile = "ultra-lowmem";
    ss_env_default("SUMMERTIME_MAX_TEX", "1024");
    ss_env_default("SUMMERTIME_TEXDS_MAX", "640");
    ss_env_default("SUMMERTIME_ETC1", "1");
    ss_env_default("SUMMERTIME_PREMUL_PATH", "1");
    ss_env_default("SUMMERTIME_TEX16", "1");
    ss_env_default("SUMMERTIME_GC_EVERY", "6");
    ss_env_default("SUMMERTIME_CACHE_MB", "48");
    ss_env_default("SUMMERTIME_PREDICT", "4");
  } else if (mem_kb > 0 && mem_kb < 1250000) {
    profile = g_es_major >= 3 ? "gles3-lowmem" : "gles2-lowmem";
    ss_env_default("SUMMERTIME_MAX_TEX", "1280");
    ss_env_default("SUMMERTIME_TEXDS_MAX", "800");
    ss_env_default("SUMMERTIME_ETC1", "1");
    ss_env_default("SUMMERTIME_PREMUL_PATH", "1");
    ss_env_default("SUMMERTIME_TEX16", "1");
    ss_env_default("SUMMERTIME_GC_EVERY", "8");
  } else if (g_es_major >= 3) {
    profile = "gles3-mid";
    ss_env_default("SUMMERTIME_MAX_TEX", "1280");
    ss_env_default("SUMMERTIME_TEXDS_MAX", "768");
    ss_env_default("SUMMERTIME_ETC1", "0");
    ss_env_default("SUMMERTIME_PREMUL_PATH", "0");
    ss_env_default("SUMMERTIME_TEX16", "1");
    ss_env_default("SUMMERTIME_GC_EVERY", "8");
  } else {
    profile = "gles2";
    ss_env_default("SUMMERTIME_MAX_TEX", "1280");
    ss_env_default("SUMMERTIME_TEXDS_MAX", "800");
    ss_env_default("SUMMERTIME_ETC1", "1");
    ss_env_default("SUMMERTIME_PREMUL_PATH", "1");
    ss_env_default("SUMMERTIME_TEX16", "1");
    ss_env_default("SUMMERTIME_GC_EVERY", "8");
  }

  ss_env_default("SUMMERTIME_ETC1_MIN", "64");
  ss_env_default("SUMMERTIME_RENPY_CURSOR", "1");
  {
    int fb1_cursor =
        !ss_env_on("SUMMERTIME_NO_FB1") &&
        access("/dev/fb1", R_OK | W_OK) == 0 &&
        access("/sys/class/graphics/fb1/color_key", W_OK) == 0 &&
        access("/sys/class/graphics/fb1/enable_key", W_OK) == 0;
    ss_env_default("SUMMERTIME_CURSOR", fb1_cursor ? "0" : "1");
  }

  char actual[16];
  snprintf(actual, sizeof(actual), "%d.%d", g_es_major, g_es_minor);
  setenv("SUMMERTIME_GLVER", actual, 1);
  setenv("SUMMERTIME_PROFILE", profile, 1);

  debugPrintf(
      "profile: %s mem=%ldkB GLES=%s GL_MAX_TEXTURE_SIZE=%d "
      "maxtex=%s texds=%s etc1=%s tex16=%s premul=%s gc=%s cursor=%s\n",
      profile, mem_kb, actual, (int)gl_max_texture,
      getenv("SUMMERTIME_MAX_TEX"), getenv("SUMMERTIME_TEXDS_MAX"),
      getenv("SUMMERTIME_ETC1"), getenv("SUMMERTIME_TEX16"),
      getenv("SUMMERTIME_PREMUL_PATH"), getenv("SUMMERTIME_GC_EVERY"),
      getenv("SUMMERTIME_CURSOR"));
}

int egl_shim_create_window(void) {
  /* resolucao nativa do device (TV 1080p, handheld 480p...) c/ fallback 720p */
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    summertime_screen_w = dm.w; summertime_screen_h = dm.h;
    debugPrintf("egl_shim: desktop mode %dx%d\n", dm.w, dm.h);
  }
  { const char *e = getenv("SUMMERTIME_RES"); int w, h; /* override opcional */
    if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      summertime_screen_w = w; summertime_screen_h = h;
      debugPrintf("egl_shim: SUMMERTIME_RES override %dx%d\n", w, h);
    } }

  int versions[2] = {3, 2};
  int version_count = 2;
  const char *forced = getenv("SUMMERTIME_GLES_MAJOR");
  if (!forced || !*forced)
    forced = getenv("SUMMERTIME_GLVER");
  if (forced && (forced[0] == '2' || forced[0] == '3')) {
    versions[0] = forced[0] - '0';
    version_count = 1;
  }
  static const struct {
    int depth;
    int stencil;
  } formats[] = {{24, 8}, {16, 0}, {0, 0}};

  Uint32 fullscreen = ss_env_on("SUMMERTIME_EXCLUSIVE_FULLSCREEN")
                          ? SDL_WINDOW_FULLSCREEN
                          : SDL_WINDOW_FULLSCREEN_DESKTOP;
  for (size_t f = 0;
       f < sizeof(formats) / sizeof(formats[0]) && !egl_share_root; f++) {
    ss_set_context_attributes(versions[0], 0, formats[f].depth,
                              formats[f].stencil);
    egl_window = SDL_CreateWindow(
        PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_OPENGL | fullscreen);
    if (!egl_window) {
      debugPrintf("egl_shim: window depth%d/stencil%d FAILED: %s\n",
                  formats[f].depth, formats[f].stencil, SDL_GetError());
      continue;
    }

    for (int v = 0; v < version_count && !egl_share_root; v++) {
      ss_set_context_attributes(versions[v], 0, formats[f].depth,
                                formats[f].stencil);
      egl_share_root = gl_createcontext(egl_window);
      if (egl_share_root && !ss_env_on("SUMMERTIME_ALLOW_DESKTOP_GL") &&
          !ss_context_is_gles()) {
        debugPrintf("egl_shim: ES%d returned desktop OpenGL; rejected\n",
                    versions[v]);
        SDL_GL_DeleteContext(egl_share_root);
        egl_share_root = NULL;
      }
      if (egl_share_root) {
        g_es_major = versions[v];
        g_es_minor = 0;
        g_depth_size = formats[f].depth;
        g_stencil_size = formats[f].stencil;
      } else {
        debugPrintf(
            "egl_shim: ES%d depth%d/stencil%d context FAILED: %s\n",
            versions[v], formats[f].depth, formats[f].stencil,
            SDL_GetError());
      }
    }
    if (!egl_share_root) {
      SDL_DestroyWindow(egl_window);
      egl_window = NULL;
    }
  }
  if (!egl_window || !egl_share_root) {
    debugPrintf("egl_shim: no EGL/GLES video combination succeeded\n");
    return -1;
  }

  {
    int drawable_w = 0, drawable_h = 0;
    SDL_GL_GetDrawableSize(egl_window, &drawable_w, &drawable_h);
    if (drawable_w > 0 && drawable_h > 0) {
      summertime_screen_w = drawable_w;
      summertime_screen_h = drawable_h;
    }
  }

  const char *driver = SDL_GetCurrentVideoDriver();
  const char *vendor = (const char *)glGetString(GL_VENDOR);
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  const char *version = (const char *)glGetString(GL_VERSION);
  const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  debugPrintf(
      "egl_shim: backend=%s context=ES%d.%d depth=%d stencil=%d "
      "drawable=%dx%d\n",
      driver ? driver : "?", g_es_major, g_es_minor, g_depth_size,
      g_stencil_size, SCREEN_WIDTH, SCREEN_HEIGHT);
  debugPrintf("egl_shim: GL_VENDOR=%s\n", vendor ? vendor : "?");
  debugPrintf("egl_shim: GL_RENDERER=%s\n", renderer ? renderer : "?");
  debugPrintf("egl_shim: GL_VERSION=%s\n", version ? version : "?");
  debugPrintf("egl_shim: GL_GLSL=%s\n", glsl ? glsl : "?");

  ss_apply_runtime_profile();

  /* SUMMERTIME_SWAPINT no contexto novo (a engine pode nunca chamar
   * eglSwapInterval; default SDL=vsync 1 + limiter da engine = 30fps). */
  {
    const char *f = getenv("SUMMERTIME_SWAPINT");
    if (f) {
      SDL_GL_SetSwapInterval(atoi(f));
      debugPrintf("egl_shim: swap interval forçado=%d\n", atoi(f));
    }
  }

  gl_makecurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
  return 0;
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
    /* SUMMERTIME_SWAPINT: intervalo é estado por-contexto; aplica 1x em cada */
    {
      static const char *si = (const char *)-1;
      if (si == (const char *)-1) si = getenv("SUMMERTIME_SWAPINT");
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

/* --- Seta de verdade: quad texturizado com alpha premultiplicado ---------
 * A textura anti-aliased (contorno preto, nucleo branco, sombra suave) vem
 * de cursor_arrow.h (tools/gen_cursor_arrow.py). Hover = tinta vermelha via
 * uniform (o Ren'Py publica o foco em /dev/shm/summertime_hover). Todo o
 * estado GL tocado e salvo/restaurado: a engine nao percebe o overlay. */
#include "cursor_arrow.h"

static GLuint cur_prog = 0, cur_tex = 0;
static GLint cur_u_tint = -1;
static int cur_gl_failed = 0;

static GLuint cursor_compile(GLenum type, const char *src) {
  GLuint sh = glCreateShader(type);
  if (!sh)
    return 0;
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256] = {0};
    glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
    debugPrintf("cursor: shader FAILED: %s\n", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static int cursor_gl_init(void) {
  static const char *vs_src =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  static const char *fs_src =
      "precision mediump float;\n"
      "uniform sampler2D u_tex;\n"
      "uniform vec3 u_tint;\n"
      "varying vec2 v_uv;\n"
      "void main(){ vec4 c = texture2D(u_tex, v_uv);\n"
      "  gl_FragColor = vec4(c.rgb * u_tint, c.a); }\n";

  GLuint vs = cursor_compile(GL_VERTEX_SHADER, vs_src);
  GLuint fs = cursor_compile(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return -1;
  }
  cur_prog = glCreateProgram();
  glBindAttribLocation(cur_prog, 0, "a_pos");
  glBindAttribLocation(cur_prog, 1, "a_uv");
  glAttachShader(cur_prog, vs);
  glAttachShader(cur_prog, fs);
  glLinkProgram(cur_prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(cur_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(cur_prog);
    cur_prog = 0;
    return -1;
  }
  cur_u_tint = glGetUniformLocation(cur_prog, "u_tint");
  GLint u_tex = glGetUniformLocation(cur_prog, "u_tex");
  GLint prev_prog = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glUseProgram(cur_prog);
  glUniform1i(u_tex, 0);
  glUseProgram((GLuint)prev_prog);

  glGenTextures(1, &cur_tex);
  GLint prev_tex = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glBindTexture(GL_TEXTURE_2D, cur_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SS_ARROW_TEX_SIZE,
               SS_ARROW_TEX_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               summertime_arrow_tex);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  debugPrintf("cursor: seta texturizada pronta (tex=%u prog=%u)\n",
              cur_tex, cur_prog);
  return 0;
}

/* hover publicado pelo core.py; le a cada 8 frames (barato) */
static int cursor_hover_state(void) {
  static int frames = 0, hover = 0;
  if (++frames % 8 == 0) {
    int fd = open("/dev/shm/summertime_hover", O_RDONLY);
    if (fd >= 0) {
      char c = '0';
      if (read(fd, &c, 1) == 1)
        hover = (c == '1');
      close(fd);
    } else {
      hover = 0;
    }
  }
  return hover;
}

typedef struct {
  GLint enabled, size, type, normalized, stride, buffer;
  void *pointer;
} _attrib_state;

static void cursor_save_attrib(GLuint i, _attrib_state *s) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s->size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s->type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s->normalized);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s->stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s->buffer);
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s->pointer);
}

static void cursor_restore_attrib(GLuint i, const _attrib_state *s) {
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->buffer);
  if (s->size)
    glVertexAttribPointer(i, s->size, (GLenum)s->type,
                          (GLboolean)s->normalized, s->stride, s->pointer);
  if (s->enabled)
    glEnableVertexAttribArray(i);
  else
    glDisableVertexAttribArray(i);
}

static void draw_port_cursor(void) {
  const char *cursor = getenv("SUMMERTIME_CURSOR");
  if (cursor && strcmp(cursor, "0") == 0)
    return;
  if (cur_gl_failed)
    return;
  if (!cur_prog && cursor_gl_init() != 0) {
    cur_gl_failed = 1;
    return;
  }

  float cx = summertime_cursor_x * (float)SCREEN_WIDTH;
  float cy = summertime_cursor_y * (float)SCREEN_HEIGHT;

  /* altura da seta proporcional a tela (~5% da altura), mipmap faz o resto */
  float h_px = (float)SCREEN_HEIGHT * 0.052f;
  if (h_px < 24.0f) h_px = 24.0f;
  if (h_px > 96.0f) h_px = 96.0f;
  float scale = h_px / SS_ARROW_DESIGN_H;
  float quad = (float)SS_ARROW_TEX_SIZE * scale;
  float qx = cx - SS_ARROW_TIP_X * scale;
  float qy = cy - SS_ARROW_TIP_Y * scale;

  float x0 = 2.0f * qx / (float)SCREEN_WIDTH - 1.0f;
  float x1 = 2.0f * (qx + quad) / (float)SCREEN_WIDTH - 1.0f;
  float y0 = 1.0f - 2.0f * qy / (float)SCREEN_HEIGHT;
  float y1 = 1.0f - 2.0f * (qy + quad) / (float)SCREEN_HEIGHT;
  const float verts[16] = {
      x0, y0, 0.0f, 0.0f,
      x1, y0, 1.0f, 0.0f,
      x0, y1, 0.0f, 1.0f,
      x1, y1, 1.0f, 1.0f,
  };

  /* ------ salvar estado ------ */
  GLint prev_prog = 0, prev_active = 0, prev_tex = 0, prev_ab = 0;
  GLint prev_vp[4] = {0, 0, 0, 0};
  GLint b_srgb = 0, b_drgb = 0, b_sa = 0, b_da = 0, b_eq_rgb = 0, b_eq_a = 0;
  GLboolean prev_cmask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  GLboolean en_blend = glIsEnabled(GL_BLEND);
  GLboolean en_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean en_cull = glIsEnabled(GL_CULL_FACE);
  GLboolean en_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean en_stencil = glIsEnabled(GL_STENCIL_TEST);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_ab);
  glGetIntegerv(GL_VIEWPORT, prev_vp);
  glGetIntegerv(GL_BLEND_SRC_RGB, &b_srgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &b_drgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &b_sa);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &b_da);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &b_eq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &b_eq_a);
  glGetBooleanv(GL_COLOR_WRITEMASK, prev_cmask);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  _attrib_state a0, a1;
  cursor_save_attrib(0, &a0);
  cursor_save_attrib(1, &a1);

  /* ------ desenhar ------ */
  glUseProgram(cur_prog);
  glBindTexture(GL_TEXTURE_2D, cur_tex);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  if (cursor_hover_state())
    glUniform3f(cur_u_tint, 1.0f, 0.15f, 0.15f);
  else
    glUniform3f(cur_u_tint, 1.0f, 1.0f, 1.0f);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, verts);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  /* ------ restaurar estado ------ */
  cursor_restore_attrib(0, &a0);
  cursor_restore_attrib(1, &a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_ab);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glActiveTexture((GLenum)prev_active);
  glUseProgram((GLuint)prev_prog);
  glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
  glBlendEquationSeparate((GLenum)b_eq_rgb, (GLenum)b_eq_a);
  glBlendFuncSeparate((GLenum)b_srgb, (GLenum)b_drgb,
                      (GLenum)b_sa, (GLenum)b_da);
  glColorMask(prev_cmask[0], prev_cmask[1], prev_cmask[2], prev_cmask[3]);
  if (!en_blend) glDisable(GL_BLEND);
  if (en_depth) glEnable(GL_DEPTH_TEST);
  if (en_cull) glEnable(GL_CULL_FACE);
  if (en_scissor) glEnable(GL_SCISSOR_TEST);
  if (en_stencil) glEnable(GL_STENCIL_TEST);
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    if (getenv("SUMMERTIME_GLDBG"))
      summertime_gl_debug_frame();
    draw_port_cursor();
    dys_maybe_screenshot();
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
        if (getenv("SUMMERTIME_PERF") && sum >= 5000) {
          fprintf(stderr, "[PERF] fps=%.1f avg=%.1fms max=%.0fms >20ms=%u >40ms=%u\n",
                  n * 1000.0 / sum, sum / n, mx, s20, s40);
          sum = 0; n = 0; mx = 0; s20 = 0; s40 = 0;
        } else if (sum >= 5000) {
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
  case 0x3020: *value = 24; break;
  case 0x3021: *value = 0; break;
  case 0x3022: *value = 8; break;
  case 0x3023: *value = 8; break;
  case 0x3024: *value = 8; break;
  case 0x3025: *value = g_depth_size; break;
  case 0x3026: *value = g_stencil_size; break;
  case 0x3027: *value = 0x3038; break;
  case 0x3028: *value = 1; break;
  case 0x3033: *value = 0x0005; break;
  case 0x3040:
  case 0x3042:
    *value = g_es_major >= 3 ? 0x0040 : 0x0004;
    break;
  case 0x3039: *value = 0x308E; break;
  case 0x3032: *value = 0; break;
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = summertime_gl_lookup(procname);
  if (ptr) return ptr;

  ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = summertime_gl_lookup(stripped);
      if (ptr) return ptr;
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  if (getenv("SUMMERTIME_VERBOSE"))
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
  /* SUMMERTIME_SWAPINT força o intervalo (teste do double-pacing: engine dorme
   * ~16ms + vsync = 2 períodos = trava em 30fps; =0 deixa a engine ditar). */
  const char *f = getenv("SUMMERTIME_SWAPINT");
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
