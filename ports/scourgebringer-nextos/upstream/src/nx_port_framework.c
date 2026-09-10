/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE

#include "nx_port_framework.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#include <pthread.h>
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

#define NX_PORT_RUNTIME_JSON_MAX 32768u

typedef struct nx_port_framework_state {
    nxcompat_host host;
    nxcompat_probe_result probe;
    nxcompat_plan_v2 plan;
    nxcompat_registry *capabilities;
    nxcompat_requirements requirements;
    nxloader_registry *loader_registry;
    nxinput_context *input;
    nxgl_surface_state_v2 surface;
    uint64_t graphics_generation;
    uint64_t audio_generation;
    unsigned long input_poll_count;
    int graphics_published;
    int audio_published;
    int input_published;
    int initialized;
    int ready_logged;
} nx_port_framework_state;

typedef struct nx_port_delegate {
    nx_port_runtime_fn runtime;
    int argc;
    char **argv;
    int status;
} nx_port_delegate;

static nx_port_framework_state g_nx_port;
static pthread_mutex_t g_nx_port_lock = PTHREAD_MUTEX_INITIALIZER;

static int nx_port_copy(char *destination, size_t destination_size,
                        const char *source)
{
    size_t length;

    if (!destination || destination_size == 0u)
        return 0;
    destination[0] = '\0';
    if (!source)
        return 1;
    length = strlen(source);
    if (length >= destination_size)
        return 0;
    memcpy(destination, source, length + 1u);
    return 1;
}

static void nx_port_log_runtime_locked(nxcompat_phase phase)
{
    nxcompat_runtime_report report;
    nxcompat_result_code status;
    char *json;

    if (!g_nx_port.capabilities)
        return;
    memset(&report, 0, sizeof(report));
    status = nxcompat_registry_runtime_report(g_nx_port.capabilities,
                                               &g_nx_port.requirements,
                                               phase, &report);
    if (status != NXCOMPAT_OK && status != NXCOMPAT_FAILED)
        return;
    json = (char *)malloc(NX_PORT_RUNTIME_JSON_MAX);
    if (!json)
        return;
    if (nxcompat_format_runtime_json(&g_nx_port.host, &g_nx_port.plan,
                                     &report, json,
                                     NX_PORT_RUNTIME_JSON_MAX) >= 0)
        fprintf(stderr, "[nxfw] NXCOMPAT_REPORT %s\n", json);
    free(json);
}

static int nx_port_requirements_locked(nxcompat_phase phase)
{
    nxcompat_requirement_report report;
    nxcompat_result_code result;

    memset(&report, 0, sizeof(report));
    result = nxcompat_requirements_evaluate(g_nx_port.capabilities,
                                            &g_nx_port.requirements, phase,
                                            &report);
    if (result != NXCOMPAT_OK && result != NXCOMPAT_FAILED) {
        fprintf(stderr, "[nxfw] requirements phase=%s rc=%d\n",
                nxcompat_phase_name(phase), (int)result);
        return -1;
    }
    fprintf(stderr,
            "[nxfw] requirements phase=%s satisfied=%zu pending=%zu missing=%zu\n",
            nxcompat_phase_name(phase), report.satisfied_count,
            report.pending_count, report.missing_count);
    nx_port_log_runtime_locked(phase);
    if (!g_nx_port.ready_logged && report.pending_count == 0u &&
        report.missing_count == 0u) {
        g_nx_port.ready_logged = 1;
        fprintf(stderr,
                "[nxfw] READY nxbootstrap=0.6.8 nxloader=%s nxcompat=%s "
                "nxgl=%s nxaudio=%s nxinput=%s nxandroid=%s\n",
                NXLOADER_VERSION_STRING, NXCOMPAT_VERSION, NXGL_VERSION,
                NXAUDIO_VERSION, NXINPUT_VERSION, NXANDROID_VERSION);
    }
    return report.missing_count == 0u ? 0 : -1;
}

