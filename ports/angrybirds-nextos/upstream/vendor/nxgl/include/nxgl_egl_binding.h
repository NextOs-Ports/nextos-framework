/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * V4-GRAPHICS-03: bind a relocated Android guest's EGL imports to the concrete
 * provider that actually owns the CURRENT context, never to the first SONAME
 * or global symbol found by accident.
 *
 * Origin: the OTR 1.0.3 crash on ROCKNIX. GLVND kept libEGL.so.1 inside SDL's
 * RTLD_LOCAL scope, so the so-loader's dlsym(RTLD_DEFAULT) probe found nothing,
 * printed UNRESOLVED and still ran the guest constructors. The first
 * eglGetCurrentContext@plt jumped through a GOT slot that still held the raw
 * link-time value.
 *
 * This API is opt-in and per port. It never adds DT_NEEDED libEGL to the
 * universal executable, never creates a second context or surface, and never
 * globalizes EGL for every game.
 *
 * Everything effectful is injected through nxgl_egl_dl_ops, so the whole
 * contract - including GLVND split, monolithic Mali, an already global path
 * and every negative - is provable hermetically on the host.
 */
#ifndef NXGL_EGL_BINDING_H
#define NXGL_EGL_BINDING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_EGL_BINDING_API_VERSION 1u
#define NXGL_EGL_BINDING_MAX_IMPORTS 64u
#define NXGL_EGL_BINDING_PATH_MAX 512u
#define NXGL_EGL_BINDING_BUILD_ID_MAX 65u

typedef enum nxgl_egl_binding_status {
  NXGL_EGL_BINDING_OK = 0,
  NXGL_EGL_BINDING_INVALID_ARGUMENT,
  /* The SDL resolver does not know eglGetCurrentContext at all. */
  NXGL_EGL_BINDING_NO_RESOLVER,
  /* dladdr could not name the DSO that owns the resolved address. */
  NXGL_EGL_BINDING_NO_PROVIDER_IDENTITY,
  /* The candidate could not even be opened locally. */
  NXGL_EGL_BINDING_OPEN_FAILED,
  /* A declared import is absent from the candidate. */
  NXGL_EGL_BINDING_MISSING_IMPORT,
  /* The candidate answers, but no context is current in it. */
  NXGL_EGL_BINDING_NULL_CONTEXT,
  /* The candidate resolves the probe to a different address than the SDL
   * resolver did: a foreign provider behind the same SONAME. */
  NXGL_EGL_BINDING_PROVIDER_MISMATCH,
  /* Proof succeeded but the object could not be promoted to RTLD_GLOBAL. */
  NXGL_EGL_BINDING_PROMOTION_FAILED,
  /* The guest imports an EGL symbol the port never declared. */
  NXGL_EGL_BINDING_UNDECLARED_IMPORT
} nxgl_egl_binding_status;

typedef struct nxgl_egl_dso_identity {
  size_t struct_size;
  char path[NXGL_EGL_BINDING_PATH_MAX];
  char build_id[NXGL_EGL_BINDING_BUILD_ID_MAX];
  /* Non-zero when the object was already in the global scope before this
   * call. An already global provider is proven the same way and simply is
   * not promoted again. */
  int already_global;
} nxgl_egl_dso_identity;

/* Injected effects. Every call must be side-effect free with respect to the
 * global namespace except promote(), which is only ever reached after proof. */
typedef struct nxgl_egl_dl_ops {
  size_t struct_size;
  void *userdata;
  /* SDL_GL_GetProcAddress, or its equivalent for the port's window system. */
  void *(*resolve)(void *userdata, const char *symbol);
  /* dladdr over an address already resolved by resolve(). */
  int (*identify)(void *userdata, void *address,
                  nxgl_egl_dso_identity *out_identity);
  /* dlopen(path, RTLD_NOW | RTLD_LOCAL). */
  void *(*open_local)(void *userdata, const char *path);
  /* dlsym on the handle returned by open_local. */
  void *(*symbol)(void *userdata, void *handle, const char *name);
  /* Call the candidate's own eglGetCurrentContext. Non-zero when a context
   * is current; the caller never dereferences the value. */
  int (*current_context)(void *userdata, void *handle);
  /* dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL). Returns non-zero on
   * success. Only reached after every proof passed. */
  int (*promote_global)(void *userdata, const char *path);
  /* dlclose for a rejected candidate. A rejected candidate must never leave
   * anything in the global namespace. */
  void (*close)(void *userdata, void *handle);
} nxgl_egl_dl_ops;

typedef struct nxgl_egl_guest_identity {
  size_t struct_size;
  char name[128];
  char build_id[NXGL_EGL_BINDING_BUILD_ID_MAX];
} nxgl_egl_guest_identity;

typedef struct nxgl_egl_binding_request {
  size_t struct_size;
  uint32_t api_version;
  /* The exact EGL imports the port declared for this guest. Order is the
   * order of the produced table. */
  const char *const *declared_imports;
  size_t declared_import_count;
  nxgl_egl_guest_identity guest;
} nxgl_egl_binding_request;

typedef struct nxgl_egl_binding_entry {
  /* Borrowed from declared_imports; valid while the request outlives it. */
  const char *name;
  uintptr_t address;
} nxgl_egl_binding_entry;

typedef struct nxgl_egl_binding {
  size_t struct_size;
  uint32_t api_version;
  nxgl_egl_binding_status status;
  nxgl_egl_dso_identity provider;
  nxgl_egl_guest_identity guest;
  size_t expected_imports;
  size_t resolved_imports;
  int context_current;
  int promoted;
  /* Retained for the guest's lifetime on success; NULL otherwise. */
  void *handle;
  /* Names the first import that failed, when applicable. */
  char failed_symbol[128];
  nxgl_egl_binding_entry entries[NXGL_EGL_BINDING_MAX_IMPORTS];
  size_t entry_count;
} nxgl_egl_binding;

/* Resolve, prove and (only then) promote. On any failure the binding is left
 * with status != OK, handle == NULL, promoted == 0 and nothing added to the
 * global namespace. The caller must fail closed BEFORE init_array and before
 * JNI_OnLoad. */
nxgl_egl_binding_status nxgl_egl_binding_resolve(
    const nxgl_egl_binding_request *request, const nxgl_egl_dl_ops *ops,
    nxgl_egl_binding *out_binding);

/* Every EGL symbol the guest actually leaves undefined must be declared.
 * An extra, undeclared EGL import fails closed instead of silently jumping
 * through a raw GOT slot. Non-EGL names are ignored here: nxloader's own
 * resolve already refuses any remaining undefined strong relocation. */
nxgl_egl_binding_status nxgl_egl_binding_check_inventory(
    const nxgl_egl_binding_request *request,
    const char *const *guest_undefined, size_t guest_undefined_count,
    char *out_offender, size_t out_offender_cap);

const char *nxgl_egl_binding_status_name(nxgl_egl_binding_status status);

/* Structured, one-shot receipt. Never called per frame and never carries a
 * personal path beyond the provider/guest identity the port already ships. */
size_t nxgl_egl_binding_receipt(const nxgl_egl_binding *binding,
                                char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_EGL_BINDING_H */
