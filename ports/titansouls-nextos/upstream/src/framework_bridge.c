/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "framework_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxaudio.h"
#include "nxcompat.h"
#include "nxinput_nxcompat.h"

#define TS_AUDIO_CONTRACT_ID "titansouls-fmodex-opensl-sdl-v2"

typedef struct ts_framework_state {
    nxcompat_host host;
    nxcompat_probe_result probe;
    nxcompat_plan_v2 plan;
    nxcompat_registry *registry;
    nxcompat_requirements requirements;
    uint64_t graphics_generation;
    uint64_t audio_generation;
    int audio_contract_validated;
    int graphics_published;
    int input_published;
    int audio_published;
    int ready_validated;
    int initialized;
} ts_framework_state;

static ts_framework_state framework;

static int fixed_string_length(const char *value, size_t capacity,
                               size_t *length)
{
    size_t index;

    if (!value || capacity == 0u)
        return -1;
    for (index = 0u; index < capacity; ++index) {
        if (value[index] == '\0') {
            if (length)
                *length = index;
            return 0;
        }
    }
    return -1;
}

static int fixed_string_valid(const char *value, size_t capacity,
                              int require_nonempty)
{
    size_t length = 0u;

    return fixed_string_length(value, capacity, &length) == 0 &&
           (!require_nonempty || length != 0u);
}

static int copy_fixed(char *output, size_t output_size, const char *value,
                      size_t value_capacity, int require_nonempty)
{
    size_t length;

    if (!output || output_size == 0u ||
        !fixed_string_valid(value, value_capacity, require_nonempty))
        return -1;
    if (fixed_string_length(value, value_capacity, &length) != 0)
        return -1;
    if (length >= output_size)
        return -1;
    memcpy(output, value, length + 1u);
    return 0;
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
            "[titansouls/framework] phase=%s satisfied=%zu pending=%zu "
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
        fprintf(stderr, "[titansouls/framework] missing capability=%s\n",
                definition ? definition->name : "unknown");
    }
    return result == NXCOMPAT_OK && report.missing_count == 0u &&
                   (!require_complete || report.pending_count == 0u)
               ? 0
               : -1;
}

/* Receipts may arrive in either EGL->OpenSL or OpenSL->EGL order.  Pending is
 * not an error: the first successful publication that completes the trio owns
 * the fail-closed READY transition.  Once validated, later authoritative
 * receipt refreshes do not manufacture a second lifecycle transition. */
static int maybe_validate_ready(void)
{
    if (framework.ready_validated)
        return 0;
    if (!framework.graphics_published || !framework.input_published ||
        !framework.audio_published)
        return 0;
    if (ts_framework_ready() != 0) {
        fprintf(stderr,
                "[titansouls/framework] READY gate rejected complete "
                "receipt set\n");
        return -1;
    }
    fprintf(stderr, "[titansouls/framework] READY gate accepted\n");
    return 0;
}

static int validate_audio_contract(void)
{
    nxaudio_adapter_request request;
    nxaudio_reason reason = NXAUDIO_REASON_NONE;

    memset(&request, 0, sizeof(request));
    request.api_version = NXAUDIO_API_VERSION;
    request.struct_size = sizeof(request);
    request.stack = NXAUDIO_STACK_FMOD_EX;
    request.contract_id = TS_AUDIO_CONTRACT_ID;
    request.guest_uses_stack = 1;
    request.canonical_recipe = 0;
    request.bundles_external_provider = 0;
    if (nxaudio_adapter_validate(&request, &reason) != NXAUDIO_OK) {
        fprintf(stderr,
                "[titansouls/framework] audio contract rejected: %s\n",
                nxaudio_reason_name(reason));
        return -1;
    }
    framework.audio_contract_validated = 1;
    return 0;
}

