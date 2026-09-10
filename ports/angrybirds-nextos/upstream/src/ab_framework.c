/* ab_framework.c — capability-driven universal runtime boundary.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ab_port.h"
#include "nxcompat.h"
#include "nxcompat_sdl2.h"
#include "nxgl.h"
#include "nxgl_gles2.h"
#include "ab_egl.h"
#include "nxgl_nxcompat.h"
#include "nxinput.h"
#include "nxinput_nxcompat.h"

static nxcompat_host g_host;
static nxcompat_plan_v2 g_plan;
static nxcompat_registry *g_registry;
static nxcompat_requirements g_requirements;
static nxgl_context *g_graphics;
static nxgl_report_v2 g_graphics_report;
static uint64_t g_generation = 1u;
/* GLES2 do convidado: os 69 imports gl* sao resolvidos por SDL_GL_GetProcAddress
 * numa tabela propria (ab_gl.c) e o convidado nao importa EGL. A antiga
 * "promocao" do provedor para RTLD_GLOBAL (dladdr + dlopen RTLD_NOLOAD +
 * dlsym pelo nome) nao tem como passar sob GLVND (ROCKNIX/Mesa): o endereco
 * devolvido por eglGetProcAddress mora em libGLdispatch, que nao exporta os
 * nomes gl*. O portao certo, herdado da V4 (nxgl 0.3.5), e' a prova de vida do
 * conjunto ES2 no contexto corrente: nxgl_gles2 resolve os 142 pontos de
 * entrada pelo resolver do SDL e confirma glGetString(GL_RENDERER) do proprio
 * provedor. Nenhum handle e' promovido ao namespace global. */
static nxgl_gles2_receipt g_gles2_receipt;

static int provider_exports_egl(void *handle) {
  static const char *const required[] = {
      "eglGetDisplay",          "eglInitialize",
      "eglTerminate",           "eglChooseConfig",
      "eglGetConfigAttrib",     "eglCreateWindowSurface",
      "eglCreatePbufferSurface", "eglDestroySurface",
      "eglBindAPI",             "eglCreateContext",
      "eglDestroyContext",      "eglMakeCurrent",
      "eglSwapBuffers",         "eglSwapInterval",
      "eglGetError",            "eglGetProcAddress",
      "eglQueryString",         "eglQueryContext",
      "eglQuerySurface",        "eglGetCurrentDisplay",
      "eglGetCurrentContext",   "eglGetCurrentSurface"};
  size_t index;
  if (!handle)
    return 0;
  for (index = 0u; index < sizeof(required) / sizeof(required[0]); ++index) {
    if (!dlsym(handle, required[index]))
      return 0;
  }
  return 1;
}

static void compat_status(void *userdata, nxcompat_status_kind kind,
                          const char *message) {
  (void)userdata;
  ab_log("[nxcompat:%d] %s", (int)kind, message ? message : "-");
}

static void gl_status(void *userdata, nxgl_status_kind kind,
                      const char *message) {
  (void)userdata;
  ab_log("[nxgl:%d] %s", (int)kind, message ? message : "-");
}

static void log_graphics_failure(int result,
                                 const nxgl_report_v2 *report) {
  unsigned int journal_index;
  ab_log("[nxgl] open falhou rc=%d stage=%d reason=%d", result,
         (int)report->final_stage, (int)report->final_reason);
  ab_log("[nxgl] SDL: %s; tentativas=%u journal=%u descartadas=%u",
         SDL_GetError(), report->legacy.attempt_count, report->journal_count,
         report->journal_dropped);
  for (journal_index = 0u; journal_index < report->journal_count;
       journal_index++) {
    const nxgl_attempt_entry_v2 *entry = &report->journal[journal_index];
    ab_log("[nxgl] tentativa=%u round=%u candidato=%zu stage=%d reason=%d rc=%d",
           entry->attempt_index, entry->round_index, entry->candidate_index,
           (int)entry->stage, (int)entry->reason, entry->result);
  }
}

