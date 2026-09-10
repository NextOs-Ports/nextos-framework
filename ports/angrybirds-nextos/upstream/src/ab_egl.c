/* ab_egl.c — ver ab_egl.h. SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define AB_EGL_NO_REDIRECT
#include "ab_egl.h"

#include <SDL.h>
#include <dlfcn.h>
#include <string.h>

#include "nxgl_egl_binding.h"

EGLBoolean (*ab_egl_pfn_eglQueryContext)(EGLDisplay, EGLContext, EGLint, EGLint *);
EGLContext (*ab_egl_pfn_eglGetCurrentContext)(void);
EGLBoolean (*ab_egl_pfn_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLSurface (*ab_egl_pfn_eglGetCurrentSurface)(EGLint);
EGLDisplay (*ab_egl_pfn_eglGetCurrentDisplay)(void);
EGLContext (*ab_egl_pfn_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLint (*ab_egl_pfn_eglGetError)(void);
EGLSurface (*ab_egl_pfn_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
EGLBoolean (*ab_egl_pfn_eglDestroyContext)(EGLDisplay, EGLContext);
EGLBoolean (*ab_egl_pfn_eglDestroySurface)(EGLDisplay, EGLSurface);
EGLBoolean (*ab_egl_pfn_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);

static const char *const k_imports[] = {
    "eglGetCurrentContext",  "eglGetCurrentDisplay",   "eglGetCurrentSurface",
    "eglQueryContext",       "eglChooseConfig",        "eglCreateContext",
    "eglCreatePbufferSurface", "eglDestroyContext",    "eglDestroySurface",
    "eglMakeCurrent",        "eglGetError"};

static char g_receipt[640];

static void *op_resolve(void *u, const char *symbol) {
  (void)u;
  return SDL_GL_GetProcAddress(symbol);
}
static int op_identify(void *u, void *address, nxgl_egl_dso_identity *out) {
  Dl_info info;
  (void)u;
  if (!address || !dladdr(address, &info) || !info.dli_fname || !info.dli_fname[0])
    return 0;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  strncpy(out->path, info.dli_fname, sizeof(out->path) - 1);
  /* ja' global? um dlopen NOLOAD com RTLD_LAZY nao muda o escopo; a bandeira
   * so' informa o receipt e nao altera a prova. */
  out->already_global = 0;
  return 1;
}
static void *op_open_local(void *u, const char *path) {
  (void)u;
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *op_symbol(void *u, void *handle, const char *name) {
  (void)u;
  return handle ? dlsym(handle, name) : NULL;
}
static int op_current_context(void *u, void *handle) {
  void *(*get_current)(void);
  (void)u;
  get_current = (void *(*)(void))dlsym(handle, "eglGetCurrentContext");
  return get_current && get_current() != NULL;
}
static int op_promote_global(void *u, const char *path) {
  (void)u;
  return dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL) != NULL;
}
static void op_close(void *u, void *handle) {
  (void)u;
  if (handle)
    dlclose(handle);
}

#define TAKE(field, sym)                                                   \
  do {                                                                     \
    for (i = 0; i < b.entry_count; i++)                                    \
      if (strcmp(b.entries[i].name, (sym)) == 0)                           \
        field = (void *)b.entries[i].address;                              \
    if (!field)                                                            \
      return 0;                                                            \
  } while (0)

int ab_egl_bind(void) {
  nxgl_egl_binding_request req;
  nxgl_egl_dl_ops ops;
  nxgl_egl_binding b;
  size_t i;
  memset(&req, 0, sizeof(req));
  req.struct_size = sizeof(req);
  req.api_version = NXGL_EGL_BINDING_API_VERSION;
  req.declared_imports = k_imports;
  req.declared_import_count = sizeof(k_imports) / sizeof(k_imports[0]);
  req.guest.struct_size = sizeof(req.guest);
  strncpy(req.guest.name, "angrybirds-nextos loader (EGLWrapper)", sizeof(req.guest.name) - 1);
  memset(&ops, 0, sizeof(ops));
  ops.struct_size = sizeof(ops);
  ops.resolve = op_resolve;
  ops.identify = op_identify;
  ops.open_local = op_open_local;
  ops.symbol = op_symbol;
  ops.current_context = op_current_context;
  ops.promote_global = op_promote_global;
  ops.close = op_close;
  memset(&b, 0, sizeof(b));
  b.struct_size = sizeof(b);
  nxgl_egl_binding_resolve(&req, &ops, &b);
  nxgl_egl_binding_receipt(&b, g_receipt, sizeof(g_receipt));
  if (b.status != NXGL_EGL_BINDING_OK)
    return 0;
  TAKE(ab_egl_pfn_eglQueryContext, "eglQueryContext");
  TAKE(ab_egl_pfn_eglGetCurrentContext, "eglGetCurrentContext");
  TAKE(ab_egl_pfn_eglChooseConfig, "eglChooseConfig");
  TAKE(ab_egl_pfn_eglGetCurrentSurface, "eglGetCurrentSurface");
  TAKE(ab_egl_pfn_eglGetCurrentDisplay, "eglGetCurrentDisplay");
  TAKE(ab_egl_pfn_eglCreateContext, "eglCreateContext");
  TAKE(ab_egl_pfn_eglGetError, "eglGetError");
  TAKE(ab_egl_pfn_eglCreatePbufferSurface, "eglCreatePbufferSurface");
  TAKE(ab_egl_pfn_eglDestroyContext, "eglDestroyContext");
  TAKE(ab_egl_pfn_eglDestroySurface, "eglDestroySurface");
  TAKE(ab_egl_pfn_eglMakeCurrent, "eglMakeCurrent");
  return 1;
}

const char *ab_egl_receipt(void) { return g_receipt; }
