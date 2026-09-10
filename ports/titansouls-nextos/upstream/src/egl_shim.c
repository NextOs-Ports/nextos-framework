#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "Titan Souls"
#endif
/*
 * egl_shim.c -- Android EGL facade over the nxgl-v2 SDL/EGL stack.
 *
 * nxgl owns the real window and its GLES2 share-root.  Guest EGL contexts are
 * tracked SDL child contexts in that same share group; the engine remains the
 * sole owner of presentation through eglSwapBuffers.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "egl_shim.h"
#include "nxgl.h"
#include "runtime_hooks.h"
#include "util.h"

/* Resolucao DINAMICA (qualquer device): desktop mode do SDL com fallback
 * 1280x720. Exportada p/ imports.c (ANativeWindow_getWidth/Height — o que o
 * JOGO le) e android_shim.c (clamp do cursor). */
int ts_screen_w = 1280, ts_screen_h = 720;
#define SCREEN_WIDTH ts_screen_w
#define SCREEN_HEIGHT ts_screen_h

/* ARM32 (bionic armeabi-v7a): a stack-canary NAO vem de TLS como no arm64 —
 * a engine importa o objeto global __stack_chk_guard, que o loader fornece.
 * Logo aqui nao existe o salva/restaura de tpidr do lineage AArch64. */
static int gl_makecurrent(SDL_Window *w, SDL_GLContext c) {
  return SDL_GL_MakeCurrent(w, c);
}
static SDL_GLContext gl_createcontext(SDL_Window *w) {
  return SDL_GL_CreateContext(w);
}

typedef enum ts_egl_surface_kind {
  TS_EGL_SURFACE_WINDOW = 1,
  TS_EGL_SURFACE_PBUFFER = 2
} ts_egl_surface_kind;

typedef struct _egl_surface {
  ts_egl_surface_kind kind;
  int width;
  int height;
  unsigned binding_count;
  struct _egl_surface *next;
} _egl_surface;

typedef struct _egl_context {
  SDL_GLContext sdl_context;
  int swapint_applied;
  int id;
  int owner_valid;
  pthread_t owner;
  struct _egl_context *next;
} _egl_context;

static const unsigned char egl_display_token = 0xd1u;
static const unsigned char egl_config_token = 0xc1u;

static nxgl_context *egl_nxgl = NULL;
static nxgl_report_v2 egl_nxgl_report;
static int egl_nxgl_report_valid = 0;
static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static _egl_context *egl_contexts = NULL;
static _egl_surface *egl_surfaces = NULL;
static int egl_initialized = 0;
static int egl_terminating = 0;
static int graphics_ready_attempted = 0;
static int graphics_ready_status = -1;
static int frame_count = 0;
static int next_context_id = 1;

static _Thread_local _egl_context *tls_context = NULL;
static _Thread_local _egl_surface *tls_draw_surface = NULL;
static _Thread_local _egl_surface *tls_read_surface = NULL;
static _Thread_local EGLint tls_egl_error = EGL_SUCCESS;

static void egl_set_error(EGLint error) {
  tls_egl_error = error;
}

static EGLBoolean egl_fail(EGLint error) {
  egl_set_error(error);
  return EGL_FALSE;
}

static int egl_display_valid(EGLDisplay display) {
  return display == (EGLDisplay)(uintptr_t)&egl_display_token && egl_nxgl &&
         egl_window && !egl_terminating;
}

static int egl_config_valid(EGLConfig config) {
  return config == (EGLConfig)(uintptr_t)&egl_config_token;
}

/* Callers hold egl_state_mutex.  Comparing handles before dereferencing them
 * keeps stale/foreign guest pointers from becoming host memory accesses. */
static _egl_context *egl_find_context(EGLContext handle) {
  _egl_context *context;
  for (context = egl_contexts; context; context = context->next)
    if ((EGLContext)context == handle)
      return context;
  return NULL;
}

static _egl_surface *egl_find_surface(EGLSurface handle) {
  _egl_surface *surface;
  for (surface = egl_surfaces; surface; surface = surface->next)
    if ((EGLSurface)surface == handle)
      return surface;
  return NULL;
}

static void egl_release_tls_binding_locked(void) {
  if (tls_draw_surface && tls_draw_surface->binding_count)
    --tls_draw_surface->binding_count;
  if (tls_read_surface && tls_read_surface->binding_count)
    --tls_read_surface->binding_count;
  if (tls_context) {
    tls_context->owner_valid = 0;
    memset(&tls_context->owner, 0, sizeof(tls_context->owner));
  }
  tls_context = NULL;
  tls_draw_surface = NULL;
  tls_read_surface = NULL;
}