static int bind_precontext_provider(const nxgl_report_v2 *report,
                                    size_t candidate_count) {
  static const char provider_name[] = "libMali.so";
  nxgl_sdl_precontext_recovery_v2 recovery;
  void *candidate;
  int exports_egl;
  int exports_gles;

  nxgl_sdl_precontext_recovery_v2_init(&recovery);
  recovery.video_backend = report->legacy.video_backend;
  recovery.provider_name = provider_name;
  recovery.failed_stage = report->final_stage;
  recovery.final_reason = report->final_reason;
  recovery.attempts_exhausted =
      report->legacy.attempt_count >= candidate_count;
  /* Presence, including an empty value, is caller/firmware ownership. */
  recovery.inherited_provider_hint =
      getenv("SDL_VIDEO_EGL_DRIVER") != NULL ||
      getenv("SDL_VIDEO_GL_DRIVER") != NULL;
  recovery.same_object = 1;

  candidate = dlopen(provider_name, RTLD_LAZY | RTLD_LOCAL);
  exports_egl = provider_exports_egl(candidate);
  exports_gles = ab_gl_provider_exports(candidate);
  recovery.exports_egl = exports_egl;
  recovery.exports_engine_gles = exports_gles;
  if (candidate)
    dlclose(candidate);

  if (nxgl_plan_sdl_precontext_recovery_v2(&recovery) !=
      NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT)
    return 0;
  if (setenv("SDL_VIDEO_EGL_DRIVER", provider_name, 1) != 0 ||
      setenv("SDL_VIDEO_GL_DRIVER", provider_name, 1) != 0) {
    unsetenv("SDL_VIDEO_EGL_DRIVER");
    unsetenv("SDL_VIDEO_GL_DRIVER");
    return 0;
  }
  ab_log("[nxgl] retry coerente autorizado: backend=%s EGL+GLES=mesmo-provider",
         report->legacy.video_backend);
  return 1;
}

static void unbind_precontext_provider(int active) {
  if (!active)
    return;
  unsetenv("SDL_VIDEO_EGL_DRIVER");
  unsetenv("SDL_VIDEO_GL_DRIVER");
}

static int evaluate(nxcompat_phase phase) {
  nxcompat_requirement_report report;
  nxcompat_result_code result = nxcompat_requirements_evaluate(
      g_registry, &g_requirements, phase, &report);
  ab_log("[nxcompat] fase=%s ok=%zu pendente=%zu ausente=%zu reason=%s",
         nxcompat_phase_name(phase), report.satisfied_count,
         report.pending_count, report.missing_count,
         nxcompat_reason_name(report.final_reason));
  return result == NXCOMPAT_OK;
}

int ab_framework_preflight(const char *game_dir) {
  nxcompat_probe_options probe;
  nxcompat_probe_result observations;
  nxcompat_plan_options plan_options;
  nxcompat_sdl2_options audio_options;
  nxcompat_backend_result_v2 audio_result;
  nxcompat_audio_receipt audio_receipt;
  nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
  const char *portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");

  memset(&probe, 0, sizeof(probe));
  memset(&observations, 0, sizeof(observations));
  probe.api_version = NXCOMPAT_API_VERSION_V2;
  probe.struct_size = sizeof(probe);
  probe.port_id = "angrybirds";
  probe.game_dir = game_dir;
  probe.portmaster_dir = portmaster_dir && *portmaster_dir ? portmaster_dir : NULL;
  probe.result = &observations;
  if (nxcompat_probe(&probe, &g_host) != NXCOMPAT_OK) {
    ab_log("[nxcompat] probe falhou: %s",
           nxcompat_reason_name(observations.final_reason));
    return 0;
  }

  memset(&plan_options, 0, sizeof(plan_options));
  plan_options.api_version = NXCOMPAT_API_VERSION_V2;
  plan_options.struct_size = sizeof(plan_options);
  plan_options.runtime_arch = NXCOMPAT_ARCH_ARMV7;
  plan_options.policy_flags =
      NXCOMPAT_POLICY_AUTOMATIC_SAFE | NXCOMPAT_POLICY_LOW_MEMORY_ARENAS;
  plan_options.low_memory_arena_max = 2u;
  if (nxcompat_plan_environment_v2(&g_host, &plan_options, &g_plan) !=
          NXCOMPAT_OK ||
      nxcompat_apply_environment_v2(&g_plan) != NXCOMPAT_OK) {
    ab_log("[nxcompat] plano de ambiente falhou: %s",
           nxcompat_reason_name(g_plan.final_reason));
    return 0;
  }

  if (nxcompat_registry_create(&g_registry) != NXCOMPAT_OK ||
      nxcompat_registry_seed_host(g_registry, &g_host) != NXCOMPAT_OK) {
    ab_log("[nxcompat] registry nao iniciou");
    return 0;
  }
  if (nxcompat_requirements_parse_runtime_ex(&g_requirements, &reason) !=
      NXCOMPAT_OK) {
    ab_log("[nxcompat] contrato invalido: %s", nxcompat_reason_name(reason));
    return 0;
  }
  if (!evaluate(NXCOMPAT_PHASE_PREFLIGHT))
    return 0;

  memset(&audio_options, 0, sizeof(audio_options));
  audio_options.api_version = NXCOMPAT_API_VERSION_V2;
  audio_options.struct_size = sizeof(audio_options);
  audio_options.kind = NXCOMPAT_BACKEND_AUDIO;
  audio_options.status = compat_status;
  if (nxcompat_sdl2_negotiate_audio_v2(&audio_options, g_generation++,
                                        &audio_result, &audio_receipt) !=
          NXCOMPAT_OK ||
      nxcompat_registry_publish_audio(g_registry, &audio_receipt) !=
          NXCOMPAT_OK) {
    ab_log("[nxcompat] audio preflight falhou: %s",
           nxcompat_reason_name(audio_result.final_reason));
    return 0;
  }
  ab_log("[nxcompat] %s %s; audio=%s", NXCOMPAT_VERSION,
         nxcompat_arch_name(g_host.process_arch), audio_receipt.backend);
  return 1;
}

