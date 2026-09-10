/* SPDX-License-Identifier: GPL-3.0-only */
/* Host integration test: real nxloader registry, fake effectful EGL/GL. */
#include "nxgl_nxloader_provider.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_SHADING_LANGUAGE_VERSION 0x8B8Cu
#define GL_COMPILE_STATUS 0x8B81u
#define GL_LINK_STATUS 0x8B82u

static int make_current_calls;
static int get_proc_calls;
static int create_shader_calls;
static int delete_shader_calls;
static int delete_program_calls;
static int receipt_calls;
static char captured_receipt[4096];

static void check(int condition, const char *message) {
  if (!condition) {
    (void)fprintf(stderr, "nxgl_nxloader_provider: %s\n", message);
    exit(1);
  }
}

static uintptr_t function_address(const void *storage, size_t size) {
  uintptr_t result = 0u;
  check(size == sizeof(result), "function and uintptr sizes differ");
  memcpy(&result, storage, sizeof(result));
  return result;
}

#define FN_ADDRESS(fn, type)                                                   \
  function_address(&(type){fn}, sizeof(type))

static const unsigned char *fake_gl_get_string(unsigned name) {
  switch (name) {
    case GL_RENDERER:
      return (const unsigned char *)"Fake Mali-450";
    case GL_VERSION:
      return (const unsigned char *)"OpenGL ES 2.0 fake";
    case GL_SHADING_LANGUAGE_VERSION:
      return (const unsigned char *)"OpenGL ES GLSL ES 1.00";
    default:
      return (const unsigned char *)"";
  }
}

static unsigned fake_gl_create_shader(unsigned type) {
  (void)type;
  create_shader_calls++;
  return (unsigned)(100 + create_shader_calls);
}

static void fake_gl_shader_source(unsigned shader, int count,
                                  const char *const *sources,
                                  const int *lengths) {
  (void)shader;
  (void)count;
  (void)sources;
  (void)lengths;
}

static void fake_gl_compile_shader(unsigned shader) { (void)shader; }

static void fake_gl_get_shader_iv(unsigned shader, unsigned pname, int *value) {
  (void)shader;
  check(pname == GL_COMPILE_STATUS, "unexpected shader query");
  *value = 1;
}

static unsigned fake_gl_create_program(void) { return 200u; }
static void fake_gl_attach_shader(unsigned program, unsigned shader) {
  (void)program;
  (void)shader;
}
static void fake_gl_link_program(unsigned program) { (void)program; }
static void fake_gl_get_program_iv(unsigned program, unsigned pname, int *value) {
  (void)program;
  check(pname == GL_LINK_STATUS, "unexpected program query");
  *value = 1;
}
static void fake_gl_delete_shader(unsigned shader) {
  (void)shader;
  delete_shader_calls++;
}
static void fake_gl_delete_program(unsigned program) {
  (void)program;
  delete_program_calls++;
}

static void fake_sdl_drawable_size(void *window, int *width, int *height) {
  (void)window;
  *width = 640;
  *height = 480;
}
static void *fake_sdl_current_window(void) { return (void *)(uintptr_t)1u; }
static int fake_sdl_get_attribute(int attribute, int *value) {
  (void)attribute;
  *value = 4;
  return 0;
}
static void fake_sdl_pump_events(void) {}

static const char *fake_egl_query_string(void *display, int name) {
  (void)display;
  (void)name;
  return "1.4 fake";
}
static void *fake_egl_get_current_display(void) {
  return (void *)(uintptr_t)2u;
}
static unsigned fake_egl_make_current(void *display, void *draw, void *read,
                                      void *context) {
  (void)display;
  (void)draw;
  (void)read;
  (void)context;
  make_current_calls++;
  return 1u;
}

