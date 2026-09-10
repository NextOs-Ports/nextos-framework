/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OTR_EGL_IMPORTS_H
#define OTR_EGL_IMPORTS_H

#include <stddef.h>

#include "so_util.h"

#define OTR_EGL_PROVIDER_IMPORT_COUNT 14u
#define OTR_EGL_GUEST_IMPORT_COUNT 15u

typedef void *(*otr_egl_symbol_lookup)(void *handle, const char *name);

typedef struct otr_egl_import_table {
  DynLibFunction functions[OTR_EGL_PROVIDER_IMPORT_COUNT];
  size_t count;
  void *current_context;
  const char *source;
} otr_egl_import_table;

extern const char *const
    otr_egl_provider_import_names[OTR_EGL_PROVIDER_IMPORT_COUNT];

/* Resolve the complete EGL set imported directly by libgame.so, excluding
 * eglGetProcAddress (which remains routed through SDL_GL_GetProcAddress).
 * The table is accepted only when the provider also observes the SDL-owned
 * current context on this thread. */
int otr_egl_import_table_build(otr_egl_import_table *table,
                               const char *source, void *handle,
                               otr_egl_symbol_lookup lookup,
                               char *error, size_t error_size);

/* Open and validate a concrete provider with local visibility first. Only a
 * complete provider that observes the current SDL context is reopened with
 * global visibility. The returned handle owns the retained global reference. */
int otr_egl_provider_open_and_promote(otr_egl_import_table *table,
                                      const char *path,
                                      void **retained_handle,
                                      char *error, size_t error_size);

#endif
