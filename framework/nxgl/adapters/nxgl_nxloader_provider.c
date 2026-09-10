/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nxgl_nxloader_provider.h"

#include "nxgl_graphics_contract_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NXGL_NLP_RECEIPT_CAPACITY 4096u
#define NXGL_NLP_HEALTH_CAPACITY 1024u
#define NXGL_NLP_PATH_CAPACITY 4096u

typedef unsigned nxgl_nlp_gluint;

typedef struct nxgl_nlp_state {
  int installed;
  nxgl_nxloader_provider_status status;
  nxgl_nxloader_provider_config config;
  nxloader_registry *registry;
  uintptr_t original_make_current;
  uintptr_t original_get_proc;
  uintptr_t original_create_shader;
  nxgl_graphics_reason reason;
  nxgl_graphics_evidence evidence;
  char receipt[NXGL_NLP_RECEIPT_CAPACITY];
  int ready_marked; /* 0 available, -1 in progress, 1 success, -2 failed */
} nxgl_nlp_state;

static pthread_mutex_t g_nlp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_nlp_condition = PTHREAD_COND_INITIALIZER;
static nxgl_nlp_state g_nlp;

static void *nxgl_nlp_object_pointer(uintptr_t address) {
  void *result = NULL;
  if (sizeof(result) != sizeof(address)) {
    return NULL;
  }
  memcpy(&result, &address, sizeof(result));
  return result;
}

static uintptr_t nxgl_nlp_function_address(const void *storage, size_t size) {
  uintptr_t result = 0u;
  if (storage == NULL || size != sizeof(result)) {
    return 0u;
  }
  memcpy(&result, storage, sizeof(result));
  return result;
}

static int nxgl_nlp_lookup(nxloader_registry *registry, const char *name,
                           uintptr_t *address, int *priority) {
  nxloader_registry_match match;
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  if (nxloader_registry_lookup(registry, name, &match) != NXLOADER_OK) {
    return 0;
  }
  *address = match.address;
  *priority = match.priority;
  return 1;
}

static void *nxgl_nlp_call_get_proc(uintptr_t address, const char *name) {
  void *(*function)(const char *) = NULL;
  if (address == 0u || sizeof(function) != sizeof(address)) {
    return NULL;
  }
  memcpy(&function, &address, sizeof(function));
  return function(name);
}

static void *nxgl_nlp_raw_resolver(void *userdata, const char *name) {
  nxgl_nlp_state *state = (nxgl_nlp_state *)userdata;
  nxloader_registry_match match;
  const char *make_current_name;
  const char *get_proc_name;
  void *resolved;

  if (state == NULL || name == NULL) {
    return NULL;
  }
  make_current_name = state->config.backend == NXGL_NXLOADER_BACKEND_EGL
                          ? "eglMakeCurrent"
                          : "SDL_GL_MakeCurrent";
  get_proc_name = state->config.backend == NXGL_NXLOADER_BACKEND_EGL
                      ? "eglGetProcAddress"
                      : "SDL_GL_GetProcAddress";
  if (strcmp(name, make_current_name) == 0) {
    return nxgl_nlp_object_pointer(state->original_make_current);
  }
  if (strcmp(name, get_proc_name) == 0) {
    return nxgl_nlp_object_pointer(state->original_get_proc);
  }
  if (strcmp(name, "glCreateShader") == 0 &&
      state->original_create_shader != 0u) {
    return nxgl_nlp_object_pointer(state->original_create_shader);
  }

  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  if (state->registry != NULL &&
      nxloader_registry_lookup(state->registry, name, &match) == NXLOADER_OK) {
    /* The three allowlisted registry entries now point at this bridge. They
     * were handled above; never feed a wrapper back to the evidence probe. */
    if (strcmp(name, "glCreateShader") != 0) {
      return nxgl_nlp_object_pointer(match.address);
    }
  }
  resolved = nxgl_nlp_call_get_proc(state->original_get_proc, name);
  return resolved;
}