static int nx_port_preflight(const char *fallback_port_id)
{
    nxcompat_probe_options probe_options;
    nxcompat_plan_options plan_options;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    const char *port_id;
    const char *game_dir;
    char cwd[NXCOMPAT_PATH_MAX];

    memset(&g_nx_port, 0, sizeof(g_nx_port));
    port_id = getenv("NXCOMPAT_PORT_ID");
    if (!port_id || !port_id[0])
        port_id = fallback_port_id;
    game_dir = getenv("NXCOMPAT_GAME_DIR");
    if (!game_dir || game_dir[0] != '/') {
        if (!getcwd(cwd, sizeof(cwd)))
            return -1;
        game_dir = cwd;
    }

    memset(&probe_options, 0, sizeof(probe_options));
    probe_options.api_version = NXCOMPAT_API_VERSION;
    probe_options.struct_size = sizeof(probe_options);
    probe_options.port_id = port_id;
    probe_options.game_dir = game_dir;
    probe_options.portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");
    probe_options.result = &g_nx_port.probe;
    if (nxcompat_probe(&probe_options, &g_nx_port.host) != NXCOMPAT_OK) {
        fprintf(stderr, "[nxfw] nxcompat probe failed\n");
        return -1;
    }

    memset(&plan_options, 0, sizeof(plan_options));
    plan_options.api_version = NXCOMPAT_API_VERSION;
    plan_options.struct_size = sizeof(plan_options);
    plan_options.runtime_arch = NXCOMPAT_ARCH_AARCH64;
    plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE;
    if (nxcompat_plan_environment_v2(&g_nx_port.host, &plan_options,
                                     &g_nx_port.plan) != NXCOMPAT_OK ||
        nxcompat_apply_environment_v2(&g_nx_port.plan) != NXCOMPAT_OK) {
        fprintf(stderr, "[nxfw] nxcompat plan/apply failed reason=%s\n",
                nxcompat_reason_name(g_nx_port.plan.final_reason));
        return -1;
    }

    if (nxcompat_registry_create(&g_nx_port.capabilities) != NXCOMPAT_OK ||
        nxcompat_registry_seed_host(g_nx_port.capabilities,
                                    &g_nx_port.host) != NXCOMPAT_OK ||
        nxcompat_requirements_parse_runtime_ex(&g_nx_port.requirements,
                                                &reason) != NXCOMPAT_OK) {
        fprintf(stderr, "[nxfw] registry/requirements failed reason=%s\n",
                nxcompat_reason_name(reason));
        return -1;
    }
    if (nxloader_registry_create(&g_nx_port.loader_registry) != NXLOADER_OK) {
        fprintf(stderr, "[nxfw] nxloader registry creation failed\n");
        return -1;
    }

    g_nx_port.initialized = 1;
    fprintf(stderr,
            "[nxfw] framework runtime linked: nxbootstrap=0.6.8 "
            "nxloader=%s nxcompat=%s nxgl=%s nxaudio=%s nxinput=%s "
            "nxandroid=%s\n",
            NXLOADER_VERSION_STRING, NXCOMPAT_VERSION, NXGL_VERSION,
            NXAUDIO_VERSION, NXINPUT_VERSION, NXANDROID_VERSION);
    return nx_port_requirements_locked(NXCOMPAT_PHASE_PREFLIGHT);
}

typedef EGLDisplay (*nx_port_egl_get_current_display_fn)(void);
typedef EGLContext (*nx_port_egl_get_current_context_fn)(void);
typedef EGLSurface (*nx_port_egl_get_current_surface_fn)(EGLint);
typedef const char *(*nx_port_egl_query_string_fn)(EGLDisplay, EGLint);
typedef EGLBoolean (*nx_port_egl_query_context_fn)(EGLDisplay, EGLContext,
                                                    EGLint, EGLint *);
typedef EGLBoolean (*nx_port_egl_query_surface_fn)(EGLDisplay, EGLSurface,
                                                    EGLint, EGLint *);
