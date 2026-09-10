/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "framework_bridge.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxaudio.h"
#include "nxcompat.h"
#include "nxinput.h"
#include "nxinput_nxcompat.h"

typedef struct dt_framework_state {
    nxcompat_host host;
    nxcompat_probe_result probe;
    nxcompat_plan_v2 plan;
    nxcompat_registry *registry;
    nxcompat_requirements requirements;
    nxinput_context *input;
    uint64_t graphics_generation;
    uint64_t audio_generation;
    int initialized;
    int graphics_published;
    int ready_published;
} dt_framework_state;

static dt_framework_state g_framework;
static pthread_mutex_t g_framework_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_bounded(char *output, size_t output_size, const char *value)
{
    if (!output || output_size == 0u)
        return;
    snprintf(output, output_size, "%s", value ? value : "");
}

static int evaluate_locked(nxcompat_phase phase, int require_complete)
{
    nxcompat_requirement_report report;
    nxcompat_result_code result;
    size_t index;

    if (!g_framework.initialized || !g_framework.registry)
        return -1;
    memset(&report, 0, sizeof(report));
    report.api_version = NXCOMPAT_API_VERSION;
    report.struct_size = sizeof(report);
    result = nxcompat_requirements_evaluate(g_framework.registry,
                                             &g_framework.requirements,
                                             phase, &report);
    fprintf(stderr,
            "[deadtrigger/framework] phase=%s satisfied=%zu pending=%zu "
            "missing=%zu reason=%s\n",
            nxcompat_phase_name(phase), report.satisfied_count,
            report.pending_count, report.missing_count,
            nxcompat_reason_name(report.final_reason));
    for (index = 0u; index < report.count; ++index) {
        const nxcompat_requirement_result *requirement =
            &report.results[index];
        const nxcompat_capability_definition *definition;

        if (requirement->state != NXCOMPAT_REQUIREMENT_MISSING)
            continue;
        definition = nxcompat_capability_definition_by_id(
            requirement->capability_id);
        fprintf(stderr, "[deadtrigger/framework] missing capability=%s\n",
                definition ? definition->name : "unknown");
    }
    if (result != NXCOMPAT_OK || report.missing_count != 0u ||
        (require_complete && report.pending_count != 0u))
        return -1;
    if (phase == NXCOMPAT_PHASE_READY && report.pending_count == 0u &&
        !g_framework.ready_published) {
        g_framework.ready_published = 1;
        fprintf(stderr, "[deadtrigger/framework] READY %zu/%zu\n",
                report.satisfied_count, report.count);
    }
    return 0;
}

static void evaluate_ready_locked(void)
{
    if (!g_framework.input || !g_framework.graphics_published ||
        g_framework.audio_generation == 0u)
        return;
    (void)evaluate_locked(NXCOMPAT_PHASE_READY, 1);
}

int dt_framework_preflight(const char *game_dir)
{
    nxcompat_probe_options probe_options;
    nxcompat_plan_options plan_options;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    const char *port_id;
    int result = -1;

    if (!game_dir || game_dir[0] != '/')
        return -1;
    pthread_mutex_lock(&g_framework_lock);
    if (g_framework.initialized)
        goto done;
    port_id = getenv("NXCOMPAT_PORT_ID");
    if (!port_id || !port_id[0])
        port_id = "deadtrigger";

    memset(&probe_options, 0, sizeof(probe_options));
    probe_options.api_version = NXCOMPAT_API_VERSION;
    probe_options.struct_size = sizeof(probe_options);
    probe_options.port_id = port_id;
    probe_options.game_dir = game_dir;
    probe_options.portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");
    probe_options.result = &g_framework.probe;
    if (nxcompat_probe(&probe_options, &g_framework.host) != 0) {
        fprintf(stderr, "[deadtrigger/framework] nxcompat probe failed\n");
        goto done;
    }

    memset(&plan_options, 0, sizeof(plan_options));
    plan_options.api_version = NXCOMPAT_API_VERSION;
    plan_options.struct_size = sizeof(plan_options);
    plan_options.runtime_arch = NXCOMPAT_ARCH_AARCH64;
    plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE |
                                NXCOMPAT_POLICY_LOW_MEMORY_ARENAS;
    plan_options.low_memory_arena_max = 2u;
    if (nxcompat_plan_environment_v2(&g_framework.host, &plan_options,
                                     &g_framework.plan) != NXCOMPAT_OK ||
        nxcompat_apply_environment_v2(&g_framework.plan) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[deadtrigger/framework] environment plan/apply failed: %s\n",
                nxcompat_reason_name(g_framework.plan.final_reason));
        goto done;
    }
    if (nxcompat_registry_create(&g_framework.registry) != NXCOMPAT_OK ||
        nxcompat_registry_seed_host(g_framework.registry,
                                    &g_framework.host) != NXCOMPAT_OK ||
        nxcompat_requirements_parse_runtime_ex(&g_framework.requirements,
                                                &reason) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[deadtrigger/framework] registry/requirements rejected: %s\n",
                nxcompat_reason_name(reason));
        nxcompat_registry_destroy(g_framework.registry);
        g_framework.registry = NULL;
        goto done;
    }
    g_framework.initialized = 1;
    result = evaluate_locked(NXCOMPAT_PHASE_PREFLIGHT, 0);
