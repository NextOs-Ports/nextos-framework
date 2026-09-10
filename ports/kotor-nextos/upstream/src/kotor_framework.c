/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "kotor_framework.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nxaudio.h"
#include "nxandroid.h"
#include "nxcompat.h"
#include "nxgl.h"
#include "nxinput.h"
#include "nxinput_nxcompat.h"
#include "nxloader.h"

#include "util.h"

#define KOTOR_RUNTIME_JSON_MAX 32768u
#define KOTOR_NO_MODULE NXANDROID_NO_MODULE

typedef struct kotor_framework_state {
  nxcompat_host host;
  nxcompat_probe_result probe;
  nxcompat_plan_v2 plan;
  nxcompat_registry *registry;
  nxcompat_requirements requirements;
  nxinput_context *input;
  nxandroid_context *android;
  int initialized;
  int input_published;
  int graphics_published;
  int audio_published;
  int ready_reported;

  int step_armed;
  nxandroid_phase expected_phase;
  size_t expected_module;
  int (*delegated_entry)(int, char **);
  int delegated_argc;
  char **delegated_argv;
  int delegated_result;
} kotor_framework_state;

static kotor_framework_state kotor_framework;

static const nxandroid_module_spec kotor_modules[] = {
    {"kotor-lzma", NXANDROID_JNI_NONE},
    {"kotor-miniz", NXANDROID_JNI_NONE},
    {"kotor-freetype", NXANDROID_JNI_NONE},
    {"kotor-fmod", NXANDROID_JNI_REQUIRED},
    {"kotor-android-port", NXANDROID_JNI_NONE},
    {"kotor-main", NXANDROID_JNI_NONE},
};

#define KOTOR_STEP(phase_value, module_value, cycle_value, contract_value)    \
  {                                                                           \
    phase_value, module_value, cycle_value, contract_value, NULL,             \
        NXANDROID_TERMINAL_NONE, 0u, 0u                                       \
  }

static const nxandroid_step kotor_steps[] = {
    KOTOR_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u,
               "kotor-lzma-initialized-v1"),
    KOTOR_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 1u, 0u,
               "kotor-miniz-initialized-v1"),
    KOTOR_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 2u, 0u,
               "kotor-freetype-initialized-v1"),
    KOTOR_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 3u, 0u,
               "kotor-fmod-initialized-v1"),
    KOTOR_STEP(NXANDROID_PHASE_MODULE_JNI, 3u, 0u,
               "kotor-fmod-jni-onload-positive-v1"),
    KOTOR_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 4u, 0u,
               "kotor-android-port-initialized-v1"),
    KOTOR_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 5u, 0u,
               "kotor-main-initialized-v1"),
    KOTOR_STEP(NXANDROID_PHASE_ACTIVITY_CREATE, KOTOR_NO_MODULE, 0u,
               "kotor-native-create-mutex-activity-v1"),
    KOTOR_STEP(NXANDROID_PHASE_RESUME, KOTOR_NO_MODULE, 1u,
               "kotor-native-on-resume-epoch-1-v1"),
    KOTOR_STEP(NXANDROID_PHASE_RUNTIME_DELEGATED, KOTOR_NO_MODULE, 0u,
               "kotor-obb-sdl-main-delegated-runtime-v1"),
};

static void kotor_copy_text(char *output, size_t output_size,
                            const char *input) {
  if (!output || output_size == 0u)
    return;
  if (!input)
    input = "";
  snprintf(output, output_size, "%s", input);
}

static void kotor_loader_log(void *userdata, nxloader_log_level level,
                             const char *message) {
  (void)userdata;
  debugPrintf("NXLOADER[%d] %s\n", (int)level, message ? message : "");
}

static int kotor_validate_guest(const char *path) {
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_module_info info;
  nxloader_result result;
  nxloader_config_init(&config);
  config.expected_arch = NXLOADER_ARCH_ARMV7;
  config.log = kotor_loader_log;
  result = nxloader_module_create(&config, &module);
  if (result == NXLOADER_OK)
    result = nxloader_module_load_file(module, path);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  if (result == NXLOADER_OK)
    result = nxloader_module_get_info(module, &info);
  if (result == NXLOADER_OK)
    debugPrintf("NXLOADER validated %s arch=%d segments=%zu needed=%zu\n",
                path, (int)info.arch, info.segment_count, info.needed_count);
  else
    debugPrintf("NXLOADER rejected %s rc=%d\n", path, (int)result);
  nxloader_module_destroy(module);
  return result == NXLOADER_OK ? 0 : -1;
}