typedef EGLBoolean (*nx_port_egl_choose_config_fn)(EGLDisplay, const EGLint *,
                                                   EGLConfig *, EGLint,
                                                   EGLint *);
typedef EGLBoolean (*nx_port_egl_get_config_attrib_fn)(EGLDisplay, EGLConfig,
                                                        EGLint, EGLint *);

typedef struct nx_port_egl_api {
    nx_port_egl_get_current_display_fn get_current_display;
    nx_port_egl_get_current_context_fn get_current_context;
    nx_port_egl_get_current_surface_fn get_current_surface;
    nx_port_egl_query_string_fn query_string;
    nx_port_egl_query_context_fn query_context;
    nx_port_egl_query_surface_fn query_surface;
    nx_port_egl_choose_config_fn choose_config;
    nx_port_egl_get_config_attrib_fn get_config_attrib;
} nx_port_egl_api;

static int nx_port_load_proc(void *destination, size_t destination_size,
                             const char *name)
{
    void *symbol;

    if (!destination || destination_size != sizeof(symbol) || !name)
        return 0;
    symbol = SDL_GL_GetProcAddress(name);
    if (!symbol)
        return 0;
    memcpy(destination, &symbol, destination_size);
    return 1;
}

static int nx_port_load_egl(nx_port_egl_api *api)
{
    memset(api, 0, sizeof(*api));
    return nx_port_load_proc(&api->get_current_display,
                             sizeof(api->get_current_display),
                             "eglGetCurrentDisplay") &&
           nx_port_load_proc(&api->get_current_context,
                             sizeof(api->get_current_context),
                             "eglGetCurrentContext") &&
           nx_port_load_proc(&api->get_current_surface,
                             sizeof(api->get_current_surface),
                             "eglGetCurrentSurface") &&
           nx_port_load_proc(&api->query_string, sizeof(api->query_string),
                             "eglQueryString") &&
           nx_port_load_proc(&api->query_context, sizeof(api->query_context),
                             "eglQueryContext") &&
           nx_port_load_proc(&api->query_surface, sizeof(api->query_surface),
                             "eglQuerySurface") &&
           nx_port_load_proc(&api->choose_config, sizeof(api->choose_config),
                             "eglChooseConfig") &&
           nx_port_load_proc(&api->get_config_attrib,
                             sizeof(api->get_config_attrib),
                             "eglGetConfigAttrib");
}

static int nx_port_egl_attribute(const nx_port_egl_api *api,
                                 EGLDisplay display, EGLConfig config,
                                 EGLint attribute, int *destination)
{
    EGLint value = 0;

    if (api->get_config_attrib(display, config, attribute, &value) != EGL_TRUE)
        return 0;
    *destination = (int)value;
    return 1;
}