static void egl_status(void *userdata, nxgl_status_kind kind,
                       const char *message) {
  static const char *const labels[] = {
      "info", "attempt", "selected", "warning", "error",
  };
  const char *label = "unknown";
  (void)userdata;
  if ((unsigned)kind < sizeof(labels) / sizeof(labels[0]))
    label = labels[kind];
  fprintf(stderr, "[egl_shim/nxgl:%s] %s\n", label,
          message ? message : "(sem diagnostico)");
}

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_create_window(void) {
  static const nxgl_config_candidate candidates[] = {
      {2, 0, 8, 8, 8, 8, 24, 8, 1},
      {2, 0, 8, 8, 8, 8, 16, 0, 1},
  };
  nxgl_engine_requirements requirements;
  nxgl_open_options_v2 options;
  nxgl_context *opened = NULL;
  nxgl_report_v2 report;
  int requested_width = ts_screen_w;
  int requested_height = ts_screen_h;
  int requested_override_valid = 0;
  int status;

  if (egl_nxgl || egl_window) {
    fprintf(stderr, "[egl_shim] nxgl-v2 ja foi aberto\n");
    return;
  }

  /* Mantem o override logico exclusivamente para os A/B de bancada.  Nao
   * consulte SDL video aqui: nxgl ainda vai inicializa-lo e possui toda a
   * negociacao da resolucao fisica/janela. */
  { const char *e = getenv("TS_RES"); int w, h; /* override opcional */
    if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      ts_screen_w = w; ts_screen_h = h;
      requested_override_valid = 1;
      debugPrintf("egl_shim: TS_RES override %dx%d\n", w, h);
    } }
  requested_width = ts_screen_w;
  requested_height = ts_screen_h;

  nxgl_engine_requirements_init(&requirements);
  requirements.minimum_gles_major = 2;
  requirements.minimum_gles_minor = 0;
  requirements.minimum_red_bits = 8;
  requirements.minimum_green_bits = 8;
  requirements.minimum_blue_bits = 8;
  requirements.minimum_alpha_bits = 8;
  requirements.minimum_depth_bits = 16;
  requirements.minimum_stencil_bits = 0;
  requirements.require_double_buffer = 0;

  nxgl_open_options_v2_init(&options);
  options.window_title = PORT_WINDOW_TITLE;
  options.window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                         SDL_WINDOW_FULLSCREEN;
  options.requirements = &requirements;
  options.candidates = candidates;
  options.candidate_count = sizeof(candidates) / sizeof(candidates[0]);
  options.status = egl_status;
  options.status_userdata = NULL;

  memset(&report, 0, sizeof(report));
  status = nxgl_open_v2(&options, &opened, &report);
  if (status != NXGL_SUCCESS) {
    fprintf(stderr,
            "[egl_shim] nxgl_open_v2 falhou: status=%d stage=%d reason=%d\n",
            status, (int)report.final_stage, (int)report.final_reason);
    if (opened) {
      int close_status = nxgl_close_v2(opened);
      fprintf(stderr, "[egl_shim] limpeza nxgl apos falha: %d\n",
              close_status);
    }
    return;
  }
  if (report.api_version != NXGL_API_VERSION_V2 ||
      report.final_stage != NXGL_OPEN_STAGE_V2_CURRENT_VALIDATE ||
      report.final_reason != NXGL_OPEN_REASON_V2_SELECTED ||
      report.stack_owner != NXGL_STACK_OWNER_V2_SDL_EGL ||
      !report.handles.sdl_window || !report.handles.sdl_context ||
      report.handles.native_window == 0u || report.egl.observed != 1 ||
      report.legacy.actual.gles_major < 2 ||
      report.legacy.actual.alpha_bits < 8 ||
      report.legacy.actual.depth_bits < 16 ||
      report.legacy.drawable_width <= 0 ||
      report.legacy.drawable_height <= 0) {
    fprintf(stderr, "[egl_shim] nxgl-v2 entregou report fora do contrato\n");
    (void)nxgl_close_v2(opened);
    return;
  }

  egl_nxgl = opened;
  egl_nxgl_report = report;
  egl_nxgl_report_valid = 1;
  egl_window = report.handles.sdl_window;
  egl_share_root = report.handles.sdl_context;

  fprintf(stderr,
          "[egl_shim] nxgl-v2 SDL_EGL %s candidate=%zu GLES%d.%d "
          "R%dG%dB%dA%d d%d s%d double=%d config=%d\n",
          report.legacy.video_backend,
          report.legacy.selected_candidate_index,
          report.legacy.actual.gles_major, report.legacy.actual.gles_minor,
          report.legacy.actual.red_bits, report.legacy.actual.green_bits,
          report.legacy.actual.blue_bits, report.legacy.actual.alpha_bits,
          report.legacy.actual.depth_bits, report.legacy.actual.stencil_bits,
          report.legacy.actual.double_buffer, report.egl.config_id);

  /* O drawable provado por nxgl e' a autoridade. TS_KEEP_REQUESTED_RES
   * continua somente como gate dos A/B fisicos em andamento. */
  if (requested_override_valid && getenv("TS_KEEP_REQUESTED_RES") &&
      (requested_width != report.legacy.drawable_width ||
       requested_height != report.legacy.drawable_height)) {
      /* Bench-only diagnostic: let the Android guest see the requested
       * logical size even when KMS fixes the physical drawable.  This is
       * useful for reproducing pixelScale=2 on a 480p test panel; it must
       * never be enabled by the release launcher. */
    fprintf(stderr,
            "[egl_shim] bench logical drawable %dx%d over physical %dx%d\n",
            requested_width, requested_height,
            report.legacy.drawable_width, report.legacy.drawable_height);
    ts_screen_w = requested_width;
    ts_screen_h = requested_height;
  } else {
    ts_screen_w = report.legacy.drawable_width;
    ts_screen_h = report.legacy.drawable_height;
    fprintf(stderr, "[egl_shim] drawable nxgl real %dx%d adotado\n",
            ts_screen_w, ts_screen_h);
  }

  /* open_v2 termina com o root current. Ele e' nxgl-owned e nunca e' exposto
   * ao guest; solte-o antes que qualquer child seja criado. */
  if (gl_makecurrent(egl_window, NULL) != 0) {
    fprintf(stderr, "[egl_shim] falha ao soltar share-root nxgl: %s\n",
            SDL_GetError());
    egl_window = NULL;
    egl_share_root = NULL;
    egl_nxgl_report_valid = 0;
    (void)nxgl_close_v2(egl_nxgl);
    egl_nxgl = NULL;
    return;
  }
  debugPrintf("egl_shim: nxgl share-root released, ready for guest children\n");
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  _egl_context *context = tls_context;
  SDL_GLContext sdl_context;
  if (!egl_window || !context)
    return 0;
  pthread_mutex_lock(&egl_state_mutex);
  if (egl_find_context((EGLContext)context) != context ||
      !context->owner_valid ||
      !pthread_equal(context->owner, pthread_self())) {
    pthread_mutex_unlock(&egl_state_mutex);
    return 0;
  }
  sdl_context = context->sdl_context;
  pthread_mutex_unlock(&egl_state_mutex);
  if (SDL_GL_GetCurrentWindow() == egl_window &&
      SDL_GL_GetCurrentContext() == sdl_context)
    return 1;
  if (gl_makecurrent(egl_window, sdl_context) == 0) {
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), context->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), context->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

