/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "ct_framework.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxaudio.h"
#include "nxcompat.h"
#include "nxgl.h"
#include "nxgl_nxcompat.h"
#include "nxinput.h"
#include "nxinput_nxcompat.h"
#include "nxloader.h"

#include "ct_platform.h"
#include "util.h"

#define CT_RUNTIME_JSON_MAX 32768u

struct ct_framework {
  nxcompat_host host;
  nxcompat_probe_result probe;
  nxcompat_plan_v2 plan;
  nxcompat_registry *capabilities;
  nxcompat_requirements requirements;

  nxgl_context *graphics;
  nxgl_report graphics_report;
  nxgl_present_policy present_policy;
  nxinput_context *input;

  nxloader_registry *loader_registry;
  nxloader_module *libcxx;
  nxloader_module *game;
  nxloader_symbol *host_symbols;
  size_t host_symbol_count;
  uintptr_t guest_mapping_base;
  size_t guest_mapping_size;
  int game_started;
};

static ct_framework *ct_active_framework;

static void ct_status(void *userdata, nxcompat_status_kind kind,
                      const char *message) {
  (void)userdata;
  debugPrintf("NXCOMPAT[%d] %s\n", (int)kind, message ? message : "");
}

static void ct_gl_status(void *userdata, nxgl_status_kind kind,
                         const char *message) {
  (void)userdata;
  debugPrintf("NXGL[%d] %s\n", (int)kind, message ? message : "");
}

static void ct_loader_log(void *userdata, nxloader_log_level level,
                          const char *message) {
  (void)userdata;
  debugPrintf("NXLOADER[%d] %s\n", (int)level, message ? message : "");
}

static int ct_requirements_ok(ct_framework *framework, nxcompat_phase phase) {
  nxcompat_requirement_report report;
  nxcompat_result_code result;
  memset(&report, 0, sizeof(report));
  result = nxcompat_requirements_evaluate(framework->capabilities,
                                          &framework->requirements, phase,
                                          &report);
  if (result != NXCOMPAT_OK) {
    debugPrintf("NXCOMPAT requirements phase=%s failed rc=%d\n",
                nxcompat_phase_name(phase), (int)result);
    return -1;
  }
  debugPrintf("NXCOMPAT requirements phase=%s satisfied=%zu pending=%zu "
              "missing=%zu\n", nxcompat_phase_name(phase),
              report.satisfied_count, report.pending_count,
              report.missing_count);
  return report.missing_count == 0u ? 0 : -1;
}

static void ct_log_runtime_report(ct_framework *framework,
                                  nxcompat_phase phase) {
  nxcompat_runtime_report report;
  char *json;
  if (!framework || !framework->capabilities)
    return;
  memset(&report, 0, sizeof(report));
  if (nxcompat_registry_runtime_report(framework->capabilities,
                                       &framework->requirements, phase,
                                       &report) != NXCOMPAT_OK)
    return;
  json = (char *)malloc(CT_RUNTIME_JSON_MAX);
  if (!json)
    return;
  if (nxcompat_format_runtime_json(&framework->host, &framework->plan,
                                   &report, json,
                                   CT_RUNTIME_JSON_MAX) >= 0)
    debugPrintf("NXCOMPAT_REPORT %s\n", json);
  free(json);
}

ct_framework *ct_framework_create(void) {
  ct_framework *framework = (ct_framework *)calloc(1u, sizeof(*framework));
  if (framework)
    ct_active_framework = framework;
  return framework;
}

void ct_framework_destroy(ct_framework *framework) {
  if (!framework)
    return;
  if (ct_active_framework == framework)
    ct_active_framework = NULL;
  ct_framework_close_input(framework);
  ct_framework_close_graphics(framework);
  nxcompat_registry_destroy(framework->capabilities);
  nxloader_registry_destroy(framework->loader_registry);
  if (!framework->game_started) {
    nxloader_module_destroy(framework->game);
    nxloader_module_destroy(framework->libcxx);
  }
  free(framework->host_symbols);
  free(framework);
}