static void nx_port_capture_egl(nxcompat_graphics_receipt *receipt)
{
    nx_port_egl_api api;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config = (EGLConfig)0;
    EGLint config_id = 0;
    EGLint config_count = 0;
    EGLint render_buffer = 0;
    EGLint attributes[3];
    const char *vendor;
    const char *version;
    const char *client_apis;

    if (!nx_port_load_egl(&api))
        return;
    display = api.get_current_display();
    context = api.get_current_context();
    surface = api.get_current_surface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT ||
        surface == EGL_NO_SURFACE)
        return;
    if (receipt->double_buffer < 0 &&
        api.query_surface(display, surface, EGL_RENDER_BUFFER,
                          &render_buffer) == EGL_TRUE &&
        (render_buffer == EGL_BACK_BUFFER ||
         render_buffer == EGL_SINGLE_BUFFER))
        receipt->double_buffer =
            render_buffer == EGL_BACK_BUFFER ? 1 : 0;
    vendor = api.query_string(display, EGL_VENDOR);
    version = api.query_string(display, EGL_VERSION);
    client_apis = api.query_string(display, EGL_CLIENT_APIS);
    if (!vendor || !vendor[0] || !version || !version[0] ||
        !client_apis || !client_apis[0] ||
        !nx_port_copy(receipt->egl_vendor, sizeof(receipt->egl_vendor),
                      vendor) ||
        !nx_port_copy(receipt->egl_version, sizeof(receipt->egl_version),
                      version) ||
        !nx_port_copy(receipt->egl_client_apis,
                      sizeof(receipt->egl_client_apis), client_apis))
        return;

    receipt->proof_flags |= NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                            NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT;
    if (api.query_context(display, context, EGL_CONFIG_ID, &config_id) !=
            EGL_TRUE ||
        config_id <= 0)
        return;
    attributes[0] = EGL_CONFIG_ID;
    attributes[1] = config_id;
    attributes[2] = EGL_NONE;
    if (api.choose_config(display, attributes, &config, 1, &config_count) !=
            EGL_TRUE ||
        config_count != 1 || !config)
        return;

    receipt->egl_config_id = (int)config_id;
    if (!nx_port_egl_attribute(&api, display, config, EGL_RED_SIZE,
                               &receipt->egl_red_bits) ||
        !nx_port_egl_attribute(&api, display, config, EGL_GREEN_SIZE,
                               &receipt->egl_green_bits) ||
        !nx_port_egl_attribute(&api, display, config, EGL_BLUE_SIZE,
                               &receipt->egl_blue_bits) ||
        !nx_port_egl_attribute(&api, display, config, EGL_ALPHA_SIZE,
                               &receipt->egl_alpha_bits) ||
        !nx_port_egl_attribute(&api, display, config, EGL_DEPTH_SIZE,
                               &receipt->egl_depth_bits) ||
        !nx_port_egl_attribute(&api, display, config, EGL_STENCIL_SIZE,
                               &receipt->egl_stencil_bits) ||
        !nx_port_egl_attribute(&api, display, config, EGL_RENDERABLE_TYPE,
                               &receipt->egl_renderable_type) ||
        !nx_port_egl_attribute(&api, display, config, EGL_SURFACE_TYPE,
                               &receipt->egl_surface_type)) {
        receipt->egl_config_id = 0;
        receipt->egl_red_bits = 0;
        receipt->egl_green_bits = 0;
        receipt->egl_blue_bits = 0;
        receipt->egl_alpha_bits = 0;
        receipt->egl_depth_bits = 0;
        receipt->egl_stencil_bits = 0;
        receipt->egl_renderable_type = 0;
        receipt->egl_surface_type = 0;
        return;
    }
    receipt->proof_flags |= NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED;
}