static int kotor_requirements_ok(nxcompat_phase phase) {
  nxcompat_requirement_report report;
  nxcompat_result_code result;
  memset(&report, 0, sizeof(report));
  result = nxcompat_requirements_evaluate(kotor_framework.registry,
                                          &kotor_framework.requirements,
                                          phase, &report);
  debugPrintf("NXCOMPAT requirements phase=%s satisfied=%zu pending=%zu "
              "missing=%zu rc=%d\n",
              nxcompat_phase_name(phase), report.satisfied_count,
              report.pending_count, report.missing_count, (int)result);
  return result == NXCOMPAT_OK ? 0 : -1;
}

static void kotor_runtime_report(nxcompat_phase phase) {
  nxcompat_runtime_report report;
  char *json;
  if (!kotor_framework.registry)
    return;
  memset(&report, 0, sizeof(report));
  if (nxcompat_registry_runtime_report(kotor_framework.registry,
                                       &kotor_framework.requirements, phase,
                                       &report) != NXCOMPAT_OK)
    return;
  json = (char *)malloc(KOTOR_RUNTIME_JSON_MAX);
  if (!json)
    return;
  if (nxcompat_format_runtime_json(&kotor_framework.host,
                                   &kotor_framework.plan, &report, json,
                                   KOTOR_RUNTIME_JSON_MAX) >= 0)
    debugPrintf("NXCOMPAT_REPORT %s\n", json);
  free(json);
}

static void kotor_maybe_ready(void) {
  if (!kotor_framework.ready_reported && kotor_framework.input_published &&
      kotor_framework.graphics_published && kotor_framework.audio_published &&
      kotor_requirements_ok(NXCOMPAT_PHASE_READY) == 0) {
    kotor_framework.ready_reported = 1;
    kotor_runtime_report(NXCOMPAT_PHASE_READY);
    debugPrintf("NEXTOS_FRAMEWORK ready loader=1 android=1 graphics=1 "
                "audio=1 input=1\n");
  }
}

static int kotor_android_invoke(void *userdata, const nxandroid_step *step) {
  kotor_framework_state *state = (kotor_framework_state *)userdata;
  if (!state || !step || !state->step_armed ||
      step->phase != state->expected_phase ||
      step->module_index != state->expected_module)
    return -1;
  state->step_armed = 0;
  debugPrintf("NXANDROID phase=%s contract=%s\n",
              nxandroid_phase_name(step->phase), step->contract_id);
  if (step->phase == NXANDROID_PHASE_RUNTIME_DELEGATED) {
    if (!state->delegated_entry)
      return -1;
    state->delegated_result = state->delegated_entry(
        state->delegated_argc, state->delegated_argv);
  }
  return 0;
}

static int kotor_android_rollback(void *userdata, const nxandroid_step *step) {
  (void)userdata;
  (void)step;
  return 0;
}

static int kotor_android_commit(nxandroid_phase phase, size_t module_index) {
  nxandroid_result result;
  if (!kotor_framework.android || kotor_framework.step_armed)
    return -1;
  kotor_framework.step_armed = 1;
  kotor_framework.expected_phase = phase;
  kotor_framework.expected_module = module_index;
  result = nxandroid_context_step(kotor_framework.android);
  if (result != NXANDROID_OK || kotor_framework.step_armed) {
    kotor_framework.step_armed = 0;
    debugPrintf("NXANDROID commit failed phase=%s rc=%d\n",
                nxandroid_phase_name(phase), (int)result);
    return -1;
  }
  return 0;
}