int ab_framework_open_graphics(SDL_Window **window, SDL_GLContext *context,
                               int *width, int *height) {
  nxgl_open_options_v2 options;
  nxgl_engine_requirements requirements;
  nxgl_resolution_sources fallback;
  nxgl_config_candidate candidates[] = {
      {2, 0, 5, 6, 5, 0, 16, 0, 1}, {2, 0, 8, 8, 8, 0, 16, 0, 1},
      {2, 0, 8, 8, 8, 8, 16, 0, 1}, {2, 0, 5, 6, 5, 0, 16, 0, 0},
      {2, 0, 8, 8, 8, 0, 16, 0, 0}, {2, 0, 8, 8, 8, 8, 16, 0, 0}};
  nxcompat_graphics_receipt receipt;
  int result;
  int provider_recovery = 0;

  nxgl_open_options_v2_init(&options);
  nxgl_engine_requirements_init(&requirements);
  nxgl_resolution_sources_init(&fallback);
  nxgl_nxcompat_resolution_sources(&g_host, &fallback);
  requirements.minimum_gles_major = 2;
  requirements.minimum_gles_minor = 0;
  /* A GLES3-capable driver may legally return a newer ES context for an ES2
   * request. The guest imports GLES2 only, so no artificial maximum applies. */
  requirements.maximum_gles_major = 0;
  requirements.maximum_gles_minor = 0;
  requirements.minimum_red_bits = 5;
  requirements.minimum_green_bits = 6;
  requirements.minimum_blue_bits = 5;
  requirements.minimum_depth_bits = 16;
  requirements.require_double_buffer = 0;

  options.flags = NXGL_OPEN_INITIALIZE_VIDEO |
                  NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
                  NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE;
  options.window_title = "Angry Birds Classic";
  options.window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP;
  options.display_index = 0;
  options.drawable_wait_ms = 1500u;
  options.fallback_facts = &fallback;
  options.requirements = &requirements;
  options.candidates = candidates;
  options.candidate_count = sizeof(candidates) / sizeof(candidates[0]);
  options.status = gl_status;
  result = nxgl_open_v2(&options, &g_graphics, &g_graphics_report);
  if (result != NXGL_SUCCESS) {
    log_graphics_failure(result, &g_graphics_report);
    provider_recovery = bind_precontext_provider(
        &g_graphics_report,
        sizeof(candidates) / sizeof(candidates[0]));
    if (!provider_recovery)
      return 0;
    result = nxgl_open_v2(&options, &g_graphics, &g_graphics_report);
    if (result != NXGL_SUCCESS) {
      log_graphics_failure(result, &g_graphics_report);
      unbind_precontext_provider(provider_recovery);
      return 0;
    }
  }
  ab_log("[nxgl] selecionado backend=%s GLES=%d.%d rgba=%d/%d/%d/%d "
         "depth=%d stencil=%d double=%d drawable=%dx%d egl_obs=%d "
         "egl_cfg=%d egl_rgba=%d/%d/%d/%d egl_depth=%d egl_stencil=%d "
         "egl_renderable=0x%x egl_surface=0x%x",
         g_graphics_report.legacy.video_backend,
         g_graphics_report.legacy.actual.gles_major,
         g_graphics_report.legacy.actual.gles_minor,
         g_graphics_report.legacy.actual.red_bits,
         g_graphics_report.legacy.actual.green_bits,
         g_graphics_report.legacy.actual.blue_bits,
         g_graphics_report.legacy.actual.alpha_bits,
         g_graphics_report.legacy.actual.depth_bits,
         g_graphics_report.legacy.actual.stencil_bits,
         g_graphics_report.legacy.actual.double_buffer,
         g_graphics_report.legacy.drawable_width,
         g_graphics_report.legacy.drawable_height,
         g_graphics_report.egl.observed, g_graphics_report.egl.config_id,
         g_graphics_report.egl.red_bits, g_graphics_report.egl.green_bits,
         g_graphics_report.egl.blue_bits, g_graphics_report.egl.alpha_bits,
         g_graphics_report.egl.depth_bits,
         g_graphics_report.egl.stencil_bits,
         g_graphics_report.egl.renderable_type,
         g_graphics_report.egl.surface_type);
  ab_log("[nxgl] GL vendor=%s renderer=%s version=%s glsl=%s; EGL vendor=%s "
         "version=%s apis=%s",
         g_graphics_report.legacy.vendor,
         g_graphics_report.legacy.renderer,
         g_graphics_report.legacy.version,
         g_graphics_report.legacy.shading_language_version,
         g_graphics_report.egl.vendor, g_graphics_report.egl.version,
         g_graphics_report.egl.client_apis);
  if (nxgl_nxcompat_publish_context(g_registry, g_graphics, g_generation++,
                                     &receipt) != NXCOMPAT_OK) {
    ab_log("[nxgl] recibo forte rejeitado");
    unbind_precontext_provider(provider_recovery);
    return 0;
  }
  /* Prova de vida do ES2 (V4): resolver = SDL, contexto corrente = o que o nxgl
   * acabou de abrir. Falha fechada se algum ponto de entrada faltar ou se o
   * provedor nao responder glGetString. */
  nxgl_gles2_set_primary_resolver((nxgl_gles2_resolver_fn)SDL_GL_GetProcAddress);
  if (nxgl_gles2_init(&g_gles2_receipt) != 0) {
    ab_log("[nxgl] gles2: conjunto ES2 incompleto ou provedor sem vida: %s "
           "(resolvidos=%u/%u faltando=%s)",
           g_gles2_receipt.text, g_gles2_receipt.resolved,
           g_gles2_receipt.total, g_gles2_receipt.first_missing);
    unbind_precontext_provider(provider_recovery);
    return 0;
  }
  ab_log("[nxgl] gles2: provedor=%s vida=%s resolvidos=%u/%u",
         nxgl_gles2_provider(), nxgl_gles2_liveness(),
         g_gles2_receipt.resolved, g_gles2_receipt.total);
  /* EGL do proprio loader (contextos compartilhados do EGLWrapper): ligado ao
   * provedor que possui o contexto corrente pelo nxgl_egl_binding (V4). */
  if (!ab_egl_bind()) {
    ab_log("[nxgl] egl-binding recusado: %s", ab_egl_receipt());
    unbind_precontext_provider(provider_recovery);
    return 0;
  }
  ab_log("[nxgl] egl-binding: %s", ab_egl_receipt());
  unbind_precontext_provider(provider_recovery);
  *window = g_graphics_report.handles.sdl_window;
  *context = g_graphics_report.handles.sdl_context;
  *width = g_graphics_report.legacy.drawable_width;
  *height = g_graphics_report.legacy.drawable_height;
  ab_log("[nxgl] %s %dx%d rgb=%d/%d/%d a=%d d=%d backend=%s provider=coerente",
         NXGL_VERSION, *width, *height, receipt.red_bits, receipt.green_bits,
         receipt.blue_bits, receipt.alpha_bits, receipt.depth_bits,
         receipt.video_backend);
  return evaluate(NXCOMPAT_PHASE_GRAPHICS);
}