static int egl_config_attribute(EGLint attribute, EGLint *value) {
  const nxgl_config_actual *actual;
  if (!value || !egl_nxgl_report_valid)
    return -1;
  actual = &egl_nxgl_report.legacy.actual;
  switch (attribute) {
  case EGL_BUFFER_SIZE:
    *value = actual->red_bits + actual->green_bits + actual->blue_bits +
             actual->alpha_bits;
    return 0;
  case EGL_ALPHA_SIZE: *value = actual->alpha_bits; return 0;
  case EGL_BLUE_SIZE: *value = actual->blue_bits; return 0;
  case EGL_GREEN_SIZE: *value = actual->green_bits; return 0;
  case EGL_RED_SIZE: *value = actual->red_bits; return 0;
  case EGL_DEPTH_SIZE: *value = actual->depth_bits; return 0;
  case EGL_STENCIL_SIZE: *value = actual->stencil_bits; return 0;
  case EGL_CONFIG_CAVEAT: *value = EGL_NONE; return 0;
  case EGL_CONFIG_ID: *value = egl_nxgl_report.egl.config_id; return 0;
  case EGL_SURFACE_TYPE:
    *value = egl_nxgl_report.egl.surface_type;
    return 0;
  case EGL_RENDERABLE_TYPE:
    *value = egl_nxgl_report.egl.renderable_type;
    return 0;
  case EGL_LEVEL: *value = 0; return 0;
  case EGL_MAX_PBUFFER_HEIGHT: *value = 4096; return 0;
  case EGL_MAX_PBUFFER_PIXELS: *value = 4096 * 4096; return 0;
  case EGL_MAX_PBUFFER_WIDTH: *value = 4096; return 0;
  case EGL_SAMPLE_BUFFERS: *value = 0; return 0;
  case EGL_SAMPLES: *value = 0; return 0;
  case EGL_NATIVE_RENDERABLE: *value = EGL_FALSE; return 0;
  case EGL_NATIVE_VISUAL_ID: *value = 0; return 0;
  case EGL_NATIVE_VISUAL_TYPE: *value = 0; return 0;
  case EGL_TRANSPARENT_TYPE: *value = EGL_NONE; return 0;
  case EGL_TRANSPARENT_BLUE_VALUE: *value = 0; return 0;
  case EGL_TRANSPARENT_GREEN_VALUE: *value = 0; return 0;
  case EGL_TRANSPARENT_RED_VALUE: *value = 0; return 0;
  case EGL_BIND_TO_TEXTURE_RGB: *value = EGL_FALSE; return 0;
  case EGL_BIND_TO_TEXTURE_RGBA: *value = EGL_FALSE; return 0;
  case EGL_MIN_SWAP_INTERVAL: *value = 0; return 0;
  case EGL_MAX_SWAP_INTERVAL: *value = 1; return 0;
  case EGL_LUMINANCE_SIZE: *value = 0; return 0;
  case EGL_ALPHA_MASK_SIZE: *value = 0; return 0;
  case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; return 0;
  case EGL_CONFORMANT: *value = egl_nxgl_report.egl.renderable_type; return 0;
  default: return -1;
  }
}