int ct_framework_preflight(ct_framework *framework, const char *game_dir) {
  nxcompat_probe_options probe_options;
  nxcompat_plan_options plan_options;
  nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
  const char *port_id;
  if (!framework || !game_dir || game_dir[0] != '/')
    return -1;
  port_id = getenv("NXCOMPAT_PORT_ID");
  if (!port_id || !port_id[0])
    port_id = "chrono";
  memset(&probe_options, 0, sizeof(probe_options));
  probe_options.api_version = NXCOMPAT_API_VERSION;
  probe_options.struct_size = sizeof(probe_options);
  probe_options.port_id = port_id;
  probe_options.game_dir = game_dir;
  probe_options.portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");
  probe_options.result = &framework->probe;
  if (nxcompat_probe(&probe_options, &framework->host) != 0) {
    debugPrintf("NXCOMPAT probe failed\n");
    return -1;
  }
  memset(&plan_options, 0, sizeof(plan_options));
  plan_options.api_version = NXCOMPAT_API_VERSION;
  plan_options.struct_size = sizeof(plan_options);
  plan_options.runtime_arch = NXCOMPAT_ARCH_AARCH64;
  plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE;
  if (nxcompat_plan_environment_v2(&framework->host, &plan_options,
                                   &framework->plan) != NXCOMPAT_OK ||
      nxcompat_apply_environment_v2(&framework->plan) != NXCOMPAT_OK) {
    debugPrintf("NXCOMPAT environment plan/apply failed reason=%s\n",
                nxcompat_reason_name(framework->plan.final_reason));
    return -1;
  }
  if (nxcompat_registry_create(&framework->capabilities) != NXCOMPAT_OK ||
      nxcompat_registry_seed_host(framework->capabilities,
                                  &framework->host) != NXCOMPAT_OK ||
      nxcompat_requirements_parse_runtime_ex(&framework->requirements,
                                              &reason) != NXCOMPAT_OK) {
    debugPrintf("NXCOMPAT registry/requirements failed reason=%s\n",
                nxcompat_reason_name(reason));
    return -1;
  }
  if (ct_requirements_ok(framework, NXCOMPAT_PHASE_PREFLIGHT) != 0)
    return -1;
  ct_log_runtime_report(framework, NXCOMPAT_PHASE_PREFLIGHT);
  return 0;
}

static SDL_GLContext ct_create_context_guarded(void *userdata,
                                                SDL_Window *window) {
  unsigned long thread_pointer;
  unsigned long guard;
  SDL_GLContext context;
  (void)userdata;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
  guard = *(unsigned long *)(thread_pointer + 0x28u);
  context = SDL_GL_CreateContext(window);
  *(unsigned long *)(thread_pointer + 0x28u) = guard;
  return context;
}

static int ct_make_current_guarded(void *userdata, SDL_Window *window,
                                   SDL_GLContext context) {
  unsigned long thread_pointer;
  unsigned long guard;
  int result;
  (void)userdata;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
  guard = *(unsigned long *)(thread_pointer + 0x28u);
  result = SDL_GL_MakeCurrent(window, context);
  *(unsigned long *)(thread_pointer + 0x28u) = guard;
  return result;
}

static void ct_delete_context_guarded(void *userdata,
                                      SDL_GLContext context) {
  unsigned long thread_pointer;
  unsigned long guard;
  (void)userdata;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
  guard = *(unsigned long *)(thread_pointer + 0x28u);
  SDL_GL_DeleteContext(context);
  *(unsigned long *)(thread_pointer + 0x28u) = guard;
}

static int ct_present_guarded(void *userdata, SDL_Window *window,
                              SDL_GLContext context, char *error,
                              size_t error_size) {
  unsigned long thread_pointer;
  unsigned long guard;
  (void)userdata;
  (void)context;
  if (!window) {
    if (error && error_size)
      snprintf(error, error_size, "missing-window");
    return -1;
  }
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
  guard = *(unsigned long *)(thread_pointer + 0x28u);
  SDL_GL_SwapWindow(window);
  *(unsigned long *)(thread_pointer + 0x28u) = guard;
  if (error && error_size)
    error[0] = '\0';
  return 0;
}