int ab_framework_publish_input(nxinput_context *input) {
  nxcompat_input_receipt receipt;
  if (!input || nxinput_nxcompat_publish_context(g_registry, input, &receipt) !=
                    NXCOMPAT_OK) {
    ab_log("[nxinput] recibo forte rejeitado");
    return 0;
  }
  ab_log("[nxinput] %s conectados=%u geracao=%llu", NXINPUT_VERSION,
         receipt.connected_count,
         (unsigned long long)receipt.topology_generation);
  return 1;
}

int ab_framework_require_ready(void) {
  nxcompat_runtime_report report;
  char json[32768];
  if (!evaluate(NXCOMPAT_PHASE_READY))
    return 0;
  if (nxcompat_registry_runtime_report(g_registry, &g_requirements,
                                       NXCOMPAT_PHASE_READY, &report) !=
      NXCOMPAT_OK)
    return 0;
  if (nxcompat_format_runtime_json(&g_host, &g_plan, &report, json,
                                   sizeof(json)) > 0)
    ab_log("[nxcompat-runtime] %s", json);
  return 1;
}

int ab_framework_present(void) {
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  nxgl_present_policy_v2_init(&policy);
  nxgl_present_result_v2_init(&result);
  policy.owner = NXGL_PRESENT_SDL;
  policy.quirk = NXGL_PRESENT_QUIRK_V2_NONE;
  policy.reason = NXGL_PRESENT_REASON_V2_NONE;
  return nxgl_present_v2(g_graphics, &policy, &result) == NXGL_SUCCESS;
}