int kotor_framework_preflight(const char *game_dir) {
  static const char *const guests[] = {
      "libLzmaLib.so", "libminiz.so", "libfreetype.so", "libfmod.so",
      "libhidapi.so", "libandroid_port.so", "libKOTOR.so",
  };
  nxcompat_probe_options probe_options;
  nxcompat_plan_options plan_options;
  nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
  nxandroid_profile profile;
  nxandroid_ops ops;
  size_t index;
  if (!game_dir || game_dir[0] != '/' || kotor_framework.initialized)
    return -1;
  memset(&kotor_framework, 0, sizeof(kotor_framework));
  memset(&probe_options, 0, sizeof(probe_options));
  probe_options.api_version = NXCOMPAT_API_VERSION;
  probe_options.struct_size = sizeof(probe_options);
  probe_options.port_id = "kotor";
  probe_options.game_dir = game_dir;
  probe_options.portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");
  probe_options.result = &kotor_framework.probe;
  if (nxcompat_probe(&probe_options, &kotor_framework.host) != 0)
    goto fail;
  memset(&plan_options, 0, sizeof(plan_options));
  plan_options.api_version = NXCOMPAT_API_VERSION;
  plan_options.struct_size = sizeof(plan_options);
  plan_options.runtime_arch = NXCOMPAT_ARCH_ARMV7;
  plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE;
  if (nxcompat_plan_environment_v2(&kotor_framework.host, &plan_options,
                                   &kotor_framework.plan) != NXCOMPAT_OK ||
      nxcompat_apply_environment_v2(&kotor_framework.plan) != NXCOMPAT_OK)
    goto fail;
  if (nxcompat_registry_create(&kotor_framework.registry) != NXCOMPAT_OK ||
      nxcompat_registry_seed_host(kotor_framework.registry,
                                  &kotor_framework.host) != NXCOMPAT_OK ||
      nxcompat_requirements_parse_runtime_ex(&kotor_framework.requirements,
                                              &reason) != NXCOMPAT_OK)
    goto fail;
  if (kotor_requirements_ok(NXCOMPAT_PHASE_PREFLIGHT) != 0)
    goto fail;
  kotor_runtime_report(NXCOMPAT_PHASE_PREFLIGHT);

  for (index = 0u; index < sizeof(guests) / sizeof(guests[0]); ++index)
    if (kotor_validate_guest(guests[index]) != 0)
      goto fail;

  memset(&profile, 0, sizeof(profile));
  profile.api_version = NXANDROID_API_VERSION;
  profile.struct_size = sizeof(profile);
  profile.modules = kotor_modules;
  profile.module_count = sizeof(kotor_modules) / sizeof(kotor_modules[0]);
  profile.steps = kotor_steps;
  profile.step_count = sizeof(kotor_steps) / sizeof(kotor_steps[0]);
  profile.flags = NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME;
  if (nxandroid_profile_validate(&profile, NULL) != NXANDROID_OK)
    goto fail;
  memset(&ops, 0, sizeof(ops));
  ops.api_version = NXANDROID_API_VERSION;
  ops.struct_size = sizeof(ops);
  ops.invoke = kotor_android_invoke;
  ops.rollback = kotor_android_rollback;
  ops.userdata = &kotor_framework;
  if (nxandroid_context_create(&profile, &ops,
                               &kotor_framework.android) != NXANDROID_OK)
    goto fail;
  kotor_framework.initialized = 1;
  debugPrintf("NEXTOS_FRAMEWORK preflight=ok nxloader=%s nxcompat=%s "
              "nxandroid=%s nxgl=%s nxinput=%s nxaudio=%s\n",
              NXLOADER_VERSION_STRING, NXCOMPAT_VERSION,
              NXANDROID_VERSION, NXGL_VERSION, NXINPUT_VERSION,
              nxaudio_stack_name(NXAUDIO_STACK_OPENSL_ES));
  return 0;

fail:
  debugPrintf("NEXTOS_FRAMEWORK preflight=failed reason=%s\n",
              nxcompat_reason_name(reason));
  kotor_framework_finish();
  return -1;
}

int kotor_framework_android_module_initialized(size_t module_index) {
  return kotor_android_commit(NXANDROID_PHASE_MODULE_INITIALIZED,
                              module_index);
}

