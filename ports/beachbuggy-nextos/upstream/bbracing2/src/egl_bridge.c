/* SPDX-License-Identifier: GPL-3.0-only */
/* Ponte EGL sem DT_NEEDED, com selecao por MEDICAO (a mesma licao do nxgl:
 * nenhuma ordem fixa de SONAME acerta em todas as firmwares — no dArkOS a
 * Mesa versionada e' morta e o blob e' vivo; no ROCKNIX e' o oposto).
 *
 * A engine Vu faz um probe REAL de capacidade antes da janela SDL:
 * eglGetDisplay + eglInitialize + eglGetConfigs + contexto pbuffer. Se o
 * provedor escolhido nao inicializa, a engine aborta o boot. Por isso o
 * criterio de vida aqui e' o probe inteiro: o candidato so' vence se o
 * eglInitialize do PROPRIO candidato responder EGL_TRUE.
 *
 * Mesa headless: sem plataforma declarada o eglGetDisplay(DEFAULT) da Mesa
 * falha fora de X11/Wayland. Cada candidato e' provado duas vezes: com o
 * ambiente como esta' e com EGL_PLATFORM=surfaceless. */
#include "egl_bridge.h"

#define NXGL_GLES2_NO_REDIRECT 1
#include "nxgl_gles2.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"

typedef EGLDisplay (*pfn_eglGetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*pfn_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLDisplay (*pfn_eglGetPlatformDisplay)(EGLenum, void *, const void *);

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

/* Superficie pbuffer sintetica: quando as configs reais nao tem PBUFFER_BIT
 * (GBM), o contexto de probe da engine roda surfaceless
 * (EGL_KHR_surfaceless_context) e a superficie devolvida e' este sentinela. */
#define NXEGL_FAKE_PBUFFER ((EGLSurface)(uintptr_t)0x9BFA)

/* Display GBM real, criado do jeito que o SDL/KMSDRM cria: um gbm_device em
 * cima de um no' DRM. E' a unica plataforma cujas configs carregam
 * EGL_WINDOW_BIT — o filtro de configs da engine exige janela. */
static void *nxegl_gbm_display_native(void) {
  static void *gbm_dev;
  static int tried;
  void *libgbm;
  void *(*create_device)(int);
  static const char *const nodes[] = {
    "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/renderD128",
  };
  size_t i;

  if (tried) return gbm_dev;
  tried = 1;
  libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
  if (libgbm == NULL) libgbm = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
  if (libgbm == NULL) return NULL;
  create_device = (void *(*)(int))dlsym(libgbm, "gbm_create_device");
  if (create_device == NULL) return NULL;
  for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); ++i) {
    int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    gbm_dev = create_device(fd);
    if (gbm_dev != NULL) {
      logPrintf("[egl-bridge] gbm_device em %s\n", nodes[i]);
      return gbm_dev;
    }
    close(fd);
  }
  return NULL;
}

static void *g_handle;
static EGLDisplay g_probe_display = EGL_NO_DISPLAY;