int nx_port_framework_graphics_ready(void *sdl_window)
{
    SDL_Window *window = (SDL_Window *)sdl_window;
    nxcompat_graphics_receipt receipt;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    nxgl_surface_observation_v2 observation;
    nxgl_surface_metrics_input_v2 metrics_input;
    nxgl_surface_metrics_v2 metrics;
    const GLubyte *vendor;
    const GLubyte *renderer;
    const GLubyte *version;
    const GLubyte *glsl;
    const GLubyte *extensions;
    const char *backend;
    int result = -1;

    if (!g_nx_port.initialized || !window ||
        SDL_GL_GetCurrentWindow() != window ||
        SDL_GL_GetCurrentContext() == NULL) {
        fprintf(stderr, "[nxfw] graphics receipt refused: context not current\n");
        return -1;
    }

    memset(&receipt, 0, sizeof(receipt));
    receipt.api_version = NXCOMPAT_API_VERSION;
    receipt.struct_size = sizeof(receipt);
    receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
    receipt.generation = ++g_nx_port.graphics_generation;
    receipt.proof_flags = NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                          NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT;
    SDL_GetWindowSize(window, &receipt.window_width, &receipt.window_height);
    SDL_GL_GetDrawableSize(window, &receipt.drawable_width,
                           &receipt.drawable_height);
    backend = SDL_GetCurrentVideoDriver();
    if (!backend || !backend[0] ||
        !nx_port_copy(receipt.video_backend, sizeof(receipt.video_backend),
                      backend))
        return -1;
    if (receipt.drawable_width > 0 && receipt.drawable_height > 0)
        receipt.proof_flags |= NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE;

    receipt.double_buffer = -1;
    if (SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER,
                            &receipt.double_buffer) != 0)
        receipt.double_buffer = -1;
    if (SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &receipt.red_bits) != 0 ||
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &receipt.green_bits) != 0 ||
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &receipt.blue_bits) != 0 ||
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &receipt.alpha_bits) != 0 ||
        SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &receipt.depth_bits) != 0 ||
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &receipt.stencil_bits) != 0 ||
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            &receipt.profile_mask) != 0) {
        fprintf(stderr, "[nxfw] graphics receipt refused: SDL config query\n");
        return -1;
    }

    vendor = glGetString(GL_VENDOR);
    renderer = glGetString(GL_RENDERER);
    version = glGetString(GL_VERSION);
    glsl = glGetString(GL_SHADING_LANGUAGE_VERSION);
    extensions = glGetString(GL_EXTENSIONS);
    if (!vendor || !vendor[0] || !renderer || !renderer[0] ||
        !version || !version[0] || !glsl || !glsl[0] ||
        nxgl_parse_gles_version((const char *)version, &receipt.gles_major,
                                &receipt.gles_minor) != NXGL_SUCCESS ||
        !nx_port_copy(receipt.gl_vendor, sizeof(receipt.gl_vendor),
                      (const char *)vendor) ||
        !nx_port_copy(receipt.gl_renderer, sizeof(receipt.gl_renderer),
                      (const char *)renderer) ||
        !nx_port_copy(receipt.gl_version, sizeof(receipt.gl_version),
                      (const char *)version) ||
        !nx_port_copy(receipt.glsl_version, sizeof(receipt.glsl_version),
                      (const char *)glsl)) {
        fprintf(stderr, "[nxfw] graphics receipt refused: real GL strings\n");
        return -1;
    }
    if (!nx_port_copy(receipt.gl_extensions, sizeof(receipt.gl_extensions),
                      (const char *)extensions))
        receipt.gl_extensions[0] = '\0';
    receipt.proof_flags |= NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL;
    nx_port_capture_egl(&receipt);
    if (receipt.double_buffer < 0) {
        fprintf(stderr,
                "[nxfw] graphics receipt refused: render buffer query\n");
        return -1;
    }

    pthread_mutex_lock(&g_nx_port_lock);
    if (nxcompat_registry_publish_graphics_ex(g_nx_port.capabilities, &receipt,
                                               &reason) != NXCOMPAT_OK) {
        fprintf(stderr, "[nxfw] graphics publish failed reason=%s\n",
                nxcompat_reason_name(reason));
        goto out;
    }
    g_nx_port.graphics_published = 1;

    nxgl_surface_state_v2_init(&g_nx_port.surface);
    memset(&observation, 0, sizeof(observation));
    observation.api_version = NXGL_API_VERSION_V2;
    observation.struct_size = sizeof(observation);
    observation.event = NXGL_SURFACE_EVENT_V2_RESIZED;
    observation.window_width = receipt.window_width;
    observation.window_height = receipt.window_height;
    observation.drawable_width = receipt.drawable_width;
    observation.drawable_height = receipt.drawable_height;
    if (nxgl_surface_observe_v2(&g_nx_port.surface, &observation) !=
        NXGL_SUCCESS) {
        fprintf(stderr, "[nxfw] nxgl surface observation failed\n");
        goto out;
    }

    memset(&metrics_input, 0, sizeof(metrics_input));
    metrics_input.api_version = NXGL_API_VERSION_V2;
    metrics_input.struct_size = sizeof(metrics_input);
    metrics_input.display_width = receipt.window_width;
    metrics_input.display_height = receipt.window_height;
    metrics_input.drawable_width = receipt.drawable_width;
    metrics_input.drawable_height = receipt.drawable_height;
    metrics_input.viewport_width = receipt.drawable_width;
    metrics_input.viewport_height = receipt.drawable_height;
    metrics_input.render_target_width = receipt.drawable_width;
    metrics_input.render_target_height = receipt.drawable_height;
    if (nxgl_calculate_surface_metrics_v2(&metrics_input, &metrics) !=
        NXGL_SUCCESS) {
        fprintf(stderr, "[nxfw] nxgl metrics calculation failed\n");
        goto out;
    }

    fprintf(stderr,
            "[nxfw] graphics-open backend=%s GLES=%d.%d drawable=%dx%d "
            "egl=%s config=%s scale=%.3fx%.3f\n",
            receipt.video_backend, receipt.gles_major, receipt.gles_minor,
            receipt.drawable_width, receipt.drawable_height,
            (receipt.proof_flags &
             NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT) ? "yes" : "no",
            (receipt.proof_flags &
             NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED) ? "yes" : "no",
            metrics.drawable_per_display_scale_x,
            metrics.drawable_per_display_scale_y);
    result = nx_port_requirements_locked(
        g_nx_port.audio_published && g_nx_port.input_published
            ? NXCOMPAT_PHASE_READY
            : NXCOMPAT_PHASE_GRAPHICS);