static int nxgl_nlp_hex64(const char *value) {
  size_t i;
  if (value == NULL || strlen(value) != 64u) {
    return 0;
  }
  for (i = 0u; i < 64u; i++) {
    char ch = value[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static int nxgl_nlp_bounded_nonempty(const char *value, size_t capacity) {
  return value != NULL && capacity > 1u && value[0] != '\0' &&
         memchr(value, '\0', capacity) != NULL;
}

static int nxgl_nlp_bounded_optional(const char *value, size_t capacity) {
  return value != NULL && capacity > 0u &&
         memchr(value, '\0', capacity) != NULL;
}

static int nxgl_nlp_binding_structural_valid(
    const nxgl_nxloader_evidence_binding *binding) {
  return binding != NULL && binding->struct_size == sizeof(*binding) &&
         binding->api_version == NXGL_NXLOADER_EVIDENCE_BINDING_API_VERSION &&
         nxgl_nlp_bounded_optional(binding->run_id,
                                   sizeof(binding->run_id)) &&
         nxgl_nlp_bounded_optional(binding->generation,
                                   sizeof(binding->generation)) &&
         nxgl_nlp_bounded_optional(binding->port_id,
                                   sizeof(binding->port_id)) &&
         nxgl_nlp_bounded_nonempty(binding->framework_commit,
                                   sizeof(binding->framework_commit)) &&
         nxgl_nlp_bounded_nonempty(binding->cfw, sizeof(binding->cfw)) &&
         nxgl_nlp_bounded_nonempty(binding->device, sizeof(binding->device)) &&
         nxgl_nlp_bounded_nonempty(binding->port_version,
                                   sizeof(binding->port_version)) &&
         nxgl_nlp_bounded_nonempty(binding->artifact_sha256,
                                   sizeof(binding->artifact_sha256)) &&
         (binding->generation[0] == '\0' ||
          nxgl_nlp_hex64(binding->generation)) &&
         nxgl_nlp_hex64(binding->artifact_sha256);
}

static int nxgl_nlp_copy_bound(char *destination, size_t capacity,
                               const char *explicit_value,
                               const char *bootstrap_value) {
  const char *selected = explicit_value;
  size_t length;
  if (destination == NULL || capacity == 0u || explicit_value == NULL) {
    return 0;
  }
  if (explicit_value[0] == '\0') {
    selected = bootstrap_value;
  } else if (bootstrap_value != NULL && bootstrap_value[0] != '\0' &&
             strcmp(explicit_value, bootstrap_value) != 0) {
    return 0;
  }
  if (selected == NULL || selected[0] == '\0') {
    return 0;
  }
  length = strlen(selected);
  if (length >= capacity) {
    return 0;
  }
  memcpy(destination, selected, length + 1u);
  return 1;
}

static int nxgl_nlp_apply_binding(nxgl_graphics_evidence *evidence,
                                  const nxgl_nxloader_evidence_binding *binding) {
  const char *health_run = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
  const char *health_generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
  const char *health_port = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");

  if (evidence == NULL || !nxgl_nlp_binding_structural_valid(binding)) {
    return 0;
  }
  evidence->run_id[0] = '\0';
  evidence->generation[0] = '\0';
  evidence->port_id[0] = '\0';
  if (
      !nxgl_nlp_copy_bound(evidence->run_id, sizeof(evidence->run_id),
                           binding->run_id, health_run) ||
      !nxgl_nlp_copy_bound(evidence->generation,
                           sizeof(evidence->generation), binding->generation,
                           health_generation) ||
      !nxgl_nlp_copy_bound(evidence->port_id, sizeof(evidence->port_id),
                           binding->port_id, health_port)) {
    return 0;
  }
  (void)snprintf(evidence->commit, sizeof(evidence->commit), "%s",
                 binding->framework_commit);
  (void)snprintf(evidence->cfw, sizeof(evidence->cfw), "%s", binding->cfw);
  (void)snprintf(evidence->device, sizeof(evidence->device), "%s",
                 binding->device);
  (void)snprintf(evidence->port_version, sizeof(evidence->port_version), "%s",
                 binding->port_version);
  (void)snprintf(evidence->artifact_sha256,
                 sizeof(evidence->artifact_sha256), "%s",
                 binding->artifact_sha256);
  return nxgl_nlp_hex64(evidence->generation) &&
         nxgl_nlp_hex64(evidence->artifact_sha256);
}

static int nxgl_nlp_evidence_bound(const nxgl_graphics_evidence *evidence) {
  return evidence != NULL && evidence->run_id[0] != '\0' &&
         nxgl_nlp_hex64(evidence->generation) &&
         evidence->commit[0] != '\0' && evidence->cfw[0] != '\0' &&
         evidence->device[0] != '\0' && evidence->port_id[0] != '\0' &&
         evidence->port_version[0] != '\0' &&
         nxgl_nlp_hex64(evidence->artifact_sha256) &&
         (evidence->sdl_major == 2 || evidence->sdl_major == 3) &&
         evidence->egl_provider[0] != '\0' &&
         evidence->egl_build_id[0] != '\0' &&
         evidence->gles_provider[0] != '\0' &&
         evidence->dso_build_id[0] != '\0' &&
         evidence->renderer[0] != '\0' &&
         evidence->gl_version_str[0] != '\0' &&
         evidence->glsl_version[0] != '\0' &&
         evidence->egl_version[0] != '\0';
}

static int nxgl_nlp_probe_once(void) {
  nxgl_nxloader_failure_fn failure = NULL;
  void *userdata = NULL;
  nxgl_graphics_reason reason;
  int sink_result;
  int pass;

  if (pthread_mutex_lock(&g_nlp_mutex) != 0) {
    return 0;
  }
  while (g_nlp.status == NXGL_NXLOADER_PROVIDER_PROBING) {
    if (pthread_cond_wait(&g_nlp_condition, &g_nlp_mutex) != 0) {
      (void)pthread_mutex_unlock(&g_nlp_mutex);
      return 0;
    }
  }
  if (g_nlp.status == NXGL_NXLOADER_PROVIDER_PASS ||
      g_nlp.status == NXGL_NXLOADER_PROVIDER_FAIL) {
    pass = g_nlp.status == NXGL_NXLOADER_PROVIDER_PASS;
    (void)pthread_mutex_unlock(&g_nlp_mutex);
    return pass;
  }
  if (g_nlp.status != NXGL_NXLOADER_PROVIDER_ARMED) {
    (void)pthread_mutex_unlock(&g_nlp_mutex);
    return 0;
  }
  g_nlp.status = NXGL_NXLOADER_PROVIDER_PROBING;
  (void)pthread_mutex_unlock(&g_nlp_mutex);

  nxgl_graphics_contract_adapter_set_resolver_ex(nxgl_nlp_raw_resolver,
                                                  &g_nlp);
  reason = nxgl_graphics_contract_adapter_evidence(
      &g_nlp.config.contract, &g_nlp.evidence,
      g_nlp.receipt, sizeof(g_nlp.receipt));
  if (reason == NXGL_GRAPHICS_OK &&
      (!nxgl_nlp_apply_binding(&g_nlp.evidence, &g_nlp.config.binding) ||
       !nxgl_nlp_evidence_bound(&g_nlp.evidence))) {
    reason = NXGL_GRAPHICS_EVIDENCE_INCOMPLETE;
    g_nlp.evidence.verdict = reason;
  }
  /* The adapter's first formatting happened before the provider applied its
   * explicit release binding.  Always reformat the authoritative document. */
  g_nlp.evidence.verdict = reason;
  (void)nxgl_graphics_contract_evidence_json(
      &g_nlp.config.contract, &g_nlp.evidence,
      g_nlp.receipt, sizeof(g_nlp.receipt));
  sink_result = (reason == NXGL_GRAPHICS_OK && g_nlp.receipt[0] != '\0')
                    ? g_nlp.config.receipt_sink(
                          g_nlp.config.userdata, g_nlp.receipt,
                          strlen(g_nlp.receipt))
                    : -1;
  pass = reason == NXGL_GRAPHICS_OK && sink_result == 0;
  if (reason == NXGL_GRAPHICS_OK && sink_result != 0) {
    reason = NXGL_GRAPHICS_RECEIPT_REJECTED;
  }

  if (pthread_mutex_lock(&g_nlp_mutex) != 0) {
    return 0;
  }
  g_nlp.reason = reason;
  g_nlp.status = pass ? NXGL_NXLOADER_PROVIDER_PASS
                      : NXGL_NXLOADER_PROVIDER_FAIL;
  failure = !pass ? g_nlp.config.failure : NULL;
  userdata = g_nlp.config.userdata;
  (void)pthread_cond_broadcast(&g_nlp_condition);
  (void)pthread_mutex_unlock(&g_nlp_mutex);
  if (failure != NULL) {
    failure(userdata, reason);
  }
  return pass;
}

static unsigned nxgl_nlp_egl_make_current(void *display, void *draw,
                                           void *read, void *context) {
  unsigned (*original)(void *, void *, void *, void *) = NULL;
  unsigned result;
  memcpy(&original, &g_nlp.original_make_current, sizeof(original));
  result = original(display, draw, read, context);
  if (result != 0u && !nxgl_nlp_probe_once()) {
    return 0u;
  }
  return result;
}

static int nxgl_nlp_sdl2_make_current(void *window, void *context) {
  int (*original)(void *, void *) = NULL;
  int result;
  memcpy(&original, &g_nlp.original_make_current, sizeof(original));
  result = original(window, context);
  if (result == 0 && !nxgl_nlp_probe_once()) {
    return -1;
  }
  return result;
}

static _Bool nxgl_nlp_sdl3_make_current(void *window, void *context) {
  _Bool (*original)(void *, void *) = NULL;
  _Bool result;
  memcpy(&original, &g_nlp.original_make_current, sizeof(original));
  result = original(window, context);
  if (result && !nxgl_nlp_probe_once()) {
    return 0;
  }
  return result;
}

static nxgl_nlp_gluint nxgl_nlp_create_shader(unsigned type) {
  nxgl_nlp_gluint (*original)(unsigned) = NULL;
  void *resolved;
  uintptr_t address;
  if (!nxgl_nlp_probe_once()) {
    return 0u;
  }
  address = g_nlp.original_create_shader;
  if (address == 0u) {
    resolved = nxgl_nlp_call_get_proc(g_nlp.original_get_proc,
                                      "glCreateShader");
    if (resolved == NULL || sizeof(address) != sizeof(resolved)) {
      return 0u;
    }
    memcpy(&address, &resolved, sizeof(address));
  }
  memcpy(&original, &address, sizeof(original));
  return original(type);
}

static void *nxgl_nlp_get_proc(const char *name) {
  void *result;
  void *wrapper;
  if (name == NULL) {
    return NULL;
  }
  result = nxgl_nlp_call_get_proc(g_nlp.original_get_proc, name);
  if (strcmp(name, "glCreateShader") == 0) {
    nxgl_nlp_gluint (*function)(unsigned) = nxgl_nlp_create_shader;
    wrapper = nxgl_nlp_object_pointer(
        nxgl_nlp_function_address(&function, sizeof(function)));
    return wrapper != NULL ? wrapper : result;
  }
  if (g_nlp.config.backend == NXGL_NXLOADER_BACKEND_EGL &&
      strcmp(name, "eglMakeCurrent") == 0) {
    unsigned (*function)(void *, void *, void *, void *) =
        nxgl_nlp_egl_make_current;
    wrapper = nxgl_nlp_object_pointer(
        nxgl_nlp_function_address(&function, sizeof(function)));
    return wrapper != NULL ? wrapper : result;
  }
  return result;
}

nxloader_result nxgl_nxloader_provider_install(
    nxloader_registry *registry,
    const nxgl_nxloader_provider_config *config) {
  nxloader_symbol symbols[3];
  nxloader_provider provider;
  nxloader_registry_report report;
  uintptr_t make_current_wrapper = 0u;
  uintptr_t get_proc_wrapper = 0u;
  uintptr_t create_shader_wrapper = 0u;
  uintptr_t original_make_current = 0u;
  uintptr_t original_get_proc = 0u;
  uintptr_t original_create_shader = 0u;
  const char *make_current_name;
  const char *get_proc_name;
  int make_priority = 0, get_priority = 0, shader_priority = 0;
  int priority;
  nxloader_result result;

  if (registry == NULL || config == NULL ||
      config->struct_size < sizeof(*config) ||
      config->api_version != NXGL_NXLOADER_PROVIDER_API_VERSION ||
      (config->flags &
       ~(uint32_t)NXGL_NXLOADER_PROVIDER_GRAPHICS_EVIDENCE) != 0u) {
    return NXLOADER_EINVAL;
  }
  if (config->flags == 0u) {
    return NXLOADER_OK;
  }
  if ((config->backend != NXGL_NXLOADER_BACKEND_EGL &&
       config->backend != NXGL_NXLOADER_BACKEND_SDL2 &&
       config->backend != NXGL_NXLOADER_BACKEND_SDL3) ||
      !nxgl_graphics_contract_is_valid(&config->contract) ||
      !nxgl_nlp_binding_structural_valid(&config->binding) ||
      config->receipt_sink == NULL) {
    return NXLOADER_EINVAL;
  }

  make_current_name = config->backend == NXGL_NXLOADER_BACKEND_EGL
                          ? "eglMakeCurrent"
                          : "SDL_GL_MakeCurrent";
  get_proc_name = config->backend == NXGL_NXLOADER_BACKEND_EGL
                      ? "eglGetProcAddress"
                      : "SDL_GL_GetProcAddress";
  if (!nxgl_nlp_lookup(registry, make_current_name, &original_make_current,
                       &make_priority) ||
      !nxgl_nlp_lookup(registry, get_proc_name, &original_get_proc,
                       &get_priority)) {
    return NXLOADER_EUNRESOLVED;
  }
  (void)nxgl_nlp_lookup(registry, "glCreateShader", &original_create_shader,
                        &shader_priority);
  priority = make_priority > get_priority ? make_priority : get_priority;
  if (original_create_shader != 0u && shader_priority > priority) {
    priority = shader_priority;
  }
  if (priority == INT_MAX) {
    return NXLOADER_EOVERFLOW;
  }
  priority++;

  if (config->backend == NXGL_NXLOADER_BACKEND_EGL) {
    unsigned (*function)(void *, void *, void *, void *) =
        nxgl_nlp_egl_make_current;
    make_current_wrapper = nxgl_nlp_function_address(&function,
                                                      sizeof(function));
  } else if (config->backend == NXGL_NXLOADER_BACKEND_SDL2) {
    int (*function)(void *, void *) = nxgl_nlp_sdl2_make_current;
    make_current_wrapper = nxgl_nlp_function_address(&function,
                                                      sizeof(function));
  } else {
    _Bool (*function)(void *, void *) = nxgl_nlp_sdl3_make_current;
    make_current_wrapper = nxgl_nlp_function_address(&function,
                                                      sizeof(function));
  }
  {
    void *(*function)(const char *) = nxgl_nlp_get_proc;
    get_proc_wrapper = nxgl_nlp_function_address(&function, sizeof(function));
  }
  {
    nxgl_nlp_gluint (*function)(unsigned) = nxgl_nlp_create_shader;
    create_shader_wrapper = nxgl_nlp_function_address(&function,
                                                       sizeof(function));
  }
  if (make_current_wrapper == 0u || get_proc_wrapper == 0u ||
      create_shader_wrapper == 0u) {
    return NXLOADER_EUNSUPPORTED;
  }

  if (pthread_mutex_lock(&g_nlp_mutex) != 0) {
    return NXLOADER_ECALLBACK;
  }
  if (g_nlp.installed) {
    (void)pthread_mutex_unlock(&g_nlp_mutex);
    return NXLOADER_ESTATE;
  }
  memset(&g_nlp, 0, sizeof(g_nlp));
  g_nlp.installed = 1;
  g_nlp.status = NXGL_NXLOADER_PROVIDER_ARMED;
  g_nlp.config = *config;
  g_nlp.registry = registry;
  g_nlp.original_make_current = original_make_current;
  g_nlp.original_get_proc = original_get_proc;
  g_nlp.original_create_shader = original_create_shader;
  g_nlp.reason = NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY;

  memset(symbols, 0, sizeof(symbols));
  symbols[0].name = make_current_name;
  symbols[0].address = make_current_wrapper;
  symbols[1].name = get_proc_name;
  symbols[1].address = get_proc_wrapper;
  symbols[2].name = "glCreateShader";
  symbols[2].address = create_shader_wrapper;
  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "nxgl-nxloader-graphics-v1";
  provider.symbols = symbols;
  provider.symbol_count = sizeof(symbols) / sizeof(symbols[0]);
  provider.priority = priority;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  result = nxloader_registry_add_provider(registry, &provider, &report);
  if (result != NXLOADER_OK) {
    memset(&g_nlp, 0, sizeof(g_nlp));
  }
  (void)pthread_mutex_unlock(&g_nlp_mutex);
  return result;
}

static int nxgl_nlp_safe_identity(const char *value, size_t maximum) {
  size_t i;
  if (value == NULL || value[0] == '\0') {
    return 0;
  }
  for (i = 0u; value[i] != '\0'; i++) {
    unsigned char ch = (unsigned char)value[i];
    if (i >= maximum ||
        !((ch >= (unsigned char)'a' && ch <= (unsigned char)'z') ||
          (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
          (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
          ch == (unsigned char)'.' || ch == (unsigned char)'_' ||
          ch == (unsigned char)'-')) {
      return 0;
    }
  }
  return 1;
}

static int nxgl_nlp_generation_valid(const char *generation) {
  size_t i;
  if (generation != NULL && strcmp(generation, "none") == 0) {
    return 1;
  }
  if (generation == NULL || strlen(generation) != 64u) {
    return 0;
  }
  for (i = 0u; i < 64u; i++) {
    char ch = generation[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static int nxgl_nlp_write_all(int fd, const char *data, size_t size) {
  size_t written = 0u;
  while (written < size) {
    ssize_t step = write(fd, data + written, size - written);
    if (step < 0 && errno == EINTR) {
      continue;
    }
    if (step <= 0) {
      return 0;
    }
    written += (size_t)step;
  }
  return 1;
}

static void nxgl_nlp_unlink_owned(int directory_fd, const char *name,
                                  dev_t device, ino_t inode) {
  struct stat current;
  if (directory_fd < 0 || name == NULL ||
      fstatat(directory_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0) {
    return;
  }
  if (S_ISREG(current.st_mode) && current.st_dev == device &&
      current.st_ino == inode && current.st_uid == getuid()) {
    (void)unlinkat(directory_fd, name, 0);
  }
}

static nxloader_result nxgl_nlp_publish_health(void) {
  const char *path = getenv("NXBOOTSTRAP_HEALTH_FILE");
  const char *schema = getenv("NXBOOTSTRAP_HEALTH_SCHEMA");
  const char *schema_version = getenv("NXBOOTSTRAP_HEALTH_SCHEMA_VERSION");
  const char *run_id = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
  const char *generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
  const char *port_id = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
  const char *slash;
  const char *basename;
  char parent[NXGL_NLP_PATH_CAPACITY];
  char expected_basename[384];
  char temporary[512];
  char receipt[NXGL_NLP_HEALTH_CAPACITY];
  struct stat directory_stat;
  struct stat temporary_stat;
  struct stat final_stat;
  size_t parent_length;
  int directory_fd = -1;
  int temporary_fd = -1;
  int final_fd = -1;
  int receipt_size;
  int name_size;
  int temp_size;
  int renamed = 0;
  nxloader_result result = NXLOADER_EIO;

  memset(&directory_stat, 0, sizeof(directory_stat));
  memset(&temporary_stat, 0, sizeof(temporary_stat));
  memset(&final_stat, 0, sizeof(final_stat));

  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(parent) ||
      schema == NULL || strcmp(schema, "org.nextos.nxruntime.health") != 0 ||
      schema_version == NULL || strcmp(schema_version, "1") != 0 ||
      !nxgl_nlp_safe_identity(run_id, 160u) ||
      !nxgl_nlp_generation_valid(generation) ||
      !nxgl_nlp_safe_identity(port_id, 96u) ||
      strcmp(run_id, g_nlp.evidence.run_id) != 0 ||
      strcmp(generation, g_nlp.evidence.generation) != 0 ||
      strcmp(port_id, g_nlp.evidence.port_id) != 0) {
    return NXLOADER_EINVAL;
  }
  slash = strrchr(path, '/');
  if (slash == NULL || slash == path || slash[1] == '\0') {
    return NXLOADER_EINVAL;
  }
  basename = slash + 1;
  name_size = snprintf(expected_basename, sizeof(expected_basename),
                       "health-%s-%s.json", port_id, run_id);
  if (name_size < 0 || (size_t)name_size >= sizeof(expected_basename) ||
      strcmp(basename, expected_basename) != 0) {
    return NXLOADER_EINVAL;
  }
  parent_length = (size_t)(slash - path);
  if (parent_length == 0u || parent_length >= sizeof(parent)) {
    return NXLOADER_EINVAL;
  }
  memcpy(parent, path, parent_length);
  parent[parent_length] = '\0';

  directory_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory_fd < 0 || fstat(directory_fd, &directory_stat) != 0 ||
      !S_ISDIR(directory_stat.st_mode) ||
      directory_stat.st_uid != getuid() ||
      (directory_stat.st_mode & (mode_t)0777) != (mode_t)0700) {
    result = NXLOADER_EIO;
    goto done;
  }
  errno = 0;
  if (fstatat(directory_fd, basename, &final_stat,
              AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
    result = NXLOADER_EIO; /* includes a pre-existing symlink */
    goto done;
  }
  temp_size = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", basename,
                       (long)getpid());
  if (temp_size < 0 || (size_t)temp_size >= sizeof(temporary)) {
    result = NXLOADER_EOVERFLOW;
    goto done;
  }
  receipt_size = snprintf(
      receipt, sizeof(receipt),
      "{\"schema\":\"%s\",\"schema_version\":1,\"run_id\":\"%s\","
      "\"generation\":\"%s\",\"port_id\":\"%s\",\"status\":\"ready\"}\n",
      schema, run_id, generation, port_id);
  if (receipt_size < 0 || (size_t)receipt_size >= sizeof(receipt)) {
    result = NXLOADER_EOVERFLOW;
    goto done;
  }
  temporary_fd = openat(directory_fd, temporary,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        (mode_t)0600);
  if (temporary_fd < 0 || fchmod(temporary_fd, (mode_t)0600) != 0 ||
      fstat(temporary_fd, &temporary_stat) != 0 ||
      !S_ISREG(temporary_stat.st_mode) || temporary_stat.st_uid != getuid() ||
      temporary_stat.st_nlink != (nlink_t)1 ||
      (temporary_stat.st_mode & (mode_t)0777) != (mode_t)0600 ||
      !nxgl_nlp_write_all(temporary_fd, receipt, (size_t)receipt_size) ||
      fsync(temporary_fd) != 0 || close(temporary_fd) != 0) {
    if (temporary_fd >= 0) {
      (void)close(temporary_fd);
      temporary_fd = -1;
    }
    if (temporary_stat.st_ino != (ino_t)0) {
      nxgl_nlp_unlink_owned(directory_fd, temporary,
                            temporary_stat.st_dev, temporary_stat.st_ino);
    }
    result = NXLOADER_EIO;
    goto done;
  }
  temporary_fd = -1;

#if defined(SYS_renameat2)
  if (syscall(SYS_renameat2, directory_fd, temporary, directory_fd, basename,
              1u /* RENAME_NOREPLACE */) != 0) {
    nxgl_nlp_unlink_owned(directory_fd, temporary,
                          temporary_stat.st_dev, temporary_stat.st_ino);
    result = errno == ENOSYS ? NXLOADER_EUNSUPPORTED : NXLOADER_EIO;
    goto done;
  }
  renamed = 1;
#else
  nxgl_nlp_unlink_owned(directory_fd, temporary,
                        temporary_stat.st_dev, temporary_stat.st_ino);
  result = NXLOADER_EUNSUPPORTED;
  goto done;
#endif

  final_fd = openat(directory_fd, basename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (final_fd < 0 || fstat(final_fd, &final_stat) != 0 ||
      !S_ISREG(final_stat.st_mode) || final_stat.st_uid != getuid() ||
      final_stat.st_nlink != (nlink_t)1 ||
      final_stat.st_dev != temporary_stat.st_dev ||
      final_stat.st_ino != temporary_stat.st_ino ||
      (final_stat.st_mode & (mode_t)0777) != (mode_t)0600 ||
      fsync(directory_fd) != 0) {
    nxgl_nlp_unlink_owned(directory_fd, basename,
                          temporary_stat.st_dev, temporary_stat.st_ino);
    result = NXLOADER_EIO;
    goto done;
  }
  result = NXLOADER_OK;

done:
  if (temporary_fd >= 0) {
    (void)close(temporary_fd);
  }
  if (final_fd >= 0) {
    (void)close(final_fd);
  }
  if (result != NXLOADER_OK && renamed && directory_fd >= 0 &&
      temporary_stat.st_ino != (ino_t)0) {
    nxgl_nlp_unlink_owned(directory_fd, basename,
                          temporary_stat.st_dev, temporary_stat.st_ino);
  }
  if (directory_fd >= 0) {
    (void)close(directory_fd);
  }
  return result;
}

nxloader_result nxgl_nxloader_provider_mark_ready(void) {
  nxloader_result result;
  if (pthread_mutex_lock(&g_nlp_mutex) != 0) {
    return NXLOADER_ECALLBACK;
  }
  if (g_nlp.status != NXGL_NXLOADER_PROVIDER_PASS ||
      g_nlp.ready_marked != 0) {
    (void)pthread_mutex_unlock(&g_nlp_mutex);
    return NXLOADER_ESTATE;
  }
  g_nlp.ready_marked = -1;
  (void)pthread_mutex_unlock(&g_nlp_mutex);

  result = nxgl_nlp_publish_health();
  if (pthread_mutex_lock(&g_nlp_mutex) != 0) {
    return NXLOADER_ECALLBACK;
  }
  g_nlp.ready_marked = result == NXLOADER_OK ? 1 : -2;
  (void)pthread_mutex_unlock(&g_nlp_mutex);
  return result;
}

nxgl_nxloader_provider_status nxgl_nxloader_provider_get_status(
    nxgl_graphics_evidence *evidence, char *json, size_t json_cap) {
  nxgl_nxloader_provider_status status;
  if (json != NULL && json_cap > 0u) {
    json[0] = '\0';
  }
  if (pthread_mutex_lock(&g_nlp_mutex) != 0) {
    return NXGL_NXLOADER_PROVIDER_FAIL;
  }
  status = g_nlp.status;
  if (evidence != NULL &&
      (status == NXGL_NXLOADER_PROVIDER_PASS ||
       status == NXGL_NXLOADER_PROVIDER_FAIL)) {
    *evidence = g_nlp.evidence;
  }
  if (json != NULL && json_cap > 0u && g_nlp.receipt[0] != '\0') {
    size_t length = strlen(g_nlp.receipt);
    if (length < json_cap) {
      memcpy(json, g_nlp.receipt, length + 1u);
    }
  }
  (void)pthread_mutex_unlock(&g_nlp_mutex);
  return status;
}