int ct_framework_open_graphics(ct_framework *framework,
                               SDL_Window **window, int *drawable_width,
                               int *drawable_height) {
  static const nxgl_config_candidate candidates[] = {
      {2, 0, 8, 8, 8, 8, 24, 8, 1},
      {2, 0, 8, 8, 8, 8, 16, 0, 1},
      {2, 0, 8, 8, 8, 8, 0, 0, 1},
      {2, 0, 8, 8, 8, 0, 24, 8, 1},
      {2, 0, 8, 8, 8, 0, 16, 0, 1},
      {2, 0, 8, 8, 8, 0, 0, 0, 1},
  };
  nxgl_resolution_sources facts;
  nxgl_engine_requirements requirements;
  nxgl_context_ops context_ops;
  nxgl_open_options options;
  nxcompat_graphics_receipt receipt;
  if (!framework || !framework->capabilities || !window || !drawable_width ||
      !drawable_height)
    return -1;
  nxgl_resolution_sources_init(&facts);
  if (nxgl_nxcompat_resolution_sources(&framework->host, &facts) !=
      NXGL_SUCCESS)
    return -1;
  nxgl_engine_requirements_init(&requirements);
  requirements.minimum_gles_major = 2;
  requirements.minimum_gles_minor = 0;
  requirements.minimum_red_bits = 8;
  requirements.minimum_green_bits = 8;
  requirements.minimum_blue_bits = 8;
  /* The original working ladder requested double buffering but never made
   * SDL_GL_DOUBLEBUFFER a hard acceptance condition.  ArkOS/KMSDRM exposes a
   * working swap path as EGL_SINGLE_BUFFER, so keep requesting 1 in every
   * candidate while accepting the delivered mode for this engine. */
  requirements.require_double_buffer = 0;
  memset(&context_ops, 0, sizeof(context_ops));
  context_ops.api_version = NXGL_API_VERSION;
  context_ops.struct_size = sizeof(context_ops);
  context_ops.create_context = ct_create_context_guarded;
  context_ops.make_current = ct_make_current_guarded;
  context_ops.delete_context = ct_delete_context_guarded;
  nxgl_open_options_init(&options);
  options.flags = NXGL_OPEN_INITIALIZE_VIDEO |
                  NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
                  NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE;
  options.window_title = "Chrono Trigger";
  options.window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                         SDL_WINDOW_FULLSCREEN;
  options.display_index = 0;
  options.fallback_facts = &facts;
  options.requirements = &requirements;
  options.candidates = candidates;
  options.candidate_count = sizeof(candidates) / sizeof(candidates[0]);
  options.context_ops = &context_ops;
  options.status = ct_gl_status;
  if (nxgl_open(&options, &framework->graphics,
                &framework->graphics_report) != NXGL_SUCCESS)
    return -1;
  *window = nxgl_window(framework->graphics);
  *drawable_width = framework->graphics_report.drawable_width;
  *drawable_height = framework->graphics_report.drawable_height;
  if (!*window || *drawable_width <= 0 || *drawable_height <= 0)
    return -1;
  memset(&receipt, 0, sizeof(receipt));
  if (nxgl_nxcompat_publish_context(framework->capabilities,
                                    framework->graphics, 1u,
                                    &receipt) != NXCOMPAT_OK ||
      ct_requirements_ok(framework, NXCOMPAT_PHASE_GRAPHICS) != 0)
    return -1;
  nxgl_present_policy_init(&framework->present_policy);
  framework->present_policy.owner = NXGL_PRESENT_ADAPTER;
  framework->present_policy.adapter = ct_present_guarded;
  if (framework->graphics_report.actual.alpha_bits > 0)
    framework->present_policy.flags |=
        NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  if (ct_needs_finish_before_swap())
    framework->present_policy.flags |= NXGL_PRESENT_FINISH_BEFORE_SWAP;
  ct_log_runtime_report(framework, NXCOMPAT_PHASE_GRAPHICS);
  return 0;
}

int ct_framework_present(ct_framework *framework) {
  if (!framework || !framework->graphics)
    return -1;
  return nxgl_present(framework->graphics, &framework->present_policy) ==
                 NXGL_SUCCESS
             ? 0
             : -1;
}