out:
    pthread_mutex_unlock(&g_nx_port_lock);
    return result;
}

int nx_port_framework_input_ready(void)
{
    nxinput_config config;
    nxcompat_input_receipt receipt;
    int result = -1;

    if (!g_nx_port.initialized)
        return -1;
    if (g_nx_port.input)
        return 0;
    nxinput_config_init(&config);
    g_nx_port.input = nxinput_create(&config);
    if (!g_nx_port.input) {
        fprintf(stderr, "[nxfw] nxinput create failed: %s\n", SDL_GetError());
        return -1;
    }

    memset(&receipt, 0, sizeof(receipt));
    pthread_mutex_lock(&g_nx_port_lock);
    if (nxinput_nxcompat_publish_context(g_nx_port.capabilities,
                                         g_nx_port.input,
                                         &receipt) != NXCOMPAT_OK) {
        fprintf(stderr, "[nxfw] nxinput receipt publish failed\n");
        goto out;
    }
    g_nx_port.input_published = 1;
    fprintf(stderr,
            "[nxfw] input-open controllers=%u mapping_source=%d "
            "topology_generation=%llu\n",
            receipt.connected_count, (int)receipt.mapping_source,
            (unsigned long long)receipt.topology_generation);
    if (g_nx_port.graphics_published && g_nx_port.audio_published)
        result = nx_port_requirements_locked(NXCOMPAT_PHASE_READY);
    else {
        (void)nx_port_requirements_locked(NXCOMPAT_PHASE_INPUT);
        result = 0;
    }
out:
    pthread_mutex_unlock(&g_nx_port_lock);
    return result;
}

void nx_port_framework_observe_event(const void *sdl_event)
{
    const SDL_Event *event = (const SDL_Event *)sdl_event;

    if (g_nx_port.input && event)
        nxinput_observe_event(g_nx_port.input, event);
}

int nx_port_framework_poll_input(void)
{
    int quit;

    if (!g_nx_port.input)
        return 0;
    nxinput_poll(g_nx_port.input);
    ++g_nx_port.input_poll_count;
    if ((g_nx_port.input_poll_count % 120u) == 0u) {
        nxcompat_input_receipt receipt;
        memset(&receipt, 0, sizeof(receipt));
        pthread_mutex_lock(&g_nx_port_lock);
        if (nxinput_nxcompat_publish_context(g_nx_port.capabilities,
                                             g_nx_port.input,
                                             &receipt) == NXCOMPAT_OK) {
            fprintf(stderr,
                    "[nxfw] input-topology controllers=%u generation=%llu\n",
                    receipt.connected_count,
                    (unsigned long long)receipt.topology_generation);
            nx_port_log_runtime_locked(NXCOMPAT_PHASE_INPUT);
        }
        pthread_mutex_unlock(&g_nx_port_lock);
    }
    quit = nxinput_consume_quit_request(g_nx_port.input);
    if (quit)
        fprintf(stderr, "[nxfw] input requested normal shutdown\n");
    return quit;
}