/* Returns 1 when the sole delivered config matches, 0 when it does not and
 * -1 for a malformed/unsupported attribute list. */
static int egl_config_matches(const EGLint *attributes) {
  size_t pair;
  if (!attributes)
    return 1;
  for (pair = 0u; pair < 64u; ++pair) {
    EGLint attribute = attributes[pair * 2u];
    EGLint requested;
    EGLint actual;
    if (attribute == EGL_NONE)
      return 1;
    requested = attributes[pair * 2u + 1u];
    if (egl_config_attribute(attribute, &actual) != 0)
      return -1;
    if (requested == EGL_DONT_CARE)
      continue;
    switch (attribute) {
    case EGL_BUFFER_SIZE:
    case EGL_ALPHA_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_RED_SIZE:
    case EGL_DEPTH_SIZE:
    case EGL_STENCIL_SIZE:
    case EGL_SAMPLE_BUFFERS:
    case EGL_SAMPLES:
      if (actual < requested)
        return 0;
      break;
    case EGL_SURFACE_TYPE:
    case EGL_RENDERABLE_TYPE:
      if ((actual & requested) != requested)
        return 0;
      break;
    default:
      if (actual != requested)
        return 0;
      break;
    }
  }
  return -1;
}

static int egl_context_attributes_valid(const EGLint *attributes) {
  size_t pair;
  int version = 1;
  if (!attributes)
    return 0;
  for (pair = 0u; pair < 16u; ++pair) {
    EGLint attribute = attributes[pair * 2u];
    if (attribute == EGL_NONE)
      return version == 2 ? 0 : -1;
    if (attribute != EGL_CONTEXT_CLIENT_VERSION)
      return -1;
    version = attributes[pair * 2u + 1u];
  }
  return -1;
}