void ct_framework_close_graphics(ct_framework *framework) {
  if (!framework || !framework->graphics)
    return;
  nxgl_close(framework->graphics);
  framework->graphics = NULL;
}

int ct_framework_open_input(ct_framework *framework) {
  nxinput_config config;
  nxcompat_input_receipt receipt;
  if (!framework || !framework->capabilities)
    return -1;
  nxinput_config_init(&config);
  framework->input = nxinput_create(&config);
  if (!framework->input)
    return -1;
  memset(&receipt, 0, sizeof(receipt));
  if (nxinput_nxcompat_publish_context(framework->capabilities,
                                       framework->input,
                                       &receipt) != NXCOMPAT_OK ||
      ct_requirements_ok(framework, NXCOMPAT_PHASE_INPUT) != 0)
    return -1;
  ct_log_runtime_report(framework, NXCOMPAT_PHASE_INPUT);
  return 0;
}

void ct_framework_observe_input(ct_framework *framework,
                                const SDL_Event *event) {
  if (!framework || !framework->input || !event)
    return;
  nxinput_observe_event(framework->input, event);
  if (event->type == SDL_CONTROLLERDEVICEADDED ||
      event->type == SDL_CONTROLLERDEVICEREMOVED ||
      event->type == SDL_CONTROLLERDEVICEREMAPPED) {
    nxcompat_input_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    (void)nxinput_nxcompat_publish_context(framework->capabilities,
                                           framework->input, &receipt);
  }
}

void ct_framework_poll_input(ct_framework *framework) {
  if (framework && framework->input)
    nxinput_poll(framework->input);
}

int ct_framework_has_controller(const ct_framework *framework) {
  return framework && framework->input &&
         nxinput_connected_count(framework->input) != 0u;
}

SDL_GameController *ct_framework_sdl_controller(const ct_framework *framework) {
  int slot;
  if (!framework || !framework->input)
    return NULL;
  slot = nxinput_first_connected(framework->input);
  if (slot < 0)
    return NULL;
  return nxinput_pad_sdl_controller(framework->input, (unsigned int)slot);
}

int ct_framework_consume_quit(ct_framework *framework) {
  return framework && framework->input
             ? nxinput_consume_quit_request(framework->input)
             : 0;
}

void ct_framework_close_input(ct_framework *framework) {
  if (!framework || !framework->input)
    return;
  nxinput_destroy(framework->input);
  framework->input = NULL;
}

void ct_framework_audio_device_opened(unsigned int device_id, int frequency,
                                      unsigned int format,
                                      unsigned int channels,
                                      unsigned int samples) {
  ct_framework *framework = ct_active_framework;
  nxcompat_audio_receipt receipt;
  nxaudio_backend_observation observation;
  nxaudio_reason reason = NXAUDIO_REASON_NONE;
  const char *backend;
  if (!framework || !framework->capabilities || device_id == 0u)
    return;
  backend = SDL_GetCurrentAudioDriver();
  memset(&observation, 0, sizeof(observation));
  observation.api_version = NXAUDIO_API_VERSION;
  observation.struct_size = sizeof(observation);
  if (backend)
    snprintf(observation.backend, sizeof(observation.backend), "%s", backend);
  observation.inherited_attempt =
      framework->host.inherited_audio_driver[0] != '\0';
  observation.server_reachable = 1;
  observation.device_opened = 1;
  if (nxaudio_classify_backend(&observation, &reason) != NXAUDIO_OK) {
    debugPrintf("NXAUDIO output rejected reason=%d\n", (int)reason);
    return;
  }
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                        NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                        NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
  receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
  receipt.generation = 1u;
  receipt.lifetime = NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED;
  receipt.frequency = frequency;
  receipt.format = format;
  receipt.channels = channels;
  receipt.samples = samples;
  receipt.device_id_was_nonzero = 1;
  if (backend)
    snprintf(receipt.backend, sizeof(receipt.backend), "%s", backend);
  if (nxcompat_registry_publish_audio(framework->capabilities, &receipt) ==
      NXCOMPAT_OK) {
    debugPrintf("NXAUDIO output-open backend=%s %dHz %uch %u samples reason=%d\n",
                receipt.backend, frequency, channels, samples, (int)reason);
    ct_log_runtime_report(framework, NXCOMPAT_PHASE_AUDIO);
  }
}