static int egl_candidate_alive(void *handle, const char *name) {
  pfn_eglGetDisplay get_display =
      (pfn_eglGetDisplay)dlsym(handle, "eglGetDisplay");
  pfn_eglInitialize initialize =
      (pfn_eglInitialize)dlsym(handle, "eglInitialize");
  EGLDisplay dpy;
  EGLint major = 0, minor = 0;
  int attempt;

  if (get_display == NULL || initialize == NULL) return 0;
  for (attempt = 0; attempt < 3; ++attempt) {
    const char *label = "default";
    dpy = EGL_NO_DISPLAY;
    if (attempt == 0) {
      /* Default primeiro: e' o caminho vivo dos blobs fbdev (Mali-450). */
      dpy = get_display(EGL_DEFAULT_DISPLAY);
      label = "default";
    } else if (attempt == 1) {
      /* GBM: a plataforma cujas configs tem WINDOW_BIT, a mesma do
       * SDL/KMSDRM. */
      pfn_eglGetPlatformDisplay get_platform =
          (pfn_eglGetPlatformDisplay)dlsym(handle, "eglGetPlatformDisplay");
      void *native = nxegl_gbm_display_native();
      if (get_platform == NULL)
        get_platform = (pfn_eglGetPlatformDisplay)dlsym(
            handle, "eglGetPlatformDisplayEXT");
      if (get_platform == NULL || native == NULL) continue;
      dpy = get_platform(EGL_PLATFORM_GBM_KHR, native, NULL);
      label = "gbm";
    } else {
      setenv("EGL_PLATFORM", "surfaceless", 1);
      dpy = get_display(EGL_DEFAULT_DISPLAY);
      unsetenv("EGL_PLATFORM");
      label = "surfaceless";
    }
    if (dpy == EGL_NO_DISPLAY) continue;
    if (initialize(dpy, &major, &minor) == EGL_TRUE) {
      void *get_proc = dlsym(handle, "eglGetProcAddress");
      logPrintf("[egl-bridge] provedor VIVO: %s (EGL %d.%d, plataforma %s)\n",
                name, major, minor, label);
      g_probe_display = dpy;
      /* A EGL provada aqui e' RTLD_LOCAL: sem isto, o resolvedor GLES2 nao
       * tem NENHUMA fonte coerente com o contexto que a engine vai criar
       * (e o fallback dead ja' escolheu um blob orfao uma vez — SIGSEGV).
       * O eglGetProcAddress do provedor vivo vira a fonte primaria; quando
       * o contexto SDL nascer, o port re-mede com SDL_GL_GetProcAddress. */
      if (get_proc != NULL) {
        nxgl_gles2_set_primary_resolver((nxgl_gles2_resolver_fn)get_proc);
      }
      return 1;
    }
  }
  logPrintf("[egl-bridge] candidato morto: %s\n", name);
  return 0;
}

static void *egl_provider_handle(void) {
  static int tried;
  const char *candidates[] = {
    getenv("SDL_VIDEO_EGL_DRIVER"),
    "libEGL.so",
    "libmali.so",
    "libMali.so",
    "libGLES_mali.so",
    "libEGL.so.1",
  };
  void *first_open = NULL;
  const char *first_open_name = NULL;
  size_t i;

  if (tried) return g_handle;
  tried = 1;
  for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    void *handle;
    if (candidates[i] == NULL || candidates[i][0] == '\0') continue;
    handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) continue;
    if (egl_candidate_alive(handle, candidates[i])) {
      g_handle = handle;
      return g_handle;
    }
    if (first_open == NULL) {
      first_open = handle;
      first_open_name = candidates[i];
    } else {
      dlclose(handle);
    }
  }
  /* Nenhum respondeu vivo: seguir com o primeiro que abriu, denunciando —
   * comportamento identico ao liveness=dead do nxgl. */
  if (first_open != NULL) {
    logPrintf("[egl-bridge] NENHUM provedor vivo; seguindo com %s (dead)\n",
              first_open_name);
    g_handle = first_open;
  } else {
    logPrintf("[egl-bridge] nenhum provedor EGL encontrado\n");
  }
  return g_handle;
}

static void *egl_sym(const char *name) {
  void *handle = egl_provider_handle();
  void *addr = handle != NULL ? dlsym(handle, name) : NULL;
  if (addr == NULL) addr = dlsym(RTLD_DEFAULT, name);
  if (addr == NULL) logPrintf("[egl-bridge] simbolo ausente: %s\n", name);
  return addr;
}

/* Cada wrapper resolve uma vez e encaminha. Retornos de falha seguem a
 * convencao EGL (0/EGL_FALSE/NULL) quando o provedor nao existe. */
