/* SPDX-License-Identifier: GPL-3.0-only */
#include "otr_egl_imports.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *const
    otr_egl_provider_import_names[OTR_EGL_PROVIDER_IMPORT_COUNT] = {
        "eglChooseConfig",
        "eglCreateContext",
        "eglCreateWindowSurface",
        "eglDestroyContext",
        "eglDestroySurface",
        "eglGetConfigAttrib",
        "eglGetCurrentContext",
        "eglGetDisplay",
        "eglInitialize",
        "eglMakeCurrent",
        "eglQueryString",
        "eglSwapBuffers",
        "eglSwapInterval",
        "eglTerminate",
};

typedef void *(*otr_egl_get_current_context)(void);

static void set_error(char *error, size_t error_size, const char *format,
                      const char *detail) {
  if (error == NULL || error_size == 0u) return;
  (void)snprintf(error, error_size, format, detail ? detail : "unknown");
}

int otr_egl_import_table_build(otr_egl_import_table *table,
                               const char *source, void *handle,
                               otr_egl_symbol_lookup lookup,
                               char *error, size_t error_size) {
  size_t i;
  otr_egl_get_current_context get_current_context = NULL;

  if (error != NULL && error_size > 0u) error[0] = '\0';
  if (table == NULL || source == NULL || source[0] == '\0' || lookup == NULL) {
    set_error(error, error_size, "invalid EGL import table argument: %s",
              "null-or-empty");
    return -1;
  }

  memset(table, 0, sizeof(*table));
  table->source = source;
  for (i = 0u; i < OTR_EGL_PROVIDER_IMPORT_COUNT; ++i) {
    const char *name = otr_egl_provider_import_names[i];
    void *address = lookup(handle, name);
    if (address == NULL) {
      set_error(error, error_size, "missing EGL provider symbol: %s", name);
      memset(table, 0, sizeof(*table));
      return -1;
    }
    table->functions[i].symbol = (char *)name;
    table->functions[i].func = (uintptr_t)address;
    if (strcmp(name, "eglGetCurrentContext") == 0)
      get_current_context = (otr_egl_get_current_context)address;
  }

  if (get_current_context == NULL) {
    set_error(error, error_size, "missing EGL provider symbol: %s",
              "eglGetCurrentContext");
    memset(table, 0, sizeof(*table));
    return -1;
  }
  table->current_context = get_current_context();
  if (table->current_context == NULL) {
    set_error(error, error_size, "EGL provider has no current context: %s",
              source);
    memset(table, 0, sizeof(*table));
    return -1;
  }

  table->count = OTR_EGL_PROVIDER_IMPORT_COUNT;
  return 0;
}

static void *provider_lookup(void *handle, const char *name) {
  return dlsym(handle, name);
}

int otr_egl_provider_open_and_promote(otr_egl_import_table *table,
                                      const char *path,
                                      void **retained_handle,
                                      char *error, size_t error_size) {
  void *local_handle;
  void *global_handle;
  const char *open_error;

  if (error != NULL && error_size > 0u) error[0] = '\0';
  if (table == NULL || retained_handle == NULL || path == NULL ||
      path[0] == '\0') {
    set_error(error, error_size, "invalid EGL provider argument: %s",
              "null-or-empty");
    return -1;
  }
  memset(table, 0, sizeof(*table));
  *retained_handle = NULL;

  (void)dlerror();
  local_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (local_handle == NULL) {
    open_error = dlerror();
    set_error(error, error_size, "could not open EGL provider locally: %s",
              open_error ? open_error : path);
    return -1;
  }

  /* Validation precedes promotion. A rejected DSO never enters the global
   * namespace merely because it happened to export a subset of EGL. */
  if (otr_egl_import_table_build(table, path, local_handle, provider_lookup,
                                 error, error_size) != 0) {
    (void)dlclose(local_handle);
    return -1;
  }

  (void)dlerror();
  global_handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (global_handle == NULL) {
    open_error = dlerror();
    set_error(error, error_size, "could not promote EGL provider: %s",
              open_error ? open_error : path);
    memset(table, 0, sizeof(*table));
    (void)dlclose(local_handle);
    return -1;
  }

  /* dlopen owns one reference for each successful call. Drop the validation
   * reference and retain only the promoted one for the guest lifetime. */
  (void)dlclose(local_handle);
  *retained_handle = global_handle;
  return 0;
}