void ct_framework_audio_device_failed(void) {
  debugPrintf("NXAUDIO device-open failed\n");
}

static const char *const ct_explicit_host_symbols[] = {
    "__cxa_thread_atexit_impl", "chdir", "fchmod", "fchmodat",
    "fdopendir", "fputwc", "getc", "getcwd", "getwc", "isatty",
    "iswalpha_l", "iswblank_l", "iswcntrl_l", "iswdigit_l",
    "iswlower_l", "iswprint_l", "iswpunct_l", "iswspace_l",
    "iswupper_l", "iswxdigit_l", "link", "openat",
    "pathconf", "readlink", "realpath", "sendfile", "statvfs",
    "strcoll_l", "strftime_l", "strxfrm_l", "symlink", "towlower_l",
    "towupper_l", "truncate", "ungetc", "ungetwc", "unlinkat",
    "utimensat", "wcscoll_l", "wcsxfrm_l", "__memset_chk", "asinf",
    "atan2", "atol", "bsearch", "eglGetProcAddress", "exp2f", "frexp",
    "glBlendEquation", "glBlendFuncSeparate", "glFrontFace",
    "glGetActiveAttrib", "glGetActiveUniform", "glGetAttribLocation",
    "glGetBooleanv", "glGetError", "glGetFloatv", "glGetShaderSource",
    "glIsBuffer", "glIsEnabled", "glIsRenderbuffer", "glLineWidth",
    "glMapBufferOES", "glPixelStorei", "glUniform1f", "glUniform2f",
    "glUniform2i", "glUniform2iv", "glUniform3f", "glUniform3i",
    "glUniform3iv", "glUniform4f", "glUniform4i", "glUniform4iv",
    "glUnmapBufferOES", "gmtime", "longjmp", "modff",
    "pthread_cond_init", "putchar", "setjmp", "sqrt", "sqrtf",
};

/* Mesa/Panfrost (ROCKNIX no RG-DS, log de 05/09): a libGLESv2.so.2 do Mesa
 * NAO exporta eglGetProcAddress nem gl{Map,Unmap}BufferOES, e a libEGL e'
 * aberta pela SDL com RTLD_LOCAL -- dlsym(RTLD_DEFAULT) nao a enxerga. Nos
 * blobs ARM (Mali) tudo vive na libMali e o dlsym basta. O contexto GL ja esta
 * aberto quando o registry nasce, entao SDL_GL_GetProcAddress (que no Mesa vai
 * ao eglGetProcAddress real) e' a segunda fonte; libEGL.so.1 por dlopen, a
 * terceira. Se mesmo assim faltar, o import STRONG da libchrono recebe um
 * adaptador nosso em vez de derrubar o carregamento inteiro. */
static void *ct_host_gl_proc(const char *name) {
  static void *egl_handle;
  void *address = NULL;
  if (!name || !*name)
    return NULL;
  if (!getenv("CHRONO_GLPROC_FORCE_SDL"))
    address = dlsym(RTLD_DEFAULT, name);
  if (!address)
    address = SDL_GL_GetProcAddress(name);
  if (!address) {
    if (!egl_handle)
      egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (egl_handle)
      address = dlsym(egl_handle, name);
  }
  return address;
}

static void *ct_eglGetProcAddress_adapter(const char *name) {
  return ct_host_gl_proc(name);
}

static void *ct_glMapBufferOES_stub(unsigned int target, unsigned int access) {
  static int logged;
  (void)target; (void)access;
  if (!logged++)
    debugPrintf("NXLOADER glMapBufferOES indisponivel no host: devolvendo NULL\n");
  return NULL;
}

static unsigned char ct_glUnmapBufferOES_stub(unsigned int target) {
  (void)target;
  return 0;
}