#define EGL_BRIDGE_CALL(name, ret_type, fail_value, params, args)  \
  ret_type nxegl_##name params {                                   \
    static ret_type(*fn) params;                                   \
    ret_type egl_bridge_result;                                    \
    if (fn == NULL) fn = (ret_type(*) params)egl_sym(#name);       \
    if (fn == NULL) return fail_value;                             \
    egl_bridge_result = fn args;                                   \
    logPrintf("[egl-bridge] %s -> %ld\n", #name,                   \
              (long)(uintptr_t)egl_bridge_result);                 \
    return egl_bridge_result;                                      \
  }

/* O display que o probe de vida ja' inicializou: devolver o MESMO handle
 * mantem a engine no provedor provado (e o eglInitialize dela vira um
 * re-init idempotente do mesmo display). */
EGLDisplay nxegl_eglGetDisplay(EGLNativeDisplayType display_id) {
  static EGLDisplay (*fn)(EGLNativeDisplayType);
  if (egl_provider_handle() != NULL && g_probe_display != EGL_NO_DISPLAY &&
      display_id == EGL_DEFAULT_DISPLAY) {
    return g_probe_display;
  }
  if (fn == NULL) fn = (EGLDisplay(*)(EGLNativeDisplayType))egl_sym("eglGetDisplay");
  if (fn == NULL) return EGL_NO_DISPLAY;
  return fn(display_id);
}

EGL_BRIDGE_CALL(eglBindAPI, EGLBoolean, EGL_FALSE, (EGLenum api), (api))
EGL_BRIDGE_CALL(eglCreateContext, EGLContext, EGL_NO_CONTEXT,
                (EGLDisplay dpy, EGLConfig config, EGLContext share,
                 const EGLint *attrib_list),
                (dpy, config, share, attrib_list))
EGLSurface nxegl_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                         const EGLint *attrib_list) {
  static EGLSurface (*fn)(EGLDisplay, EGLConfig, const EGLint *);
  EGLSurface surf = EGL_NO_SURFACE;
  if (fn == NULL)
    fn = (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint *))
        egl_sym("eglCreatePbufferSurface");
  if (fn != NULL) surf = fn(dpy, config, attrib_list);
  if (surf == EGL_NO_SURFACE) {
    logPrintf("[egl-bridge] pbuffer real indisponivel; usando surfaceless\n");
    surf = NXEGL_FAKE_PBUFFER;
  }
  return surf;
}
EGL_BRIDGE_CALL(eglCreateWindowSurface, EGLSurface, EGL_NO_SURFACE,
                (EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                 const EGLint *attrib_list),
                (dpy, config, win, attrib_list))
EGL_BRIDGE_CALL(eglDestroyContext, EGLBoolean, EGL_FALSE,
                (EGLDisplay dpy, EGLContext ctx), (dpy, ctx))