static _egl_surface *egl_allocate_surface(ts_egl_surface_kind kind,
                                          int width, int height) {
  _egl_surface *surface = (_egl_surface *)calloc(1, sizeof(*surface));
  if (!surface)
    return NULL;
  surface->kind = kind;
  surface->width = width;
  surface->height = height;
  pthread_mutex_lock(&egl_state_mutex);
  surface->next = egl_surfaces;
  egl_surfaces = surface;
  pthread_mutex_unlock(&egl_state_mutex);
  return surface;
}

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  if (!egl_nxgl || !egl_window || egl_terminating) {
    egl_set_error(EGL_BAD_DISPLAY);
    return EGL_NO_DISPLAY;
  }
  return (EGLDisplay)(uintptr_t)&egl_display_token;
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  pthread_mutex_lock(&egl_state_mutex);
  if (!egl_display_valid(dpy)) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_DISPLAY);
  }
  egl_initialized = 1;
  pthread_mutex_unlock(&egl_state_mutex);
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  _egl_context *context;
  _egl_surface *surface;
  nxgl_context *root;
  int status;
  debugPrintf("egl_shim: eglTerminate()\n");

  pthread_mutex_lock(&egl_state_mutex);
  if (dpy != (EGLDisplay)(uintptr_t)&egl_display_token || !egl_nxgl ||
      egl_terminating) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_DISPLAY);
  }
  if (tls_context) {
    if (gl_makecurrent(egl_window, NULL) != 0) {
      pthread_mutex_unlock(&egl_state_mutex);
      return egl_fail(EGL_BAD_ACCESS);
    }
    egl_release_tls_binding_locked();
  }
  for (context = egl_contexts; context; context = context->next) {
    if (context->owner_valid) {
      pthread_mutex_unlock(&egl_state_mutex);
      return egl_fail(EGL_BAD_ACCESS);
    }
  }

  /* Adapter children are always torn down before the nxgl-owned root/window.
   * The normal Mali terminal still uses _exit; this is the controlled EGL
   * shutdown path only. */
  while (egl_contexts) {
    context = egl_contexts;
    egl_contexts = context->next;
    if (context->sdl_context)
      SDL_GL_DeleteContext(context->sdl_context);
    free(context);
  }
  while (egl_surfaces) {
    surface = egl_surfaces;
    egl_surfaces = surface->next;
    free(surface);
  }
  egl_initialized = 0;
  egl_terminating = 1;
  root = egl_nxgl;
  pthread_mutex_unlock(&egl_state_mutex);

  status = nxgl_close_v2(root);
  pthread_mutex_lock(&egl_state_mutex);
  egl_terminating = 0;
  if (status == NXGL_SUCCESS) {
    egl_nxgl = NULL;
    egl_window = NULL;
    egl_share_root = NULL;
    egl_nxgl_report_valid = 0;
    memset(&egl_nxgl_report, 0, sizeof(egl_nxgl_report));
  }
  pthread_mutex_unlock(&egl_state_mutex);
  if (status != NXGL_SUCCESS) {
    fprintf(stderr, "[egl_shim] nxgl_close_v2 falhou: %d\n", status);
    return egl_fail(EGL_BAD_ACCESS);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  int matches;
  debugPrintf("egl_shim: eglChooseConfig()\n");
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  if (!egl_initialized)
    return egl_fail(EGL_NOT_INITIALIZED);
  if (!num_config || config_size < 0)
    return egl_fail(EGL_BAD_PARAMETER);
  matches = egl_config_matches(attrib_list);
  if (matches < 0)
    return egl_fail(EGL_BAD_ATTRIBUTE);
  *num_config = matches;
  if (matches && configs && config_size > 0)
    configs[0] = (EGLConfig)(uintptr_t)&egl_config_token;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  _egl_surface *surface;
  if (!egl_display_valid(dpy)) {
    egl_set_error(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  if (!egl_initialized) {
    egl_set_error(EGL_NOT_INITIALIZED);
    return EGL_NO_SURFACE;
  }
  if (!egl_config_valid(config)) {
    egl_set_error(EGL_BAD_CONFIG);
    return EGL_NO_SURFACE;
  }
  if (!win) {
    egl_set_error(EGL_BAD_NATIVE_WINDOW);
    return EGL_NO_SURFACE;
  }
  if (attrib_list && attrib_list[0] != EGL_NONE) {
    egl_set_error(EGL_BAD_ATTRIBUTE);
    return EGL_NO_SURFACE;
  }
  surface = egl_allocate_surface(TS_EGL_SURFACE_WINDOW,
                                 egl_nxgl_report.legacy.drawable_width,
                                 egl_nxgl_report.legacy.drawable_height);
  if (!surface) {
    egl_set_error(EGL_BAD_ALLOC);
    return EGL_NO_SURFACE;
  }
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", surface);
  return (EGLSurface)surface;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  _egl_surface *surface;
  int width = 0;
  int height = 0;
  size_t pair;
  if (!egl_display_valid(dpy)) {
    egl_set_error(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  if (!egl_initialized) {
    egl_set_error(EGL_NOT_INITIALIZED);
    return EGL_NO_SURFACE;
  }
  if (!egl_config_valid(config)) {
    egl_set_error(EGL_BAD_CONFIG);
    return EGL_NO_SURFACE;
  }
  if (!attrib_list) {
    egl_set_error(EGL_BAD_ATTRIBUTE);
    return EGL_NO_SURFACE;
  }
  for (pair = 0u; pair < 16u; ++pair) {
    EGLint attribute = attrib_list[pair * 2u];
    if (attribute == EGL_NONE)
      break;
    if (attribute == EGL_WIDTH)
      width = attrib_list[pair * 2u + 1u];
    else if (attribute == EGL_HEIGHT)
      height = attrib_list[pair * 2u + 1u];
    else {
      egl_set_error(EGL_BAD_ATTRIBUTE);
      return EGL_NO_SURFACE;
    }
  }
  if (pair == 16u || width <= 0 || height <= 0) {
    egl_set_error(EGL_BAD_ATTRIBUTE);
    return EGL_NO_SURFACE;
  }
  surface = egl_allocate_surface(TS_EGL_SURFACE_PBUFFER, width, height);
  if (!surface) {
    egl_set_error(EGL_BAD_ALLOC);
    return EGL_NO_SURFACE;
  }
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", surface);
  return (EGLSurface)surface;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  SDL_Window *previous_window;
  SDL_GLContext previous_context;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  int restored;

  if (!c) {
    egl_set_error(EGL_BAD_ALLOC);
    return EGL_NO_CONTEXT;
  }
  if (!egl_display_valid(dpy)) {
    free(c);
    egl_set_error(EGL_BAD_DISPLAY);
    return EGL_NO_CONTEXT;
  }
  if (!egl_initialized) {
    free(c);
    egl_set_error(EGL_NOT_INITIALIZED);
    return EGL_NO_CONTEXT;
  }
  if (!egl_config_valid(config)) {
    free(c);
    egl_set_error(EGL_BAD_CONFIG);
    return EGL_NO_CONTEXT;
  }
  if (egl_context_attributes_valid(attrib_list) != 0) {
    free(c);
    egl_set_error(EGL_BAD_ATTRIBUTE);
    return EGL_NO_CONTEXT;
  }

  pthread_mutex_lock(&egl_state_mutex);
  if ((share_context && !egl_find_context(share_context)) ||
      !egl_share_root || !egl_window || egl_terminating) {
    pthread_mutex_unlock(&egl_state_mutex);
    free(c);
    egl_set_error(share_context ? EGL_BAD_CONTEXT : EGL_NOT_INITIALIZED);
    return EGL_NO_CONTEXT;
  }
  previous_window = SDL_GL_GetCurrentWindow();
  previous_context = SDL_GL_GetCurrentContext();
  if (gl_makecurrent(egl_window, egl_share_root) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1) != 0) {
    (void)SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (previous_context && previous_window)
      (void)gl_makecurrent(previous_window, previous_context);
    else
      (void)gl_makecurrent(egl_window, NULL);
    pthread_mutex_unlock(&egl_state_mutex);
    free(c);
    egl_set_error(EGL_BAD_ACCESS);
    return EGL_NO_CONTEXT;
  }
  c->sdl_context = gl_createcontext(egl_window);
  (void)SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  restored = previous_context && previous_window
                 ? gl_makecurrent(previous_window, previous_context)
                 : gl_makecurrent(egl_window, NULL);

  if (!c->sdl_context || restored != 0) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    if (c->sdl_context)
      SDL_GL_DeleteContext(c->sdl_context);
    pthread_mutex_unlock(&egl_state_mutex);
    free(c);
    egl_set_error(EGL_BAD_ALLOC);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  c->next = egl_contexts;
  egl_contexts = c;
  pthread_mutex_unlock(&egl_state_mutex);
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d]\n",
              share_context, c, c->id);
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  _egl_context *context;
  _egl_surface *draw_surface;
  _egl_surface *read_surface;
  static int mc_count = 0;
  int mc = ++mc_count;
  int ready_status;

  pthread_mutex_lock(&egl_state_mutex);
  if (!egl_display_valid(dpy)) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_DISPLAY);
  }
  if (!egl_initialized) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_NOT_INITIALIZED);
  }

  /* Unbind is valid only when all three handles are NO_*. */
  if (!ctx || !draw || !read) {
    if (ctx || draw || read) {
      pthread_mutex_unlock(&egl_state_mutex);
      return egl_fail(EGL_BAD_MATCH);
    }
    if (gl_makecurrent(egl_window, NULL) != 0) {
      pthread_mutex_unlock(&egl_state_mutex);
      return egl_fail(EGL_BAD_ACCESS);
    }
    egl_release_tls_binding_locked();
    pthread_mutex_unlock(&egl_state_mutex);
    return EGL_TRUE;
  }

  context = egl_find_context(ctx);
  draw_surface = egl_find_surface(draw);
  read_surface = egl_find_surface(read);
  if (!context) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_CONTEXT);
  }
  if (!draw_surface || !read_surface) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_SURFACE);
  }
  if (context->owner_valid &&
      !pthread_equal(context->owner, pthread_self())) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_ACCESS);
  }

  if (gl_makecurrent(egl_window, context->sdl_context) != 0) {
    debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] SDL FAILED "
                "[ctx_id=%d]: %s\n",
                mc, (unsigned long)pthread_self(), context->id,
                SDL_GetError());
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_ACCESS);
  }

  /* A prova forte e' publicada exatamente no primeiro child current real,
   * nunca durante a criacao antecipada do share-root. */
  if (!graphics_ready_attempted) {
    graphics_ready_attempted = 1;
    graphics_ready_status =
        egl_nxgl_report_valid ? ts_runtime_graphics_ready(&egl_nxgl_report)
                              : -1;
  }
  ready_status = graphics_ready_status;
  if (ready_status != 0) {
    SDL_GLContext prior = tls_context ? tls_context->sdl_context : NULL;
    if (prior)
      (void)gl_makecurrent(egl_window, prior);
    else
      (void)gl_makecurrent(egl_window, NULL);
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_NOT_INITIALIZED);
  }

  if (tls_context && tls_context != context) {
    tls_context->owner_valid = 0;
    memset(&tls_context->owner, 0, sizeof(tls_context->owner));
  }
  if (tls_draw_surface && tls_draw_surface->binding_count)
    --tls_draw_surface->binding_count;
  if (tls_read_surface && tls_read_surface->binding_count)
    --tls_read_surface->binding_count;
  tls_context = context;
  tls_draw_surface = draw_surface;
  tls_read_surface = read_surface;
  ++draw_surface->binding_count;
  ++read_surface->binding_count;
  context->owner = pthread_self();
  context->owner_valid = 1;

  /* TS_SWAPINT e' estado por contexto; aplica no child, nao no root nxgl. */
  {
    const char *swap_interval = getenv("TS_SWAPINT");
    if (swap_interval && !context->swapint_applied) {
      context->swapint_applied = 1;
      if (SDL_GL_SetSwapInterval(atoi(swap_interval)) != 0)
        debugPrintf("egl_shim: TS_SWAPINT recusado: %s\n", SDL_GetError());
    }
  }
  pthread_mutex_unlock(&egl_state_mutex);
  return EGL_TRUE;
}