done:
    pthread_mutex_unlock(&g_framework_lock);
    return result;
}

int dt_framework_open_input(void)
{
    nxinput_config config;
    nxcompat_input_receipt receipt;
    int result = -1;

    pthread_mutex_lock(&g_framework_lock);
    if (!g_framework.initialized || !g_framework.registry ||
        g_framework.input)
        goto done;
    nxinput_config_init(&config);
    config.initialize_sdl = 0;
    g_framework.input = nxinput_create(&config);
    if (!g_framework.input) {
        fprintf(stderr, "[deadtrigger/framework] nxinput create failed: %s\n",
                SDL_GetError());
        goto done;
    }
    memset(&receipt, 0, sizeof(receipt));
    if (nxinput_nxcompat_publish_context(g_framework.registry,
                                         g_framework.input,
                                         &receipt) != NXCOMPAT_OK)
        goto done;
    fprintf(stderr,
            "[deadtrigger/framework] input controller-api active pads=%u\n",
            nxinput_connected_count(g_framework.input));
    (void)evaluate_locked(NXCOMPAT_PHASE_PREFLIGHT, 0);
    result = 0;
    evaluate_ready_locked();
done:
    pthread_mutex_unlock(&g_framework_lock);
    return result;
}

void dt_framework_observe_input(const SDL_Event *event)
{
    if (!event)
        return;
    pthread_mutex_lock(&g_framework_lock);
    if (g_framework.input) {
        nxinput_observe_event(g_framework.input, event);
        if (event->type == SDL_CONTROLLERDEVICEADDED ||
            event->type == SDL_CONTROLLERDEVICEREMOVED ||
            event->type == SDL_CONTROLLERDEVICEREMAPPED) {
            nxcompat_input_receipt receipt;
            memset(&receipt, 0, sizeof(receipt));
            (void)nxinput_nxcompat_publish_context(g_framework.registry,
                                                   g_framework.input,
                                                   &receipt);
        }
    }
    pthread_mutex_unlock(&g_framework_lock);
}

void dt_framework_poll_input(void)
{
    pthread_mutex_lock(&g_framework_lock);
    if (g_framework.input)
        nxinput_poll(g_framework.input);
    pthread_mutex_unlock(&g_framework_lock);
}

void dt_framework_audio_opened(SDL_AudioDeviceID device,
                               const SDL_AudioSpec *actual)
{
    nxaudio_backend_observation observation;
    nxaudio_reason reason = NXAUDIO_REASON_NONE;
    nxcompat_audio_receipt receipt;
    const char *backend = SDL_GetCurrentAudioDriver();

    if (device == 0 || !actual || !backend)
        return;
    pthread_mutex_lock(&g_framework_lock);
    if (!g_framework.initialized || !g_framework.registry)
        goto done;
    memset(&observation, 0, sizeof(observation));
    observation.api_version = NXAUDIO_API_VERSION;
    observation.struct_size = sizeof(observation);
    copy_bounded(observation.backend, sizeof(observation.backend), backend);
    observation.inherited_attempt =
        g_framework.host.inherited_audio_driver[0] != '\0';
    observation.server_reachable = 1;
    observation.device_opened = 1;
    if (nxaudio_classify_backend(&observation, &reason) != NXAUDIO_OK) {
        fprintf(stderr, "[deadtrigger/framework] audio rejected: %s\n",
                nxaudio_reason_name(reason));
        goto done;
    }
    memset(&receipt, 0, sizeof(receipt));
    receipt.api_version = NXCOMPAT_API_VERSION;
    receipt.struct_size = sizeof(receipt);
    receipt.proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                          NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                          NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
    receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
    receipt.generation = ++g_framework.audio_generation;
    receipt.lifetime = NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED;
    receipt.frequency = actual->freq;
    receipt.format = actual->format;
    receipt.channels = actual->channels;
    receipt.samples = actual->samples;
    receipt.device_id_was_nonzero = 1;
    copy_bounded(receipt.backend, sizeof(receipt.backend), backend);
    if (nxcompat_registry_publish_audio(g_framework.registry, &receipt) !=
        NXCOMPAT_OK)
        goto done;
    (void)evaluate_locked(NXCOMPAT_PHASE_AUDIO, 0);
    evaluate_ready_locked();
done:
    pthread_mutex_unlock(&g_framework_lock);
}