EGLBoolean nxegl_eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  static EGLBoolean (*fn)(EGLDisplay, EGLSurface);
  if (surface == NXEGL_FAKE_PBUFFER) return EGL_TRUE;
  if (fn == NULL)
    fn = (EGLBoolean(*)(EGLDisplay, EGLSurface))egl_sym("eglDestroySurface");
  if (fn == NULL) return EGL_FALSE;
  return fn(dpy, surface);
}
EGLBoolean nxegl_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                    EGLint attribute, EGLint *value) {
  static EGLBoolean (*fn)(EGLDisplay, EGLConfig, EGLint, EGLint *);
  EGLBoolean ret;
  if (fn == NULL)
    fn = (EGLBoolean(*)(EGLDisplay, EGLConfig, EGLint, EGLint *))
        egl_sym("eglGetConfigAttrib");
  if (fn == NULL) return EGL_FALSE;
  ret = fn(dpy, config, attribute, value);
  /* O probe da engine so' aceita config com janela E pbuffer; plataformas
   * headless expOem um ou outro. O contexto real de jogo vem da SDL, entao
   * completar os bits aqui e' seguro — pbuffer impossivel vira surfaceless
   * no MakeCurrent (sentinela). */
  if (ret == EGL_TRUE && attribute == EGL_SURFACE_TYPE && value != NULL) {
    *value |= EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
  }
  return ret;
}
EGLBoolean nxegl_eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
                               EGLint config_size, EGLint *num_config) {
  static EGLBoolean (*fn)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
  EGLBoolean ret;
  if (fn == NULL)
    fn = (EGLBoolean(*)(EGLDisplay, EGLConfig *, EGLint, EGLint *))
        egl_sym("eglGetConfigs");
  if (fn == NULL) return EGL_FALSE;
  ret = fn(dpy, configs, config_size, num_config);
  logPrintf("[egl-bridge] eglGetConfigs(size=%d) -> %d (n=%d)\n",
            (int)config_size, (int)ret,
            num_config != NULL ? (int)*num_config : -1);
  return ret;
}
EGL_BRIDGE_CALL(eglGetCurrentContext, EGLContext, EGL_NO_CONTEXT, (void), ())
/* EGL_NV_system_time: o blob Mali exporta e a engine Vu cronometra por ele;
 * Mesa nao tem. Emulado com CLOCK_MONOTONIC em nanossegundos. */
typedef unsigned long long nxegl_uint64;
static nxegl_uint64 nxegl_GetSystemTimeFrequencyNV(void) {
  return 1000000000ull;
}
static nxegl_uint64 nxegl_GetSystemTimeNV(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (nxegl_uint64)ts.tv_sec * 1000000000ull + (nxegl_uint64)ts.tv_nsec;
}

__eglMustCastToProperFunctionPointerType
nxegl_eglGetProcAddress(const char *procname) {
  static __eglMustCastToProperFunctionPointerType (*fn)(const char *);
  __eglMustCastToProperFunctionPointerType ret;
  if (fn == NULL)
    fn = (__eglMustCastToProperFunctionPointerType(*)(const char *))
        egl_sym("eglGetProcAddress");
  ret = fn != NULL ? fn(procname) : NULL;
  if (ret == NULL && procname != NULL) {
    if (strcmp(procname, "eglGetSystemTimeFrequencyNV") == 0)
      ret = (__eglMustCastToProperFunctionPointerType)
          nxegl_GetSystemTimeFrequencyNV;
    else if (strcmp(procname, "eglGetSystemTimeNV") == 0)
      ret = (__eglMustCastToProperFunctionPointerType)nxegl_GetSystemTimeNV;
  }
  logPrintf("[egl-bridge] eglGetProcAddress(%s) -> %p\n",
            procname != NULL ? procname : "?", (void *)ret);
  return ret;
}
EGL_BRIDGE_CALL(eglInitialize, EGLBoolean, EGL_FALSE,
                (EGLDisplay dpy, EGLint *major, EGLint *minor),
                (dpy, major, minor))
EGLBoolean nxegl_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                EGLSurface read, EGLContext ctx) {
  static EGLBoolean (*fn)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  if (fn == NULL)
    fn = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))
        egl_sym("eglMakeCurrent");
  if (fn == NULL) return EGL_FALSE;
  if (draw == NXEGL_FAKE_PBUFFER) draw = EGL_NO_SURFACE;
  if (read == NXEGL_FAKE_PBUFFER) read = EGL_NO_SURFACE;
  return fn(dpy, draw, read, ctx);
}
EGL_BRIDGE_CALL(eglQueryContext, EGLBoolean, EGL_FALSE,
                (EGLDisplay dpy, EGLContext ctx, EGLint attribute,
                 EGLint *value),
                (dpy, ctx, attribute, value))
EGL_BRIDGE_CALL(eglQueryString, const char *, NULL,
                (EGLDisplay dpy, EGLint name), (dpy, name))
EGL_BRIDGE_CALL(eglTerminate, EGLBoolean, EGL_FALSE, (EGLDisplay dpy), (dpy))