static int ct_symbol_is_gl(const char *name) {
  return (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') ||
         (name[0] == 'e' && name[1] == 'g' && name[2] == 'l' && name[3] >= 'A' &&
          name[3] <= 'Z');
}

static void *ct_explicit_symbol_fallback(const char *name) {
  if (!strcmp(name, "eglGetProcAddress"))
    return (void *)&ct_eglGetProcAddress_adapter;
  if (!strcmp(name, "glMapBufferOES"))
    return (void *)&ct_glMapBufferOES_stub;
  if (!strcmp(name, "glUnmapBufferOES"))
    return (void *)&ct_glUnmapBufferOES_stub;
  return NULL;
}

static int ct_loader_registry_create(ct_framework *framework,
                                     const DynLibFunction *imports,
                                     size_t import_count) {
  nxloader_provider provider;
  nxloader_registry_report report;
  size_t extra_count = sizeof(ct_explicit_host_symbols) /
                       sizeof(ct_explicit_host_symbols[0]);
  size_t added_count = 0u;
  size_t index;
  if (import_count > SIZE_MAX - extra_count)
    return -1;
  framework->host_symbols = (nxloader_symbol *)calloc(
      import_count + extra_count, sizeof(*framework->host_symbols));
  if (!framework->host_symbols)
    return -1;
  for (index = 0; index < import_count; ++index) {
    framework->host_symbols[index].name = imports[index].symbol;
    framework->host_symbols[index].address = imports[index].func;
  }
  for (index = 0; index < extra_count; ++index) {
    const char *name = ct_explicit_host_symbols[index];
    const char *origin = "dlsym";
    void *address = NULL;
    if (ct_symbol_is_gl(name)) {
      address = ct_host_gl_proc(name);
      origin = "gl-proc";
      if (!address) {
        address = ct_explicit_symbol_fallback(name);
        origin = "adapter";
      }
    } else {
      address = dlsym(RTLD_DEFAULT, name);
    }
    if (!address) {
      debugPrintf("NXLOADER optional host symbol unavailable: %s\n", name);
      continue;
    }
    if (strcmp(origin, "dlsym") != 0 &&
        (!strcmp(origin, "adapter") || getenv("CHRONO_GLPROC_FORCE_SDL")))
      debugPrintf("NXLOADER host symbol %s via %s\n", name, origin);
    framework->host_symbols[import_count + added_count].name =
        ct_explicit_host_symbols[index];
    framework->host_symbols[import_count + added_count].address =
        (uintptr_t)address;
    ++added_count;
  }
  framework->host_symbol_count = import_count + added_count;
  if (nxloader_registry_create(&framework->loader_registry) != NXLOADER_OK)
    return -1;
  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "chrono-bionic-host-v1";
  provider.symbols = framework->host_symbols;
  provider.symbol_count = framework->host_symbol_count;
  provider.priority = 100;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  if (nxloader_registry_add_provider(framework->loader_registry, &provider,
                                     &report) != NXLOADER_OK)
    return -1;
  debugPrintf("NXLOADER host provider symbols=%zu added=%zu\n",
              framework->host_symbol_count, report.added);
  return 0;
}

static int ct_loader_module_create(nxloader_module **module) {
  nxloader_config config;
  nxloader_config_init(&config);
  config.expected_arch = NXLOADER_ARCH_AARCH64;
  config.trampoline_pool_size = 64u * 1024u;
  config.log = ct_loader_log;
  return nxloader_module_create(&config, module) == NXLOADER_OK ? 0 : -1;
}

static int ct_loader_prepare(nxloader_module *module, const char *path,
                             const nxloader_registry *registry) {
  nxloader_resolution_report report;
  nxloader_result result;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  result = nxloader_module_load_file(module, path);
  if (result == NXLOADER_OK)
    result = nxloader_module_relocate(module);
  if (result == NXLOADER_OK)
    result = nxloader_module_resolve(module, registry, 0u, &report);
  if (result != NXLOADER_OK) {
    debugPrintf("NXLOADER prepare %s failed=%s unresolved=%s\n", path,
                nxloader_result_string(result),
                report.first_unresolved ? report.first_unresolved : "none");
    return -1;
  }
  debugPrintf("NXLOADER prepared %s imports=%zu weak-zero=%zu\n", path,
              report.imports_resolved, report.weak_imports_zeroed);
  return 0;
}

int ct_framework_load_libcxx(ct_framework *framework, const char *path,
                             const DynLibFunction *imports,
                             size_t import_count) {
  nxloader_registry_report report;
  if (!framework || !path || !imports || import_count == 0u ||
      ct_loader_registry_create(framework, imports, import_count) != 0 ||
      ct_loader_module_create(&framework->libcxx) != 0 ||
      ct_loader_prepare(framework->libcxx, path,
                        framework->loader_registry) != 0 ||
      nxloader_module_finalize(framework->libcxx) != NXLOADER_OK ||
      nxloader_module_call_initializers(framework->libcxx) != NXLOADER_OK)
    return -1;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  if (nxloader_registry_add_module(framework->loader_registry,
                                   framework->libcxx,
                                   "chrono-libcxx-guest-v1", 50,
                                   &report) != NXLOADER_OK)
    return -1;
  debugPrintf("NXLOADER libc++ ready exports=%zu\n", report.added);
  return 0;
}

int ct_framework_load_game(ct_framework *framework, const char *path) {
  nxloader_module_info info;
  if (!framework || !framework->libcxx || !path ||
      ct_loader_module_create(&framework->game) != 0 ||
      ct_loader_prepare(framework->game, path,
                        framework->loader_registry) != 0)
    return -1;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  if (nxloader_module_get_info(framework->game, &info) != NXLOADER_OK)
    return -1;
  framework->guest_mapping_base = (uintptr_t)info.mapping_base;
  framework->guest_mapping_size = info.mapping_size;
  return 0;
}

uintptr_t ct_framework_find_export(ct_framework *framework,
                                   const char *name, int required) {
  uintptr_t address = 0u;
  if (!framework || !framework->game || !name ||
      nxloader_module_find_export(framework->game, name, &address) !=
          NXLOADER_OK) {
    if (required)
      debugPrintf("NXLOADER required export missing: %s\n", name ? name : "");
    return 0u;
  }
  return address;
}

uintptr_t ct_framework_find_active_export(const char *name) {
  return ct_framework_find_export(ct_active_framework, name, 0);
}

uintptr_t ct_framework_active_guest_base(void) {
  return ct_active_framework ? ct_active_framework->guest_mapping_base : 0u;
}

int ct_framework_active_guest_contains(uintptr_t address) {
  uintptr_t base;
  size_t size;
  if (!ct_active_framework)
    return 0;
  base = ct_active_framework->guest_mapping_base;
  size = ct_active_framework->guest_mapping_size;
  return base != 0u && address >= base && address - base < size;
}

int ct_framework_install_hook(ct_framework *framework, uintptr_t target,
                              uintptr_t replacement) {
  if (!framework || !framework->game || target == 0u || replacement == 0u)
    return -1;
  return nxloader_module_install_hook(framework->game, target, replacement,
                                      4u) == NXLOADER_OK
             ? 0
             : -1;
}

int ct_framework_start_game(ct_framework *framework, void *java_vm,
                            int32_t *jni_version) {
  /* libchrono 2.1.4 JNI_OnLoad returns JNI_VERSION_1_4 literally
     (mov w0,#4; movk w0,#1,lsl#16 at vaddr 0x78cd80).  The shim may expose
     newer JNIEnv slots, but the module-local negotiation must accept the
     guest's exact return value rather than infer it from GetVersion(). */
  static const int32_t accepted_versions[] = {0x00010004};
  nxloader_jni_onload_options options;
  if (!framework || !framework->game || !java_vm || !jni_version ||
      nxloader_module_finalize(framework->game) != NXLOADER_OK ||
      nxloader_module_call_initializers(framework->game) != NXLOADER_OK)
    return -1;
  framework->game_started = 1;
  memset(&options, 0, sizeof(options));
  options.struct_size = sizeof(options);
  options.java_vm = java_vm;
  options.accepted_versions = accepted_versions;
  options.accepted_version_count =
      sizeof(accepted_versions) / sizeof(accepted_versions[0]);
  if (nxloader_module_call_jni_onload(framework->game, &options,
                                      jni_version) != NXLOADER_OK)
    return -1;
  debugPrintf("NXLOADER game READY JNI=0x%x\n", (unsigned)*jni_version);
  return 0;
}