void dt_framework_audio_failed(void)
{
    fprintf(stderr, "[deadtrigger/framework] audio device open failed\n");
}

static void parse_gles_version(const char *version, int *major, int *minor)
{
    const char *cursor;
    int parsed_major = 0;
    int parsed_minor = 0;

    *major = 0;
    *minor = 0;
    if (!version)
        return;
    cursor = strstr(version, "OpenGL ES");
    if (!cursor)
        return;
    cursor += strlen("OpenGL ES");
    while (*cursor == ' ' || (*cursor >= 'A' && *cursor <= 'Z') ||
           (*cursor >= 'a' && *cursor <= 'z'))
        ++cursor;
    if (sscanf(cursor, "%d.%d", &parsed_major, &parsed_minor) == 2 &&
        parsed_major >= 1 && parsed_major <= 9 &&
        parsed_minor >= 0 && parsed_minor <= 9) {
        *major = parsed_major;
        *minor = parsed_minor;
    }
}

static EGLConfig current_config(EGLDisplay display, EGLContext context)
{
    EGLint wanted = 0;
    EGLint count = 0;
    EGLConfig configs[128];

    if (!eglQueryContext(display, context, EGL_CONFIG_ID, &wanted) ||
        wanted <= 0 || !eglGetConfigs(display, configs,
                                      (EGLint)(sizeof(configs) /
                                               sizeof(configs[0])),
                                      &count))
        return NULL;
    for (EGLint index = 0; index < count; ++index) {
        EGLint candidate = 0;
        if (eglGetConfigAttrib(display, configs[index], EGL_CONFIG_ID,
                               &candidate) && candidate == wanted)
            return configs[index];
    }
    return NULL;
}