int kotor_framework_android_module_jni(size_t module_index) {
  return kotor_android_commit(NXANDROID_PHASE_MODULE_JNI, module_index);
}

int kotor_framework_android_activity_created(void) {
  return kotor_android_commit(NXANDROID_PHASE_ACTIVITY_CREATE,
                              KOTOR_NO_MODULE);
}

int kotor_framework_android_resumed(void) {
  return kotor_android_commit(NXANDROID_PHASE_RESUME, KOTOR_NO_MODULE);
}

int kotor_framework_run_delegated(int (*entry)(int, char **), int argc,
                                  char **argv, int *result) {
  if (!entry || !result)
    return -1;
  kotor_framework.delegated_entry = entry;
  kotor_framework.delegated_argc = argc;
  kotor_framework.delegated_argv = argv;
  if (kotor_android_commit(NXANDROID_PHASE_RUNTIME_DELEGATED,
                           KOTOR_NO_MODULE) != 0)
    return -1;
  *result = kotor_framework.delegated_result;
  return 0;
}

static int kotor_ensure_input(void) {
  nxinput_config config;
  nxcompat_input_receipt receipt;
  if (!kotor_framework.initialized)
    return -1;
  if (kotor_framework.input)
    return 0;
  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  kotor_framework.input = nxinput_create(&config);
  if (!kotor_framework.input) {
    debugPrintf("NXINPUT create failed: %s\n", SDL_GetError());
    return -1;
  }
  memset(&receipt, 0, sizeof(receipt));
  if (nxinput_nxcompat_publish_context(kotor_framework.registry,
                                       kotor_framework.input,
                                       &receipt) != NXCOMPAT_OK) {
    debugPrintf("NXINPUT receipt rejected\n");
    nxinput_destroy(kotor_framework.input);
    kotor_framework.input = NULL;
    return -1;
  }
  kotor_framework.input_published = 1;
  debugPrintf("NXINPUT active connected=%u generation=%llu mapping=%s\n",
              receipt.connected_count,
              (unsigned long long)receipt.topology_generation,
              nxcompat_input_mapping_source_name(receipt.mapping_source));
  kotor_requirements_ok(NXCOMPAT_PHASE_INPUT);
  kotor_maybe_ready();
  return 0;
}

void kotor_framework_observe_event(const SDL_Event *event) {
  if (!event || kotor_ensure_input() != 0)
    return;
  nxinput_observe_event(kotor_framework.input, event);
}

void kotor_framework_poll_input(void) {
  if (kotor_ensure_input() != 0)
    return;
  nxinput_poll(kotor_framework.input);
}

static int kotor_query_egl_config(EGLDisplay display, EGLContext context,
                                  nxcompat_graphics_receipt *receipt) {
  EGLint config_id = 0;
  EGLint count = 0;
  EGLConfig config = NULL;
  EGLint attributes[3];
  if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT || !receipt ||
      !eglQueryContext(display, context, EGL_CONFIG_ID, &config_id) ||
      config_id <= 0)
    return 0;
  attributes[0] = EGL_CONFIG_ID;
  attributes[1] = config_id;
  attributes[2] = EGL_NONE;
  if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1)
    return 0;
#define KOTOR_EGL_VALUE(attribute, field)                                    \
  do {                                                                        \
    EGLint value = 0;                                                         \
    if (!eglGetConfigAttrib(display, config, attribute, &value))             \
      return 0;                                                               \
    receipt->field = value;                                                   \
  } while (0)
  receipt->egl_config_id = config_id;
  KOTOR_EGL_VALUE(EGL_RED_SIZE, egl_red_bits);
  KOTOR_EGL_VALUE(EGL_GREEN_SIZE, egl_green_bits);
  KOTOR_EGL_VALUE(EGL_BLUE_SIZE, egl_blue_bits);
  KOTOR_EGL_VALUE(EGL_ALPHA_SIZE, egl_alpha_bits);
  KOTOR_EGL_VALUE(EGL_DEPTH_SIZE, egl_depth_bits);
  KOTOR_EGL_VALUE(EGL_STENCIL_SIZE, egl_stencil_bits);
  KOTOR_EGL_VALUE(EGL_RENDERABLE_TYPE, egl_renderable_type);
  KOTOR_EGL_VALUE(EGL_SURFACE_TYPE, egl_surface_type);