int ts_framework_preflight(const char *game_dir)
{
    nxcompat_probe_options probe_options;
    nxcompat_plan_options plan_options;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    const char *port_id;

    if (framework.initialized || !game_dir || game_dir[0] != '/')
        return -1;
    if (validate_audio_contract() != 0)
        return -1;

    port_id = getenv("NXCOMPAT_PORT_ID");
    if (!port_id || !port_id[0])
        port_id = "titansouls";
    memset(&probe_options, 0, sizeof(probe_options));
    probe_options.api_version = NXCOMPAT_API_VERSION;
    probe_options.struct_size = sizeof(probe_options);
    probe_options.port_id = port_id;
    probe_options.game_dir = game_dir;
    probe_options.portmaster_dir = getenv("NXCOMPAT_PORTMASTER_DIR");
    probe_options.result = &framework.probe;
    if (nxcompat_probe(&probe_options, &framework.host) != 0) {
        fprintf(stderr, "[titansouls/framework] nxcompat probe failed\n");
        return -1;
    }

    memset(&plan_options, 0, sizeof(plan_options));
    plan_options.api_version = NXCOMPAT_API_VERSION;
    plan_options.struct_size = sizeof(plan_options);
    plan_options.runtime_arch = NXCOMPAT_ARCH_ARMV7;
    /* Titan Souls keeps its proven allocation behavior.  In particular this
     * bridge does not opt into the low-memory arena quirk. */
    plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE;
    plan_options.low_memory_arena_max = 0u;
    if (nxcompat_plan_environment_v2(&framework.host, &plan_options,
                                     &framework.plan) != NXCOMPAT_OK ||
        nxcompat_apply_environment_v2(&framework.plan) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[titansouls/framework] environment plan/apply failed: %s\n",
                nxcompat_reason_name(framework.plan.final_reason));
        return -1;
    }
    if (nxcompat_registry_create(&framework.registry) != NXCOMPAT_OK ||
        nxcompat_registry_seed_host(framework.registry, &framework.host) !=
            NXCOMPAT_OK ||
        nxcompat_requirements_parse_runtime_ex(&framework.requirements,
                                                &reason) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[titansouls/framework] registry/requirements rejected: %s\n",
                nxcompat_reason_name(reason));
        nxcompat_registry_destroy(framework.registry);
        framework.registry = NULL;
        return -1;
    }
    framework.initialized = 1;
    return evaluate(NXCOMPAT_PHASE_PREFLIGHT, 0);
}

static int graphics_report_valid(const nxgl_report_v2 *report)
{
    const nxgl_report *legacy;
    const nxgl_stack_handles_v2 *handles;
    const nxgl_egl_actual_v2 *egl;
    int parsed_major = 0;
    int parsed_minor = 0;

    if (!report || report->api_version != NXGL_API_VERSION_V2 ||
        report->struct_size < sizeof(*report) ||
        report->final_stage != NXGL_OPEN_STAGE_V2_CURRENT_VALIDATE ||
        report->final_reason != NXGL_OPEN_REASON_V2_SELECTED ||
        (report->stack_owner != NXGL_STACK_OWNER_V2_SDL_EGL &&
         report->stack_owner != NXGL_STACK_OWNER_V2_RAW_EGL))
        return 0;

    legacy = &report->legacy;
    handles = &report->handles;
    egl = &report->egl;
    if (legacy->api_version != NXGL_API_VERSION_V1 ||
        legacy->struct_size < sizeof(*legacy) ||
        legacy->window_width <= 0 || legacy->window_height <= 0 ||
        legacy->drawable_width <= 0 || legacy->drawable_height <= 0 ||
        legacy->selected_candidate_index == (size_t)-1 ||
        legacy->actual.gles_major < 2 ||
        legacy->actual.profile_mask != SDL_GL_CONTEXT_PROFILE_ES ||
        !fixed_string_valid(legacy->video_backend,
                            sizeof(legacy->video_backend), 1) ||
        !fixed_string_valid(legacy->vendor, sizeof(legacy->vendor), 1) ||
        !fixed_string_valid(legacy->renderer, sizeof(legacy->renderer), 1) ||
        !fixed_string_valid(legacy->version, sizeof(legacy->version), 1) ||
        !fixed_string_valid(legacy->shading_language_version,
                            sizeof(legacy->shading_language_version), 1) ||
        !fixed_string_valid(legacy->extensions,
                            sizeof(legacy->extensions), 0) ||
        nxgl_parse_gles_version(legacy->version, &parsed_major,
                                &parsed_minor) != NXGL_SUCCESS ||
        parsed_major != legacy->actual.gles_major ||
        parsed_minor != legacy->actual.gles_minor)
        return 0;

    if (handles->api_version != NXGL_API_VERSION_V2 ||
        handles->struct_size < sizeof(*handles) ||
        handles->owner != report->stack_owner ||
        handles->native_window == 0u ||
        handles->egl_display == 0u || handles->egl_context == 0u ||
        handles->egl_surface == 0u || handles->egl_config == 0u)
        return 0;
    if (report->stack_owner == NXGL_STACK_OWNER_V2_SDL_EGL) {
        if (!handles->sdl_window || !handles->sdl_context)
            return 0;
    } else if (handles->sdl_window || handles->sdl_context ||
               handles->native_display == 0u) {
        return 0;
    }

    if (egl->api_version != NXGL_API_VERSION_V2 ||
        egl->struct_size < sizeof(*egl) || egl->observed != 1 ||
        egl->display == 0u || egl->context == 0u || egl->surface == 0u ||
        egl->config == 0u || egl->config_id <= 0 ||
        egl->surface_width != legacy->drawable_width ||
        egl->surface_height != legacy->drawable_height ||
        egl->red_bits != legacy->actual.red_bits ||
        egl->green_bits != legacy->actual.green_bits ||
        egl->blue_bits != legacy->actual.blue_bits ||
        egl->alpha_bits != legacy->actual.alpha_bits ||
        egl->depth_bits != legacy->actual.depth_bits ||
        egl->stencil_bits != legacy->actual.stencil_bits ||
        handles->egl_display != egl->display ||
        handles->egl_context != egl->context ||
        handles->egl_surface != egl->surface ||
        handles->egl_config != egl->config ||
        !fixed_string_valid(egl->vendor, sizeof(egl->vendor), 1) ||
        !fixed_string_valid(egl->version, sizeof(egl->version), 1) ||
        !fixed_string_valid(egl->client_apis, sizeof(egl->client_apis), 1))
        return 0;
    return 1;
}

