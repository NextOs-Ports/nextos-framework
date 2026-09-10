/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "otr_egl_imports.h"

typedef struct fake_provider {
  const char *missing;
  int current_context_present;
} fake_provider;

static void fake_egl_function(void) {}

static void *fake_current_context_present(void) {
  return (void *)(uintptr_t)0x1234u;
}

static void *fake_current_context_absent(void) { return NULL; }

static void *fake_lookup(void *opaque, const char *name) {
  fake_provider *provider = (fake_provider *)opaque;
  if (provider->missing != NULL && strcmp(provider->missing, name) == 0)
    return NULL;
  if (strcmp(name, "eglGetCurrentContext") == 0) {
    return provider->current_context_present
               ? (void *)fake_current_context_present
               : (void *)fake_current_context_absent;
  }
  return (void *)fake_egl_function;
}

static void test_complete_provider(void) {
  fake_provider provider = {NULL, 1};
  otr_egl_import_table table;
  char error[160];

  assert(otr_egl_import_table_build(&table, "fake-egl", &provider,
                                    fake_lookup, error, sizeof(error)) == 0);
  assert(error[0] == '\0');
  assert(table.count == OTR_EGL_PROVIDER_IMPORT_COUNT);
  assert(table.current_context == (void *)(uintptr_t)0x1234u);
  assert(strcmp(table.source, "fake-egl") == 0);
  for (size_t i = 0u; i < table.count; ++i) {
    assert(strcmp(table.functions[i].symbol,
                  otr_egl_provider_import_names[i]) == 0);
    assert(table.functions[i].func != 0u);
  }
}

static void test_missing_symbol_is_rejected(void) {
  fake_provider provider = {"eglSwapBuffers", 1};
  otr_egl_import_table table;
  char error[160];

  assert(otr_egl_import_table_build(&table, "incomplete-egl", &provider,
                                    fake_lookup, error, sizeof(error)) != 0);
  assert(table.count == 0u);
  assert(strstr(error, "eglSwapBuffers") != NULL);
}

static void test_foreign_provider_is_rejected(void) {
  fake_provider provider = {NULL, 0};
  otr_egl_import_table table;
  char error[160];

  assert(otr_egl_import_table_build(&table, "foreign-egl", &provider,
                                    fake_lookup, error, sizeof(error)) != 0);
  assert(table.count == 0u);
  assert(strstr(error, "no current context") != NULL);
}

static void test_local_provider_is_validated_before_promotion(
    const char *complete_path, const char *incomplete_path,
    const char *null_context_path) {
  otr_egl_import_table table;
  void *local_handle;
  void *promoted_handle = NULL;
  char error[256];

  local_handle = dlopen(complete_path, RTLD_NOW | RTLD_LOCAL);
  assert(local_handle != NULL);
  assert(dlsym(RTLD_DEFAULT, "otr_egl_fixture_complete_marker") == NULL);
  assert(otr_egl_provider_open_and_promote(
             &table, complete_path, &promoted_handle,
             error, sizeof(error)) == 0);
  assert(promoted_handle != NULL);
  assert(table.count == OTR_EGL_PROVIDER_IMPORT_COUNT);
  assert(dlsym(RTLD_DEFAULT, "otr_egl_fixture_complete_marker") != NULL);
  assert(dlclose(local_handle) == 0);
  assert(dlclose(promoted_handle) == 0);

  local_handle = dlopen(incomplete_path, RTLD_NOW | RTLD_LOCAL);
  assert(local_handle != NULL);
  assert(dlsym(RTLD_DEFAULT, "otr_egl_fixture_incomplete_marker") == NULL);
  promoted_handle = NULL;
  assert(otr_egl_provider_open_and_promote(
             &table, incomplete_path, &promoted_handle,
             error, sizeof(error)) != 0);
  assert(promoted_handle == NULL);
  assert(strstr(error, "eglSwapBuffers") != NULL);
  assert(dlsym(RTLD_DEFAULT, "otr_egl_fixture_incomplete_marker") == NULL);
  assert(dlclose(local_handle) == 0);

  local_handle = dlopen(null_context_path, RTLD_NOW | RTLD_LOCAL);
  assert(local_handle != NULL);
  assert(dlsym(RTLD_DEFAULT, "otr_egl_fixture_null_context_marker") == NULL);
  promoted_handle = NULL;
  assert(otr_egl_provider_open_and_promote(
             &table, null_context_path, &promoted_handle,
             error, sizeof(error)) != 0);
  assert(promoted_handle == NULL);
  assert(strstr(error, "no current context") != NULL);
  assert(dlsym(RTLD_DEFAULT, "otr_egl_fixture_null_context_marker") == NULL);
  assert(dlclose(local_handle) == 0);
}

int main(int argc, char **argv) {
  assert(argc == 4);
  test_complete_provider();
  test_missing_symbol_is_rejected();
  test_foreign_provider_is_rejected();
  test_local_provider_is_validated_before_promotion(argv[1], argv[2], argv[3]);
  puts("test_egl_imports: PASS");
  return 0;
}