/* screenshot sob demanda (receita Bully): `touch /dev/shm/coi_shot` ->
 * RGBA cru do backbuffer em /dev/shm/coi_shot.raw + .txt WxH (flip vertical
 * na conversao). Roda na thread de render, custo zero sem o trigger. */
static void coi_maybe_screenshot(void) {
  /* GATE OFF NO RELEASE, pelo mesmo motivo dos injetores de input: no binario
   * publico isto seria um access() por frame para sempre e, pior, um /dev/shm
   * com permissao frouxa viraria um jeito de qualquer processo dumpar a tela do
   * jogo do usuario. Ferramenta de bancada: TS_DEBUG_INJECT=1. */
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("TS_DEBUG_INJECT") ? 1 : 0;
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
 * Roda a cada ~900 frames, SO na thread principal. TS_NO_STACK_SHRINK=1 desliga. */
static void coi_stack_shrink(void) {
  static int mode = -1;          /* -1=probe, 0=off, 1=on */
  static uintptr_t st_lo = 0, st_hi = 0;
  if (mode == 0) return;
  if (mode < 0) {
    if (getenv("TS_NO_STACK_SHRINK")) { mode = 0; return; }
    if ((pid_t)syscall(SYS_gettid) != getpid()) { mode = 0; return; }  /* so main */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { mode = 0; return; }
    char ln[256];
    while (fgets(ln, sizeof ln, f))
      if (strstr(ln, "[stack]")) {
        (void)sscanf(ln, "%" SCNxPTR "-%" SCNxPTR, &st_lo, &st_hi);
        break;
      }
    fclose(f);
    if (!st_lo || !st_hi) { mode = 0; return; }
    mode = 1;
    fprintf(stderr,
            "[STACKSHRINK] [stack]=%" PRIxPTR "-%" PRIxPTR
            " (%" PRIuPTR " MB reserved)\n",
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
    if (n < 4 || getenv("TS_PAGELOG"))
      { fprintf(stderr, "[STACKSHRINK] released %zu MB below the SP\n", len >> 20); n++; }
  }
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  static int perf_enabled = -1;
  _egl_surface *window_surface;
  _egl_context *context;
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&egl_state_mutex);
  window_surface = egl_find_surface(surface);
  context = tls_context;
  if (!egl_initialized || !window_surface ||
      window_surface->kind != TS_EGL_SURFACE_WINDOW) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(!egl_initialized ? EGL_NOT_INITIALIZED : EGL_BAD_SURFACE);
  }
  if (!context || tls_draw_surface != window_surface ||
      egl_find_context((EGLContext)context) != context ||
      !context->owner_valid ||
      !pthread_equal(context->owner, pthread_self()) ||
      SDL_GL_GetCurrentWindow() != egl_window ||
      SDL_GL_GetCurrentContext() != context->sdl_context) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_CURRENT_SURFACE);
  }
  pthread_mutex_unlock(&egl_state_mutex);

  coi_maybe_screenshot();
  { static unsigned fs = 0; if ((++fs % 900) == 0) coi_stack_shrink(); }
  /* NXGL_PRESENT_ENGINE_OWNED, flags=0, quirk/reason=NONE.  In particular:
   * no alpha-one clear, no glFinish and no nxgl-delegated second swap. */
  SDL_GL_SwapWindow(egl_window);
    /* TS_PERF=1 habilita a telemetria de frame-time da bancada. */
    if (perf_enabled < 0)
      perf_enabled = ts_env_enabled("TS_PERF");
    if (perf_enabled) {
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
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  _egl_surface **link;
  _egl_surface *found;
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&egl_state_mutex);
  found = egl_find_surface(surface);
  if (!found) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_SURFACE);
  }
  if (found->binding_count != 0u) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_ACCESS);
  }
  for (link = &egl_surfaces; *link && *link != found; link = &(*link)->next)
    ;
  if (*link == found)
    *link = found->next;
  pthread_mutex_unlock(&egl_state_mutex);
  free(found);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  _egl_context **link;
  _egl_context *context;
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&egl_state_mutex);
  context = egl_find_context(ctx);
  if (!context) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_CONTEXT);
  }
  if (context->owner_valid) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_ACCESS);
  }
  for (link = &egl_contexts; *link && *link != context;
       link = &(*link)->next)
    ;
  if (*link == context)
    *link = context->next;
  if (context->sdl_context)
    SDL_GL_DeleteContext(context->sdl_context);
  pthread_mutex_unlock(&egl_state_mutex);
  free(context);
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  _egl_surface *found;
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  if (!value)
    return egl_fail(EGL_BAD_PARAMETER);
  pthread_mutex_lock(&egl_state_mutex);
  found = egl_find_surface(surface);
  if (!found) {
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_SURFACE);
  }
  switch (attribute) {
  case EGL_WIDTH: *value = found->width; break;
  case EGL_HEIGHT: *value = found->height; break;
  case EGL_CONFIG_ID: *value = egl_nxgl_report.egl.config_id; break;
  case EGL_RENDER_BUFFER:
    *value = found->kind == TS_EGL_SURFACE_WINDOW ? EGL_BACK_BUFFER
                                                  : EGL_SINGLE_BUFFER;
    break;
  default:
    pthread_mutex_unlock(&egl_state_mutex);
    return egl_fail(EGL_BAD_ATTRIBUTE);
  }
  pthread_mutex_unlock(&egl_state_mutex);
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  debugPrintf("egl_shim: eglGetConfigAttrib(attr=0x%x)\n", attribute);
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  if (!egl_config_valid(config))
    return egl_fail(EGL_BAD_CONFIG);
  if (!value)
    return egl_fail(EGL_BAD_PARAMETER);
  if (egl_config_attribute(attribute, value) != 0)
    return egl_fail(EGL_BAD_ATTRIBUTE);
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) {
  EGLint error = tls_egl_error;
  tls_egl_error = EGL_SUCCESS;
  return error;
}