int ts_framework_publish_graphics(const nxgl_report_v2 *report)
{
    nxcompat_graphics_receipt receipt;
    nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
    const nxgl_report *legacy;
    const nxgl_egl_actual_v2 *egl;
    uint64_t generation;

    if (!framework.initialized || !framework.registry ||
        !graphics_report_valid(report) ||
        framework.graphics_generation == UINT64_MAX)
        return -1;
    legacy = &report->legacy;
    egl = &report->egl;
    generation = framework.graphics_generation + 1u;

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
    receipt.source = NXCOMPAT_SOURCE_NXGL;
    receipt.generation = generation;
    receipt.window_width = legacy->window_width;
    receipt.window_height = legacy->window_height;
    receipt.drawable_width = legacy->drawable_width;
    receipt.drawable_height = legacy->drawable_height;
    receipt.gles_major = legacy->actual.gles_major;
    receipt.gles_minor = legacy->actual.gles_minor;
    receipt.red_bits = legacy->actual.red_bits;
    receipt.green_bits = legacy->actual.green_bits;
    receipt.blue_bits = legacy->actual.blue_bits;
    receipt.alpha_bits = legacy->actual.alpha_bits;
    receipt.depth_bits = legacy->actual.depth_bits;
    receipt.stencil_bits = legacy->actual.stencil_bits;
    receipt.double_buffer = legacy->actual.double_buffer;
    receipt.profile_mask = legacy->actual.profile_mask;
    receipt.egl_config_id = egl->config_id;
    receipt.egl_red_bits = egl->red_bits;
    receipt.egl_green_bits = egl->green_bits;
    receipt.egl_blue_bits = egl->blue_bits;
    receipt.egl_alpha_bits = egl->alpha_bits;
    receipt.egl_depth_bits = egl->depth_bits;
    receipt.egl_stencil_bits = egl->stencil_bits;
    receipt.egl_renderable_type = egl->renderable_type;
    receipt.egl_surface_type = egl->surface_type;
    if (copy_fixed(receipt.video_backend, sizeof(receipt.video_backend),
                   legacy->video_backend, sizeof(legacy->video_backend), 1) ||
        copy_fixed(receipt.gl_vendor, sizeof(receipt.gl_vendor),
                   legacy->vendor, sizeof(legacy->vendor), 1) ||
        copy_fixed(receipt.gl_renderer, sizeof(receipt.gl_renderer),
                   legacy->renderer, sizeof(legacy->renderer), 1) ||
        copy_fixed(receipt.gl_version, sizeof(receipt.gl_version),
                   legacy->version, sizeof(legacy->version), 1) ||
        copy_fixed(receipt.glsl_version, sizeof(receipt.glsl_version),
                   legacy->shading_language_version,
                   sizeof(legacy->shading_language_version), 1) ||
        copy_fixed(receipt.gl_extensions, sizeof(receipt.gl_extensions),
                   legacy->extensions, sizeof(legacy->extensions), 0) ||
        copy_fixed(receipt.egl_vendor, sizeof(receipt.egl_vendor),
                   egl->vendor, sizeof(egl->vendor), 1) ||
        copy_fixed(receipt.egl_version, sizeof(receipt.egl_version),
                   egl->version, sizeof(egl->version), 1) ||
        copy_fixed(receipt.egl_client_apis, sizeof(receipt.egl_client_apis),
                   egl->client_apis, sizeof(egl->client_apis), 1))
        return -1;

    if (nxcompat_registry_publish_graphics_ex(framework.registry, &receipt,
                                              &reason) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[titansouls/framework] graphics receipt rejected: %s\n",
                nxcompat_reason_name(reason));
        return -1;
    }
    framework.graphics_generation = generation;
    framework.graphics_published = 1;
    (void)evaluate(NXCOMPAT_PHASE_GRAPHICS, 0);
    return maybe_validate_ready();
}