#undef KOTOR_EGL_VALUE
  return 1;
}

void kotor_framework_observe_swap(SDL_Window *window) {
  nxcompat_graphics_receipt receipt;
  nxgl_surface_metrics_input_v2 input;
  nxgl_surface_metrics_v2 metrics;
  const char *video;
  const char *gl_vendor;
  const char *gl_renderer;
  const char *gl_version;
  const char *glsl_version;
  const char *extensions;
  EGLDisplay egl_display;
  EGLContext egl_context;
  const char *egl_vendor;
  const char *egl_version;
  const char *egl_apis;
  GLint viewport[4] = {0, 0, 0, 0};
  if (!kotor_framework.initialized || kotor_framework.graphics_published ||
      !window || !SDL_GL_GetCurrentContext())
    return;
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
  receipt.generation = 1u;
  SDL_GetWindowSize(window, &receipt.window_width, &receipt.window_height);
  SDL_GL_GetDrawableSize(window, &receipt.drawable_width,
                         &receipt.drawable_height);
  video = SDL_GetCurrentVideoDriver();
  gl_vendor = (const char *)glGetString(GL_VENDOR);
  gl_renderer = (const char *)glGetString(GL_RENDERER);
  gl_version = (const char *)glGetString(GL_VERSION);
  glsl_version = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  extensions = (const char *)glGetString(GL_EXTENSIONS);
  if (!video || !gl_vendor || !gl_renderer || !gl_version || !glsl_version ||
      receipt.window_width <= 0 || receipt.window_height <= 0 ||
      receipt.drawable_width <= 0 || receipt.drawable_height <= 0)
    return;
  receipt.proof_flags = NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                        NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
                        NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL |
                        NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE;
  kotor_copy_text(receipt.video_backend, sizeof(receipt.video_backend), video);
  kotor_copy_text(receipt.gl_vendor, sizeof(receipt.gl_vendor), gl_vendor);
  kotor_copy_text(receipt.gl_renderer, sizeof(receipt.gl_renderer), gl_renderer);
  kotor_copy_text(receipt.gl_version, sizeof(receipt.gl_version), gl_version);
  kotor_copy_text(receipt.glsl_version, sizeof(receipt.glsl_version),
                  glsl_version);
  kotor_copy_text(receipt.gl_extensions, sizeof(receipt.gl_extensions),
                  extensions);
  if (sscanf(gl_version, "OpenGL ES %d.%d", &receipt.gles_major,
             &receipt.gles_minor) != 2 &&
      sscanf(gl_version, "OpenGL ES-%*s %d.%d", &receipt.gles_major,
             &receipt.gles_minor) != 2)
    return;
  (void)SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &receipt.red_bits);
  (void)SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &receipt.green_bits);
  (void)SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &receipt.blue_bits);
  (void)SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &receipt.alpha_bits);
  (void)SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &receipt.depth_bits);
  (void)SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &receipt.stencil_bits);
  (void)SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &receipt.double_buffer);
  (void)SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &receipt.profile_mask);

  egl_display = eglGetCurrentDisplay();
  egl_context = eglGetCurrentContext();
  egl_vendor = egl_display != EGL_NO_DISPLAY
                   ? eglQueryString(egl_display, EGL_VENDOR)
                   : NULL;
  egl_version = egl_display != EGL_NO_DISPLAY
                    ? eglQueryString(egl_display, EGL_VERSION)
                    : NULL;
  egl_apis = egl_display != EGL_NO_DISPLAY
                 ? eglQueryString(egl_display, EGL_CLIENT_APIS)
                 : NULL;
  if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT &&
      egl_vendor && egl_version && egl_apis) {
    receipt.proof_flags |= NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                           NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT;
    kotor_copy_text(receipt.egl_vendor, sizeof(receipt.egl_vendor), egl_vendor);
    kotor_copy_text(receipt.egl_version, sizeof(receipt.egl_version),
                    egl_version);
    kotor_copy_text(receipt.egl_client_apis, sizeof(receipt.egl_client_apis),
                    egl_apis);
    if (kotor_query_egl_config(egl_display, egl_context, &receipt))
      receipt.proof_flags |= NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED;
  }
  if (nxcompat_registry_publish_graphics(kotor_framework.registry, &receipt) !=
      NXCOMPAT_OK) {
    debugPrintf("NXGL engine-adapter receipt rejected\n");
    return;
  }
  kotor_framework.graphics_published = 1;
  memset(&input, 0, sizeof(input));
  input.api_version = NXGL_API_VERSION_V2;
  input.struct_size = sizeof(input);
  input.display_width = receipt.window_width;
  input.display_height = receipt.window_height;
  input.drawable_width = receipt.drawable_width;
  input.drawable_height = receipt.drawable_height;
  glGetIntegerv(GL_VIEWPORT, viewport);
  input.viewport_x = viewport[0];
  input.viewport_y = viewport[1];
  input.viewport_width = viewport[2];
  input.viewport_height = viewport[3];
  input.render_target_width = viewport[2];
  input.render_target_height = viewport[3];
  memset(&metrics, 0, sizeof(metrics));
  if (nxgl_calculate_surface_metrics_v2(&input, &metrics) == NXGL_SUCCESS)
    debugPrintf("NXGL engine-owned drawable=%dx%d viewport=%dx%d "
                "scale=%.3fx%.3f\n",
                metrics.drawable_width, metrics.drawable_height,
                metrics.viewport_width, metrics.viewport_height,
                metrics.drawable_per_display_scale_x,
                metrics.drawable_per_display_scale_y);
  kotor_requirements_ok(NXCOMPAT_PHASE_GRAPHICS);
  kotor_maybe_ready();
}