void nx_port_framework_audio_opened(uint32_t device_id, int frequency,
                                    uint32_t format, unsigned channels,
                                    unsigned samples)
{
    nxaudio_backend_observation observation;
    nxaudio_reason audio_reason = NXAUDIO_REASON_NONE;
    nxcompat_audio_receipt receipt;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    const char *backend;

    if (!g_nx_port.initialized || device_id == 0u)
        return;
    backend = SDL_GetCurrentAudioDriver();
    if (!backend || !backend[0]) {
        fprintf(stderr, "[nxfw] audio opened without a named SDL backend\n");
        return;
    }

    memset(&observation, 0, sizeof(observation));
    observation.api_version = NXAUDIO_API_VERSION;
    observation.struct_size = sizeof(observation);
    nx_port_copy(observation.backend, sizeof(observation.backend), backend);
    observation.inherited_attempt =
        g_nx_port.host.inherited_audio_driver[0] != '\0';
    observation.server_reachable = 1;
    observation.device_opened = 1;
    if (nxaudio_classify_backend(&observation, &audio_reason) != NXAUDIO_OK) {
        fprintf(stderr, "[nxfw] audio backend rejected reason=%d\n",
                (int)audio_reason);
        return;
    }

    memset(&receipt, 0, sizeof(receipt));
    receipt.api_version = NXCOMPAT_API_VERSION;
    receipt.struct_size = sizeof(receipt);
    receipt.proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                          NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                          NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
    receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
    receipt.generation = ++g_nx_port.audio_generation;
    receipt.lifetime = NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED;
    receipt.frequency = frequency;
    receipt.format = format;
    receipt.channels = channels;
    receipt.samples = samples;
    receipt.device_id_was_nonzero = 1;
    nx_port_copy(receipt.backend, sizeof(receipt.backend), backend);

    pthread_mutex_lock(&g_nx_port_lock);
    if (nxcompat_registry_publish_audio_ex(g_nx_port.capabilities, &receipt,
                                            &reason) == NXCOMPAT_OK) {
        g_nx_port.audio_published = 1;
        fprintf(stderr,
                "[nxfw] audio-open backend=%s %dHz %uch %u samples "
                "device=%u reason=%d\n",
                receipt.backend, frequency, channels, samples, device_id,
                (int)audio_reason);
        (void)nx_port_requirements_locked(
            g_nx_port.graphics_published && g_nx_port.input_published
                ? NXCOMPAT_PHASE_READY
                : NXCOMPAT_PHASE_AUDIO);
    } else {
        fprintf(stderr, "[nxfw] audio receipt publish failed reason=%s\n",
                nxcompat_reason_name(reason));
    }
    pthread_mutex_unlock(&g_nx_port_lock);
}

void nx_port_framework_audio_failed(void)
{
    fprintf(stderr, "[nxfw] audio device-open failed\n");
}

static void nx_port_shutdown(void)
{
    if (g_nx_port.input) {
        nxinput_destroy(g_nx_port.input);
        g_nx_port.input = NULL;
    }
    nxloader_registry_destroy(g_nx_port.loader_registry);
    g_nx_port.loader_registry = NULL;
    nxcompat_registry_destroy(g_nx_port.capabilities);
    g_nx_port.capabilities = NULL;
    g_nx_port.initialized = 0;
}

