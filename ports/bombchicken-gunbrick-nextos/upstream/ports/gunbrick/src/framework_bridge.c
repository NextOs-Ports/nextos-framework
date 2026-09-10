/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "framework_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxaudio.h"
#include "nxcompat.h"
#include "nxinput_nxcompat.h"

typedef struct gb_framework_state {
    nxcompat_host host;
    nxcompat_probe_result probe;
    nxcompat_plan_v2 plan;
    nxcompat_registry *registry;
    nxcompat_requirements requirements;
    uint64_t graphics_generation;
    uint64_t audio_generation;
    int initialized;
} gb_framework_state;

static gb_framework_state framework;

static void copy_bounded(char *output, size_t output_size, const char *value)
{
    if (!output || !output_size)
        return;
    snprintf(output, output_size, "%s", value ? value : "");
}

static int evaluate(nxcompat_phase phase, int require_complete)
{
    nxcompat_requirement_report report;
    nxcompat_result_code result;
    size_t index;

    if (!framework.initialized || !framework.registry)
        return -1;
    memset(&report, 0, sizeof(report));
    report.api_version = NXCOMPAT_API_VERSION;
    report.struct_size = sizeof(report);
    result = nxcompat_requirements_evaluate(framework.registry,
                                            &framework.requirements,
                                            phase, &report);
    fprintf(stderr,
            "[gunbrick/framework] phase=%s satisfied=%zu pending=%zu "
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
        fprintf(stderr, "[gunbrick/framework] missing capability=%s\n",
                definition ? definition->name : "unknown");
    }
    return result == NXCOMPAT_OK && report.missing_count == 0u &&
                   (!require_complete || report.pending_count == 0u)
               ? 0 : -1;
}

int gb_framework_preflight(const char *game_dir)
{
    nxcompat_probe_options probe_options;
    nxcompat_plan_options plan_options;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    const char *port_id;

    if (framework.initialized || !game_dir || game_dir[0] != '/')
        return -1;
    port_id = getenv("NXCOMPAT_PORT_ID");
    if (!port_id || !port_id[0])
        port_id = "gunbrick";
    memset(&probe_options, 0, sizeof(probe_options));
    probe_options.api_version = NXCOMPAT_API_VERSION;
    probe_options.struct_size = sizeof(probe_options);
    probe_options.port_id = port_id;
    probe_options.game_dir = game_dir;
    probe_options.portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");
    probe_options.result = &framework.probe;
    if (nxcompat_probe(&probe_options, &framework.host) != 0) {
        fprintf(stderr, "[gunbrick/framework] nxcompat probe failed\n");
        return -1;
    }

    memset(&plan_options, 0, sizeof(plan_options));
    plan_options.api_version = NXCOMPAT_API_VERSION;
    plan_options.struct_size = sizeof(plan_options);
    plan_options.runtime_arch = NXCOMPAT_ARCH_AARCH64;
    plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE |
                                NXCOMPAT_POLICY_LOW_MEMORY_ARENAS;
    plan_options.low_memory_arena_max = 2u;
    if (nxcompat_plan_environment_v2(&framework.host, &plan_options,
                                     &framework.plan) != NXCOMPAT_OK ||
        nxcompat_apply_environment_v2(&framework.plan) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[gunbrick/framework] environment plan/apply failed: %s\n",
                nxcompat_reason_name(framework.plan.final_reason));
        return -1;
    }
    if (nxcompat_registry_create(&framework.registry) != NXCOMPAT_OK ||
        nxcompat_registry_seed_host(framework.registry, &framework.host) !=
            NXCOMPAT_OK ||
        nxcompat_requirements_parse_runtime_ex(&framework.requirements,
                                                &reason) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[gunbrick/framework] registry/requirements rejected: %s\n",
                nxcompat_reason_name(reason));
        nxcompat_registry_destroy(framework.registry);
        framework.registry = NULL;
        return -1;
    }
    framework.initialized = 1;
    return evaluate(NXCOMPAT_PHASE_PREFLIGHT, 0);
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
        cursor++;
    if (sscanf(cursor, "%d.%d", &parsed_major, &parsed_minor) == 2 &&
        parsed_major >= 1 && parsed_major <= 9 &&
        parsed_minor >= 0 && parsed_minor <= 9) {
        *major = parsed_major;
        *minor = parsed_minor;
    }
}

int gb_framework_publish_graphics(const gb_graphics_evidence *evidence)
{
    nxcompat_graphics_receipt receipt;

    if (!framework.initialized || !evidence ||
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
        return -1;
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
    receipt.generation = ++framework.graphics_generation;
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
        nxcompat_registry_publish_graphics(framework.registry, &receipt) !=
            NXCOMPAT_OK)
        return -1;
    /* Publishing one strong receipt must not fail because another subsystem
     * has not published yet.  The READY gate below is the only cross-system
     * completeness decision. */
    (void)evaluate(NXCOMPAT_PHASE_GRAPHICS, 0);
    return 0;
}

int gb_framework_publish_input(const nxinput_context *input)
{
    nxcompat_input_receipt receipt;

    memset(&receipt, 0, sizeof(receipt));
    if (!framework.initialized || !input ||
        nxinput_nxcompat_publish_context(framework.registry, input,
                                         &receipt) != NXCOMPAT_OK)
        return -1;
    (void)evaluate(NXCOMPAT_PHASE_INPUT, 0);
    return 0;
}

int gb_framework_publish_audio(SDL_AudioDeviceID device,
                               const SDL_AudioSpec *actual)
{
    nxaudio_backend_observation observation;
    nxaudio_reason reason = NXAUDIO_REASON_NONE;
    nxcompat_audio_receipt receipt;
    const char *backend = SDL_GetCurrentAudioDriver();

    if (!framework.initialized || device == 0 || !actual || !backend)
        return -1;
    memset(&observation, 0, sizeof(observation));
    observation.api_version = NXAUDIO_API_VERSION;
    observation.struct_size = sizeof(observation);
    copy_bounded(observation.backend, sizeof(observation.backend), backend);
    observation.inherited_attempt =
        framework.host.inherited_audio_driver[0] != '\0';
    observation.server_reachable = 1;
    observation.device_opened = 1;
    if (nxaudio_classify_backend(&observation, &reason) != NXAUDIO_OK) {
        fprintf(stderr, "[gunbrick/framework] audio rejected: %s\n",
                nxaudio_reason_name(reason));
        return -1;
    }
    memset(&receipt, 0, sizeof(receipt));
    receipt.api_version = NXCOMPAT_API_VERSION;
    receipt.struct_size = sizeof(receipt);
    receipt.proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                          NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                          NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
    receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
    receipt.generation = ++framework.audio_generation;
    receipt.lifetime = NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED;
    receipt.frequency = actual->freq;
    receipt.format = actual->format;
    receipt.channels = actual->channels;
    receipt.samples = actual->samples;
    receipt.device_id_was_nonzero = 1;
    copy_bounded(receipt.backend, sizeof(receipt.backend), backend);
    if (nxcompat_registry_publish_audio(framework.registry, &receipt) !=
        NXCOMPAT_OK)
        return -1;
    (void)evaluate(NXCOMPAT_PHASE_AUDIO, 0);
    return 0;
}

int gb_framework_ready(void)
{
    return evaluate(NXCOMPAT_PHASE_READY, 1);
}