void kotor_framework_observe_audio(SDL_AudioDeviceID device,
                                   const SDL_AudioSpec *obtained) {
  nxcompat_audio_receipt receipt;
  nxaudio_backend_observation observation;
  nxaudio_reason reason = NXAUDIO_REASON_NONE;
  const char *backend;
  if (!kotor_framework.initialized || kotor_framework.audio_published ||
      device == 0 || !obtained)
    return;
  backend = SDL_GetCurrentAudioDriver();
  if (!backend || !backend[0])
    return;
  memset(&observation, 0, sizeof(observation));
  observation.api_version = NXAUDIO_API_VERSION;
  observation.struct_size = sizeof(observation);
  kotor_copy_text(observation.backend, sizeof(observation.backend), backend);
  observation.inherited_attempt = getenv("SDL_AUDIODRIVER") != NULL;
  observation.server_reachable = 1;
  observation.device_opened = 1;
  if (nxaudio_classify_backend(&observation, &reason) != NXAUDIO_OK) {
    debugPrintf("NXAUDIO backend=%s rejected reason=%s\n", backend,
                nxaudio_reason_name(reason));
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
  receipt.frequency = obtained->freq;
  receipt.format = obtained->format;
  receipt.channels = obtained->channels;
  receipt.samples = obtained->samples;
  receipt.device_id_was_nonzero = 1;
  kotor_copy_text(receipt.backend, sizeof(receipt.backend), backend);
  if (nxcompat_registry_publish_audio(kotor_framework.registry, &receipt) !=
      NXCOMPAT_OK) {
    debugPrintf("NXAUDIO receipt rejected\n");
    return;
  }
  kotor_framework.audio_published = 1;
  debugPrintf("NXAUDIO active backend=%s format=%dHz/%uch reason=%s\n",
              backend, obtained->freq, (unsigned)obtained->channels,
              nxaudio_reason_name(reason));
  kotor_requirements_ok(NXCOMPAT_PHASE_AUDIO);
  kotor_maybe_ready();
}

void kotor_framework_finish(void) {
  if (kotor_framework.input) {
    nxinput_destroy(kotor_framework.input);
    kotor_framework.input = NULL;
  }
  if (kotor_framework.android)
    (void)nxandroid_context_destroy(&kotor_framework.android);
  nxcompat_registry_destroy(kotor_framework.registry);
  memset(&kotor_framework, 0, sizeof(kotor_framework));
}