static void *fake_egl_get_proc_address(const char *name) {
  get_proc_calls++;
  if (name == NULL) {
    return NULL;
  }
#define RETURN_FUNCTION(symbol, function, type)                                \
  if (strcmp(name, symbol) == 0) {                                             \
    uintptr_t address = FN_ADDRESS(function, type);                            \
    void *result = NULL;                                                       \
    memcpy(&result, &address, sizeof(result));                                 \
    return result;                                                             \
  }
  RETURN_FUNCTION("glGetString", fake_gl_get_string,
                  const unsigned char *(*)(unsigned));
  RETURN_FUNCTION("glCreateShader", fake_gl_create_shader,
                  unsigned (*)(unsigned));
  RETURN_FUNCTION("glShaderSource", fake_gl_shader_source,
                  void (*)(unsigned, int, const char *const *, const int *));
  RETURN_FUNCTION("glCompileShader", fake_gl_compile_shader,
                  void (*)(unsigned));
  RETURN_FUNCTION("glGetShaderiv", fake_gl_get_shader_iv,
                  void (*)(unsigned, unsigned, int *));
  RETURN_FUNCTION("glCreateProgram", fake_gl_create_program,
                  unsigned (*)(void));
  RETURN_FUNCTION("glAttachShader", fake_gl_attach_shader,
                  void (*)(unsigned, unsigned));
  RETURN_FUNCTION("glLinkProgram", fake_gl_link_program,
                  void (*)(unsigned));
  RETURN_FUNCTION("glGetProgramiv", fake_gl_get_program_iv,
                  void (*)(unsigned, unsigned, int *));
  RETURN_FUNCTION("glDeleteShader", fake_gl_delete_shader,
                  void (*)(unsigned));
  RETURN_FUNCTION("glDeleteProgram", fake_gl_delete_program,
                  void (*)(unsigned));
  RETURN_FUNCTION("eglMakeCurrent", fake_egl_make_current,
                  unsigned (*)(void *, void *, void *, void *));
#undef RETURN_FUNCTION
  return NULL;
}

static int capture_receipt(void *userdata, const char *json, size_t size) {
  (void)userdata;
  receipt_calls++;
  if (json == NULL || size == 0u || size >= sizeof(captured_receipt)) {
    return -1;
  }
  memcpy(captured_receipt, json, size);
  captured_receipt[size] = '\0';
  return 0;
}

static uintptr_t lookup(nxloader_registry *registry, const char *name,
                        int *priority) {
  nxloader_registry_match match;
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  check(nxloader_registry_lookup(registry, name, &match) == NXLOADER_OK,
        "registry lookup failed");
  if (priority != NULL) {
    *priority = match.priority;
  }
  return match.address;
}