int ts_framework_publish_input(const nxinput_context *input)
{
    nxcompat_input_receipt receipt;

    memset(&receipt, 0, sizeof(receipt));
    if (!framework.initialized || !framework.registry || !input ||
        nxinput_nxcompat_publish_context(framework.registry, input,
                                         &receipt) != NXCOMPAT_OK)
        return -1;
    framework.input_published = 1;
    (void)evaluate(NXCOMPAT_PHASE_INPUT, 0);
    return maybe_validate_ready();
}

int ts_framework_publish_audio(SDL_AudioDeviceID device,
                               const SDL_AudioSpec *actual)
{
    nxaudio_backend_observation observation;
    nxaudio_reason audio_reason = NXAUDIO_REASON_NONE;
    nxcompat_reason_code compat_reason = NXCOMPAT_REASON_NONE;
    nxcompat_audio_receipt receipt;
    const char *backend = SDL_GetCurrentAudioDriver();
    uint64_t generation;

    if (!framework.initialized || !framework.registry ||
        !framework.audio_contract_validated || device == 0u || !actual ||
        !backend || !backend[0] ||
        framework.audio_generation == UINT64_MAX)
        return -1;
    memset(&observation, 0, sizeof(observation));
    observation.api_version = NXAUDIO_API_VERSION;
    observation.struct_size = sizeof(observation);
    {
        int written = snprintf(observation.backend,
                               sizeof(observation.backend), "%s", backend);
        if (written < 0 || (size_t)written >= sizeof(observation.backend))
            return -1;
    }
    observation.inherited_attempt =
        framework.host.inherited_audio_driver[0] != '\0';
    observation.server_reachable = 1;
    observation.device_opened = 1;
    if (nxaudio_classify_backend(&observation, &audio_reason) != NXAUDIO_OK) {
        fprintf(stderr, "[titansouls/framework] audio rejected: %s\n",
                nxaudio_reason_name(audio_reason));
        return -1;
    }

    generation = framework.audio_generation + 1u;
    memset(&receipt, 0, sizeof(receipt));
    receipt.api_version = NXCOMPAT_API_VERSION;
    receipt.struct_size = sizeof(receipt);
    receipt.proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                          NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                          NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
    receipt.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
    receipt.generation = generation;
    receipt.lifetime = NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED;
    receipt.frequency = actual->freq;
    receipt.format = actual->format;
    receipt.channels = actual->channels;
    receipt.samples = actual->samples;
    receipt.device_id_was_nonzero = 1;
    {
        int written = snprintf(receipt.backend, sizeof(receipt.backend),
                               "%s", backend);
        if (written < 0 || (size_t)written >= sizeof(receipt.backend))
            return -1;
    }
    if (nxcompat_registry_publish_audio_ex(framework.registry, &receipt,
                                           &compat_reason) != NXCOMPAT_OK) {
        fprintf(stderr,
                "[titansouls/framework] audio receipt rejected: %s\n",
                nxcompat_reason_name(compat_reason));
        return -1;
    }
    framework.audio_generation = generation;
    framework.audio_published = 1;
    (void)evaluate(NXCOMPAT_PHASE_AUDIO, 0);
    return maybe_validate_ready();
}

int ts_framework_ready(void)
{
    int result;

    if (!framework.graphics_published || !framework.input_published ||
        !framework.audio_published)
        return -1;
    if (framework.ready_validated)
        return 0;
    result = evaluate(NXCOMPAT_PHASE_READY, 1);
    if (result == 0)
        framework.ready_validated = 1;
    return result;
}
