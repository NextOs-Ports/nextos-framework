/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "fp2_graphics_contract.h"

#include "nxgl_graphics_contract.h"
#include "nxgl_graphics_contract_adapter.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

extern void *fp2_gl_raw_sym(const char *name);

static void *fp2_contract_window;
static nxgl_graphics_contract fp2_contract;
static nxgl_graphics_present_gate fp2_gate;
static nxgl_graphics_gate_result fp2_result;
static uintptr_t fp2_context_token;
static int fp2_contract_ready;

static void *fp2_contract_current_window(void)
{
    return fp2_contract_window;
}

/* GL must come from the exact SDL/raw provider already selected by the port,
 * not from one of the GLES3 facade exports in the executable.  Everything
 * else remains the system SDL/EGL symbol that owns the live context. */
static void *fp2_contract_resolve(void *userdata, const char *name)
{
    (void)userdata;
    if (!name)
        return NULL;
    if (strcmp(name, "SDL_GL_GetCurrentWindow") == 0)
        return (void *)fp2_contract_current_window;

    void *found = fp2_gl_raw_sym(name);
    if (found)
        return found;
    return dlsym(RTLD_DEFAULT, name);
}

void fp2_graphics_contract_prepare(void)
{
    if (nxgl_graphics_contract_default(&fp2_contract) != 0)
        return;
    fp2_contract.api = NXGL_GRAPHICS_API_GLES;
    fp2_contract.profile = NXGL_GRAPHICS_PROFILE_ES;
    fp2_contract.version_major = 2;
    fp2_contract.version_minor = 0;
    fp2_contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    fp2_contract.version_max_major = 0;
    fp2_contract.version_max_minor = 0;
    fp2_contract.shader_dialect = NXGL_SHADER_DIALECT_ESSL100;
    fp2_contract.drawable_ready_timeout_ms = 5000;
    if (nxgl_graphics_present_gate_init(&fp2_gate) != 0 ||
        nxgl_graphics_gate_result_init(&fp2_result) != 0)
        return;
    nxgl_graphics_contract_adapter_set_resolver_ex(
        fp2_contract_resolve, NULL);
    fp2_contract_ready = 1;
}

void fp2_graphics_contract_set_sdl_window(void *window)
{
    fp2_contract_window = window;
}

int fp2_graphics_contract_pre_present(uintptr_t context_token)
{
    if (!fp2_contract_ready || !fp2_contract_window || !context_token)
        return 0;

    if (fp2_gate.status == NXGL_GRAPHICS_GATE_PROVED)
        return 1;
    if (fp2_gate.status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
        fp2_context_token == context_token)
        return 1;

    /* Unity may make a shared window context current and replace it before
     * its first present.  No health exists yet, so reset and prove the actual
     * context that will own the page flip. */
    if (fp2_gate.status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT)
        nxgl_graphics_present_gate_reset(&fp2_gate);
    fp2_context_token = context_token;

    nxgl_graphics_gate_status status =
        nxgl_graphics_contract_adapter_pre_present(
            &fp2_contract, (uintptr_t)fp2_contract_window, context_token,
            &fp2_gate, &fp2_result);
    if (status != NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT) {
        fprintf(stderr,
                "[fp2/graphics] pre-present contract rejected: %s\n",
                nxgl_graphics_reason_name(fp2_result.reason));
        return 0;
    }

    char diagnostic[512];
    if (nxgl_graphics_present_gate_prepresent_line(
            &fp2_gate, diagnostic, sizeof diagnostic))
        fprintf(stderr, "%s\n", diagnostic);
    return 1;
}

int fp2_graphics_contract_after_present(uintptr_t context_token)
{
    char receipt[4096];
    char json[8192];
    if (!fp2_contract_ready || !fp2_contract_window || !context_token)
        return 0;

    nxgl_graphics_gate_status status =
        nxgl_graphics_contract_adapter_after_present(
            &fp2_contract, (uintptr_t)fp2_contract_window,
            context_token, &fp2_gate, &fp2_result,
            receipt, sizeof receipt);
    if (receipt[0]) {
        fprintf(stderr, "%s\n", receipt);
        if (nxgl_graphics_contract_evidence_json(
                &fp2_contract, &fp2_result.evidence,
                json, sizeof json))
            fprintf(stderr, "[fp2/graphics] evidence-json=%s\n", json);
    }
    if (status == NXGL_GRAPHICS_GATE_REJECTED) {
        fprintf(stderr,
                "[fp2/graphics] post-present contract rejected: %s\n",
                nxgl_graphics_reason_name(fp2_result.reason));
        return 0;
    }
    return 1;
}