int main(void) {
  nxloader_registry *registry = NULL;
  nxloader_symbol symbols[19];
  nxloader_provider base;
  nxgl_nxloader_provider_config config;
  nxgl_graphics_evidence evidence;
  uintptr_t original_make;
  uintptr_t original_get_proc;
  uintptr_t original_shader;
  uintptr_t unrelated;
  uintptr_t wrapped_make;
  uintptr_t wrapped_get_proc;
  uintptr_t wrapped_shader;
  int priority;
  char receipt[4096];
  char health_directory[] = "/tmp/nxgl-health-test.XXXXXX";
  char health_path[512];
  char health_line[1024];
  char expected_health[1024];
  struct stat health_stat;

  /* These legacy ambient names are deliberately hostile.  Provider API 2
   * must ignore them: only its explicit binding plus bootstrap-owned health
   * identity may reach the release evidence. */
  check(setenv("NXOBS_RUN_ID", "legacy-wrong-run", 1) == 0,
        "set hostile legacy run id");
  check(setenv("NX_GENERATION",
               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
               1) == 0, "set hostile legacy generation");
  check(setenv("NX_ARTIFACT_SHA256",
               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
               1) == 0, "set hostile legacy artifact");
  check(setenv("NX_FRAMEWORK_COMMIT", "wrong", 1) == 0,
        "set hostile legacy commit");
  check(setenv("NX_CFW", "wrong-cfw", 1) == 0,
        "set hostile legacy cfw");
  check(setenv("NX_DEVICE", "wrong-device", 1) == 0,
        "set hostile legacy device");
  check(setenv("NX_PORT_ID", "wrong-port", 1) == 0,
        "set hostile legacy port");
  check(setenv("NX_PORT_VERSION", "wrong-version", 1) == 0,
        "set hostile legacy version");
  check(mkdtemp(health_directory) != NULL, "create private health directory");
  check(snprintf(health_path, sizeof(health_path),
                 "%s/health-fixture-provider-test-1.json",
                 health_directory) > 0,
        "health path");
  check(setenv("NXBOOTSTRAP_HEALTH_FILE", health_path, 1) == 0,
        "set health file");
  check(setenv("NXBOOTSTRAP_HEALTH_SCHEMA",
               "org.nextos.nxruntime.health", 1) == 0,
        "set health schema");
  check(setenv("NXBOOTSTRAP_HEALTH_SCHEMA_VERSION", "1", 1) == 0,
        "set health schema version");
  check(setenv("NXBOOTSTRAP_HEALTH_RUN_ID", "provider-test-1", 1) == 0,
        "set health run id");
  check(setenv("NXBOOTSTRAP_HEALTH_GENERATION",
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
               1) == 0,
        "set health generation");
  check(setenv("NXBOOTSTRAP_HEALTH_PORT_ID", "fixture", 1) == 0,
        "set health port");

  memset(symbols, 0, sizeof(symbols));
#define SYMBOL(index, symbol_name, function, type)                             \
  do {                                                                         \
    symbols[index].name = symbol_name;                                         \
    symbols[index].address = FN_ADDRESS(function, type);                       \
  } while (0)
  SYMBOL(0, "eglMakeCurrent", fake_egl_make_current,
         unsigned (*)(void *, void *, void *, void *));
  SYMBOL(1, "eglGetProcAddress", fake_egl_get_proc_address,
         void *(*)(const char *));
  SYMBOL(2, "glCreateShader", fake_gl_create_shader, unsigned (*)(unsigned));
  SYMBOL(3, "glGetString", fake_gl_get_string,
         const unsigned char *(*)(unsigned));
  SYMBOL(4, "glShaderSource", fake_gl_shader_source,
         void (*)(unsigned, int, const char *const *, const int *));
  SYMBOL(5, "glCompileShader", fake_gl_compile_shader, void (*)(unsigned));
  SYMBOL(6, "glGetShaderiv", fake_gl_get_shader_iv,
         void (*)(unsigned, unsigned, int *));
  SYMBOL(7, "glCreateProgram", fake_gl_create_program, unsigned (*)(void));
  SYMBOL(8, "glAttachShader", fake_gl_attach_shader,
         void (*)(unsigned, unsigned));
  SYMBOL(9, "glLinkProgram", fake_gl_link_program, void (*)(unsigned));
  SYMBOL(10, "glGetProgramiv", fake_gl_get_program_iv,
         void (*)(unsigned, unsigned, int *));
  SYMBOL(11, "glDeleteShader", fake_gl_delete_shader, void (*)(unsigned));
  SYMBOL(12, "glDeleteProgram", fake_gl_delete_program, void (*)(unsigned));
  SYMBOL(13, "SDL_GL_GetDrawableSize", fake_sdl_drawable_size,
         void (*)(void *, int *, int *));
  SYMBOL(14, "SDL_GL_GetCurrentWindow", fake_sdl_current_window,
         void *(*)(void));
  SYMBOL(15, "SDL_GL_GetAttribute", fake_sdl_get_attribute,
         int (*)(int, int *));
  SYMBOL(16, "SDL_PumpEvents", fake_sdl_pump_events, void (*)(void));
  SYMBOL(17, "eglQueryString", fake_egl_query_string,
         const char *(*)(void *, int));
  SYMBOL(18, "eglGetCurrentDisplay", fake_egl_get_current_display,
         void *(*)(void));
#undef SYMBOL

  check(nxloader_registry_create(&registry) == NXLOADER_OK, "registry create");
  memset(&base, 0, sizeof(base));
  base.struct_size = sizeof(base);
  base.name = "fake-host";
  base.symbols = symbols;
  base.symbol_count = sizeof(symbols) / sizeof(symbols[0]);
  base.priority = 100;
  check(nxloader_registry_add_provider(registry, &base, NULL) == NXLOADER_OK,
        "base provider");
  original_make = lookup(registry, "eglMakeCurrent", NULL);
  original_get_proc = lookup(registry, "eglGetProcAddress", NULL);
  original_shader = lookup(registry, "glCreateShader", NULL);
  unrelated = lookup(registry, "glGetString", NULL);

  memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.api_version = NXGL_NXLOADER_PROVIDER_API_VERSION;
  check(nxgl_nxloader_provider_install(registry, &config) == NXLOADER_OK,
        "disabled install is no-op");
  check(lookup(registry, "eglMakeCurrent", NULL) == original_make &&
            lookup(registry, "eglGetProcAddress", NULL) == original_get_proc &&
            lookup(registry, "glCreateShader", NULL) == original_shader,
        "disabled install changed registry");

  config.flags = NXGL_NXLOADER_PROVIDER_GRAPHICS_EVIDENCE;
  config.backend = NXGL_NXLOADER_BACKEND_EGL;
  check(nxgl_graphics_contract_default(&config.contract) == 0,
        "contract default");
  config.receipt_sink = capture_receipt;
  check(nxgl_nxloader_provider_install(registry, &config) == NXLOADER_EINVAL,
        "enabled provider accepted an absent explicit evidence binding");
  memset(&config.binding, 0, sizeof(config.binding));
  config.binding.struct_size = sizeof(config.binding);
  config.binding.api_version = NXGL_NXLOADER_EVIDENCE_BINDING_API_VERSION;
  (void)snprintf(config.binding.framework_commit,
                 sizeof(config.binding.framework_commit), "deadbeef");
  (void)snprintf(config.binding.cfw, sizeof(config.binding.cfw), "test-cfw");
  (void)snprintf(config.binding.device, sizeof(config.binding.device),
                 "test-mali-450");
  (void)snprintf(config.binding.port_version,
                 sizeof(config.binding.port_version), "1.0");
  (void)snprintf(config.binding.artifact_sha256,
                 sizeof(config.binding.artifact_sha256), "%s",
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  check(nxgl_nxloader_provider_install(registry, &config) == NXLOADER_OK,
        "enabled provider install");
  wrapped_make = lookup(registry, "eglMakeCurrent", &priority);
  check(wrapped_make != original_make && priority == 101,
        "MakeCurrent not narrowly overridden");
  wrapped_get_proc = lookup(registry, "eglGetProcAddress", NULL);
  wrapped_shader = lookup(registry, "glCreateShader", NULL);
  check(wrapped_get_proc != original_get_proc &&
            wrapped_shader != original_shader &&
            lookup(registry, "glGetString", NULL) == unrelated,
        "provider changed wrong registry surface");
  check(nxgl_nxloader_provider_mark_ready() == NXLOADER_ESTATE,
        "ready cannot be published before graphics evidence passes");

  /* A live renderer without the bootstrap-owned generation binding is a
   * diagnostic failure, never a releasable receipt. Run it in a child so the
   * parent's one-shot barrier remains armed for the positive case. */
  {
    pid_t child = fork();
    int child_status = 0;
    check(child >= 0, "fork incomplete-evidence fixture");
    if (child == 0) {
      unsigned (*make_current)(void *, void *, void *, void *) = NULL;
      nxgl_graphics_evidence child_evidence;
      char child_receipt[4096];
      (void)unsetenv("NXBOOTSTRAP_HEALTH_GENERATION");
      memcpy(&make_current, &wrapped_make, sizeof(make_current));
      if (make_current(NULL, NULL, NULL, (void *)(uintptr_t)3u) != 0u ||
          nxgl_nxloader_provider_get_status(
              &child_evidence, child_receipt, sizeof(child_receipt)) !=
              NXGL_NXLOADER_PROVIDER_FAIL ||
          child_evidence.verdict != NXGL_GRAPHICS_EVIDENCE_INCOMPLETE ||
          strstr(child_receipt,
                 "\"reason\":\"evidence-incomplete\"") == NULL) {
        _exit(1);
      }
      _exit(0);
    }
    check(waitpid(child, &child_status, 0) == child &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "incomplete evidence did not fail closed");
  }

  {
    unsigned (*make_current)(void *, void *, void *, void *) = NULL;
    memcpy(&make_current, &wrapped_make, sizeof(make_current));
    check(make_current(NULL, NULL, NULL, (void *)(uintptr_t)3u) == 1u,
          "wrapped MakeCurrent failed");
  }
  check(make_current_calls == 1, "original MakeCurrent call count");
  check(receipt_calls == 1 && captured_receipt[0] == '{',
        "JSON receipt was not sunk once");
  check(strstr(captured_receipt,
               "\"schema\":\"nx-graphics-evidence\"") != NULL &&
            strstr(captured_receipt, "\"schema_version\":2") != NULL &&
            strstr(captured_receipt, "\"verdict\":\"OK\"") != NULL &&
            strstr(captured_receipt,
                   "\"run_id\":\"provider-test-1\"") != NULL &&
            strstr(captured_receipt,
                   "\"device\":\"test-mali-450\"") != NULL &&
            strstr(captured_receipt,
                   "\"artifact_sha256\":\"aaaaaaaaaaaaaaaa") != NULL &&
            strstr(captured_receipt, "legacy-wrong") == NULL &&
            strstr(captured_receipt, "wrong-device") == NULL,
        "receipt content");
  check(nxgl_nxloader_provider_get_status(&evidence, receipt,
                                           sizeof(receipt)) ==
            NXGL_NXLOADER_PROVIDER_PASS &&
            evidence.verdict == NXGL_GRAPHICS_OK &&
            strcmp(receipt, captured_receipt) == 0,
        "provider status snapshot");
  check(create_shader_calls == 2 && delete_shader_calls == 2 &&
            delete_program_calls == 1,
        "probe object lifecycle");

  {
    unsigned (*create_shader)(unsigned) = NULL;
    memcpy(&create_shader, &wrapped_shader, sizeof(create_shader));
    check(create_shader(0x8B31u) != 0u && create_shader_calls == 3,
          "passed shader barrier did not forward once");
  }
  {
    void *(*get_proc)(const char *) = NULL;
    void *dynamic_shader;
    memcpy(&get_proc, &wrapped_get_proc, sizeof(get_proc));
    dynamic_shader = get_proc("glCreateShader");
    check(dynamic_shader != NULL, "GetProcAddress did not return shader wrapper");
    check(get_proc("glGetString") != NULL, "unknown GetProcAddress not forwarded");
  }
  check(receipt_calls == 1, "barrier probed more than once");

  /* Readiness is a distinct lifecycle decision. A planted symlink is refused
   * in a child (provider state is copy-on-write), then the parent publishes
   * one exact 0600 receipt atomically. */
  check(symlink("/dev/null", health_path) == 0, "plant health symlink");
  {
    pid_t child = fork();
    int child_status = 0;
    check(child >= 0, "fork health symlink fixture");
    if (child == 0) {
      _exit(nxgl_nxloader_provider_mark_ready() == NXLOADER_EIO ? 0 : 1);
    }
    check(waitpid(child, &child_status, 0) == child &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "health symlink was not rejected");
  }
  check(unlink(health_path) == 0, "remove fixture symlink");
  check(nxgl_nxloader_provider_mark_ready() == NXLOADER_OK,
        "explicit lifecycle ready publication");
  check(lstat(health_path, &health_stat) == 0 &&
            S_ISREG(health_stat.st_mode) &&
            (health_stat.st_mode & 0777) == 0600 &&
            health_stat.st_nlink == 1,
        "health receipt is real, 0600 and single-link");
  {
    FILE *health = fopen(health_path, "r");
    check(health != NULL, "open published health receipt");
    check(fgets(health_line, sizeof(health_line), health) != NULL,
          "read health receipt");
    check(fgets(receipt, sizeof(receipt), health) == NULL,
          "health receipt has exactly one line");
    check(fclose(health) == 0, "close health receipt");
  }
  check(snprintf(
            expected_health, sizeof(expected_health),
            "{\"schema\":\"org.nextos.nxruntime.health\","
            "\"schema_version\":1,\"run_id\":\"provider-test-1\","
            "\"generation\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaa\",\"port_id\":\"fixture\","
            "\"status\":\"ready\"}\n") > 0 &&
            strcmp(health_line, expected_health) == 0,
        "health JSON is the exact nxbootstrap 0.6.32 contract");
  check(nxgl_nxloader_provider_mark_ready() == NXLOADER_ESTATE,
        "ready receipt cannot be published twice");
  check(unlink(health_path) == 0 && rmdir(health_directory) == 0,
        "clean health fixture");

  nxloader_registry_destroy(registry);
  (void)puts("nxgl_nxloader_provider tests: ok "
             "(default-off/allowlist/originals/evidence/one-shot/shader-barrier/"
             "explicit-atomic-health-ready)");
  return 0;
}