int dt_framework_publish_graphics(const dt_graphics_evidence *evidence)
{
    nxcompat_graphics_receipt receipt;
    int result = -1;

    pthread_mutex_lock(&g_framework_lock);
    if (g_framework.graphics_published) {
        result = 0;
        goto done;
    }
    if (!g_framework.initialized || !g_framework.registry || !evidence ||
        evidence->window_width <= 0 || evidence->window_height <= 0 ||
        evidence->drawable_width <= 0 || evidence->drawable_height <= 0 ||
        !evidence->backend || !evidence->backend[0] ||
        !evidence->gl_vendor || !evidence->gl_vendor[0] ||
        !evidence->gl_renderer || !evidence->gl_renderer[0] ||
        !evidence->gl_version || !evidence->gl_version[0] ||
        !evidence->glsl_version || !evidence->glsl_version[0] ||
        !evidence->egl_vendor || !evidence->egl_vendor[0] ||
        !evidence->egl_version || !evidence->egl_version[0] ||
        !evidence->egl_client_apis || !evidence->egl_client_apis[0])
        goto done;
    memset(&receipt, 0, sizeof(receipt));
    receipt.api_version = NXCOMPAT_API_VERSION;
    receipt.struct_size = sizeof(receipt);
    receipt.proof_flags = NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                          NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
                          NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL |
                          NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                          NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |
                          NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED |
                          NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE;
    receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
    receipt.generation = ++g_framework.graphics_generation;
    receipt.window_width = evidence->window_width;
    receipt.window_height = evidence->window_height;
    receipt.drawable_width = evidence->drawable_width;
    receipt.drawable_height = evidence->drawable_height;
    parse_gles_version(evidence->gl_version, &receipt.gles_major,
                       &receipt.gles_minor);
    receipt.red_bits = evidence->red_bits;
    receipt.green_bits = evidence->green_bits;
    receipt.blue_bits = evidence->blue_bits;
    receipt.alpha_bits = evidence->alpha_bits;
    receipt.depth_bits = evidence->depth_bits;
    receipt.stencil_bits = evidence->stencil_bits;
    receipt.double_buffer = evidence->double_buffer;
    receipt.profile_mask = evidence->profile_mask;
    receipt.egl_config_id = evidence->egl_config_id;
    receipt.egl_red_bits = evidence->red_bits;
    receipt.egl_green_bits = evidence->green_bits;
    receipt.egl_blue_bits = evidence->blue_bits;
    receipt.egl_alpha_bits = evidence->alpha_bits;
    receipt.egl_depth_bits = evidence->depth_bits;
    receipt.egl_stencil_bits = evidence->stencil_bits;
    receipt.egl_renderable_type = evidence->egl_renderable_type;
    receipt.egl_surface_type = evidence->egl_surface_type;
    copy_bounded(receipt.video_backend, sizeof(receipt.video_backend),
                 evidence->backend);
    copy_bounded(receipt.gl_vendor, sizeof(receipt.gl_vendor),
                 evidence->gl_vendor);
    copy_bounded(receipt.gl_renderer, sizeof(receipt.gl_renderer),
                 evidence->gl_renderer);
    copy_bounded(receipt.gl_version, sizeof(receipt.gl_version),
                 evidence->gl_version);
    copy_bounded(receipt.glsl_version, sizeof(receipt.glsl_version),
                 evidence->glsl_version);
    copy_bounded(receipt.gl_extensions, sizeof(receipt.gl_extensions),
                 evidence->gl_extensions);
    copy_bounded(receipt.egl_vendor, sizeof(receipt.egl_vendor),
                 evidence->egl_vendor);
    copy_bounded(receipt.egl_version, sizeof(receipt.egl_version),
                 evidence->egl_version);
    copy_bounded(receipt.egl_client_apis, sizeof(receipt.egl_client_apis),
                 evidence->egl_client_apis);
    if (receipt.gles_major < 2 ||
        nxcompat_registry_publish_graphics(g_framework.registry, &receipt) !=
            NXCOMPAT_OK)
        goto done;
    g_framework.graphics_published = 1;
    fprintf(stderr,
            "[deadtrigger/framework] graphics %dx%d GLES=%s renderer=%s\n",
            receipt.drawable_width, receipt.drawable_height,
            receipt.gl_version, receipt.gl_renderer);
    result = evaluate_locked(NXCOMPAT_PHASE_GRAPHICS, 0);
    evaluate_ready_locked();
done:
    pthread_mutex_unlock(&g_framework_lock);
    return result;
}

int dt_framework_publish_current_graphics(void)
{
    dt_graphics_evidence evidence;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
    EGLint width = 0;
    EGLint height = 0;
    const char *video_backend;

    display = eglGetCurrentDisplay();
    context = eglGetCurrentContext();
    surface = eglGetCurrentSurface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT ||
        surface == EGL_NO_SURFACE ||
        !eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height) ||
        width <= 0 || height <= 0)
        return -1;
    config = current_config(display, context);
    if (!config)
        return -1;

    memset(&evidence, 0, sizeof(evidence));
    evidence.window_width = width;
    evidence.window_height = height;
    evidence.drawable_width = width;
    evidence.drawable_height = height;
    (void)eglGetConfigAttrib(display, config, EGL_RED_SIZE,
                             &evidence.red_bits);
    (void)eglGetConfigAttrib(display, config, EGL_GREEN_SIZE,
                             &evidence.green_bits);
    (void)eglGetConfigAttrib(display, config, EGL_BLUE_SIZE,
                             &evidence.blue_bits);
    (void)eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE,
                             &evidence.alpha_bits);
    (void)eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE,
                             &evidence.depth_bits);
    (void)eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE,
                             &evidence.stencil_bits);
    evidence.double_buffer = 1;
    (void)eglGetConfigAttrib(display, config, EGL_CONFIG_ID,
                             &evidence.egl_config_id);
    (void)eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE,
                             &evidence.egl_renderable_type);
    (void)eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE,
                             &evidence.egl_surface_type);
    video_backend = SDL_GetCurrentVideoDriver();
    evidence.backend = video_backend && video_backend[0] ? video_backend :
                                                          "raw-egl";
    evidence.gl_vendor = (const char *)glGetString(GL_VENDOR);
    evidence.gl_renderer = (const char *)glGetString(GL_RENDERER);
    evidence.gl_version = (const char *)glGetString(GL_VERSION);
    evidence.glsl_version =
        (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    evidence.gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    evidence.egl_vendor = eglQueryString(display, EGL_VENDOR);
    evidence.egl_version = eglQueryString(display, EGL_VERSION);
    evidence.egl_client_apis = eglQueryString(display, EGL_CLIENT_APIS);
    return dt_framework_publish_graphics(&evidence);
}