static int nx_port_delegate_invoke(void *userdata,
                                   const nxandroid_step *step)
{
    nx_port_delegate *delegate = (nx_port_delegate *)userdata;

    if (!delegate || !step || !delegate->runtime)
        return 127;
    if (step->phase == NXANDROID_PHASE_MODULE_INITIALIZED ||
        step->phase == NXANDROID_PHASE_ACTIVITY_CREATE) {
        fprintf(stderr,
                "[nxfw] nxandroid phase=%s contract=%s; delegated host "
                "owner declared (guest native flow not advanced)\n",
                nxandroid_phase_name(step->phase), step->contract_id);
        return 0;
    }
    if (step->phase != NXANDROID_PHASE_RUNTIME_DELEGATED)
        return 127;
    fprintf(stderr,
            "[nxfw] nxandroid phase=%s contract=%s; native flow remains "
            "adapter-owned\n",
            nxandroid_phase_name(step->phase), step->contract_id);
    delegate->status =
        delegate->runtime(delegate->argc, delegate->argv);
    return delegate->status;
}

int nx_port_framework_run(const char *fallback_port_id, int argc,
                          char **argv, nx_port_runtime_fn runtime)
{
    static const nxandroid_module_spec modules[] = {
        {"nx-port-delegated-host-owner", NXANDROID_JNI_NONE},
    };
    static const nxandroid_step steps[] = {
        {
            NXANDROID_PHASE_MODULE_INITIALIZED,
            0u,
            0u,
            "port-delegated-host-owner-initialized-v1",
            NULL,
            NXANDROID_TERMINAL_NONE,
            0u,
            0u,
        },
        {
            NXANDROID_PHASE_ACTIVITY_CREATE,
            NXANDROID_NO_MODULE,
            0u,
            "port-delegated-host-owner-context-v1",
            NULL,
            NXANDROID_TERMINAL_NONE,
            0u,
            0u,
        },
        {
            NXANDROID_PHASE_RUNTIME_DELEGATED,
            NXANDROID_NO_MODULE,
            0u,
            "port-native-flow-delegated-runtime-v1",
            NULL,
            NXANDROID_TERMINAL_NONE,
            0u,
            0u,
        },
    };
    nxandroid_profile profile;
    nxandroid_ops ops;
    nxandroid_context *context = NULL;
    nxandroid_result result;
    nx_port_delegate delegate;
    int status = 1;

    if (!fallback_port_id || !fallback_port_id[0] || !runtime)
        return 1;
    if (nx_port_preflight(fallback_port_id) != 0) {
        nx_port_shutdown();
        return 1;
    }

    memset(&delegate, 0, sizeof(delegate));
    delegate.runtime = runtime;
    delegate.argc = argc;
    delegate.argv = argv;
    delegate.status = 1;
    memset(&profile, 0, sizeof(profile));
    profile.api_version = NXANDROID_API_VERSION;
    profile.struct_size = sizeof(profile);
    profile.modules = modules;
    profile.module_count = sizeof(modules) / sizeof(modules[0]);
    profile.steps = steps;
    profile.step_count = sizeof(steps) / sizeof(steps[0]);
    profile.flags = NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME;
    memset(&ops, 0, sizeof(ops));
    ops.api_version = NXANDROID_API_VERSION;
    ops.struct_size = sizeof(ops);
    ops.invoke = nx_port_delegate_invoke;
    ops.userdata = &delegate;

    result = nxandroid_context_create(&profile, &ops, &context);
    if (result != NXANDROID_OK) {
        fprintf(stderr, "[nxfw] nxandroid context create failed: %s\n",
                nxandroid_result_string(result));
        nx_port_shutdown();
        return 1;
    }
    result = nxandroid_context_run(context);
    status = delegate.status;
    if (result != NXANDROID_OK) {
        fprintf(stderr, "[nxfw] nxandroid runtime returned: %s status=%d\n",
                nxandroid_result_string(result), status);
        if (status == 0)
            status = 1;
    } else {
        fprintf(stderr, "[nxfw] nxandroid delegated runtime complete status=%d\n",
                status);
    }
    (void)nxandroid_context_destroy(&context);
    nx_port_shutdown();
    return status;
}