void *egl_shim_GetProcAddress(const char *procname) {
  /* Override GL: a engine resolve glGetString via procaddress; devolvemos NOSSA
   * versão (strings curtas) p/ evitar stack-smash com a lista de extensões do Mali. */
  extern void *ts_gl_proc_override(const char *name);
  if (!procname) {
    egl_set_error(EGL_BAD_PARAMETER);
    return NULL;
  }
  void *ov = ts_gl_proc_override(procname);
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
  if (api != EGL_OPENGL_ES_API)
    return egl_fail(EGL_BAD_PARAMETER);
  return EGL_TRUE;
}

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  if (!egl_display_valid(dpy)) {
    egl_set_error(EGL_BAD_DISPLAY);
    return NULL;
  }
  if (!egl_initialized) {
    egl_set_error(EGL_NOT_INITIALIZED);
    return NULL;
  }
  switch (name) {
  case EGL_VENDOR: return egl_nxgl_report.egl.vendor;
  case EGL_VERSION: return egl_nxgl_report.egl.version;
  case EGL_EXTENSIONS: return "";
  case EGL_CLIENT_APIS: return egl_nxgl_report.egl.client_apis;
  default:
    egl_set_error(EGL_BAD_PARAMETER);
    return NULL;
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  if (!tls_context || SDL_GL_GetCurrentContext() != tls_context->sdl_context)
    return egl_fail(EGL_BAD_CONTEXT);
  if (interval < 0)
    return egl_fail(EGL_BAD_PARAMETER);
  /* TS_SWAPINT força o intervalo (teste do double-pacing: engine dorme
   * ~16ms + vsync = 2 períodos = trava em 30fps; =0 deixa a engine ditar). */
  const char *f = getenv("TS_SWAPINT");
  if (f) interval = atoi(f);
  debugPrintf("egl_shim: SwapInterval(%d)%s\n", (int)interval, f ? " [forçado]" : "");
  if (SDL_GL_SetSwapInterval(interval) != 0)
    return egl_fail(EGL_BAD_PARAMETER);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)tls_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  if (readdraw == EGL_DRAW)
    return (EGLSurface)tls_draw_surface;
  if (readdraw == EGL_READ)
    return (EGLSurface)tls_read_surface;
  egl_set_error(EGL_BAD_PARAMETER);
  return EGL_NO_SURFACE;
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  _egl_surface *surface;
  if (!egl_display_valid(dpy))
    return egl_fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&egl_state_mutex);
  surface = egl_find_surface(s);
  pthread_mutex_unlock(&egl_state_mutex);
  if (!surface)
    return egl_fail(EGL_BAD_SURFACE);
  if (a == EGL_SWAP_BEHAVIOR &&
      (v == EGL_BUFFER_DESTROYED || v == EGL_BUFFER_PRESERVED))
    return EGL_TRUE;
  return egl_fail(EGL_BAD_ATTRIBUTE);
}
