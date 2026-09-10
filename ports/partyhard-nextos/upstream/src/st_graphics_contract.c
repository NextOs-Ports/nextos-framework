/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "st_graphics_contract.h"

#include "nxgl_graphics_contract.h"
#include "nxgl_graphics_contract_adapter.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

extern void *st_gl_raw_sym(const char *name);

static void *st_contract_window;
static nxgl_graphics_contract st_contract;
static nxgl_graphics_present_gate st_gate;
static nxgl_graphics_gate_result st_result;
static uintptr_t st_context_token;
static int st_contract_ready;

static void *st_contract_current_window(void)
{
    return st_contract_window;
}

/* GL must come from the exact SDL/raw provider already selected by the port,
 * not from one of the GLES3 facade exports in the executable.  Everything
 * else remains the system SDL/EGL symbol that owns the live context. */
static void *st_contract_resolve(void *userdata, const char *name)
{
    (void)userdata;
    if (!name)
        return NULL;
    if (strcmp(name, "SDL_GL_GetCurrentWindow") == 0)
        return (void *)st_contract_current_window;

    void *found = st_gl_raw_sym(name);
    if (found)
        return found;
    return dlsym(RTLD_DEFAULT, name);
}

void st_graphics_contract_prepare(void)
{
    if (nxgl_graphics_contract_default(&st_contract) != 0)
        return;
    st_contract.api = NXGL_GRAPHICS_API_GLES;
    st_contract.profile = NXGL_GRAPHICS_PROFILE_ES;
    st_contract.version_major = 2;
    st_contract.version_minor = 0;
    st_contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    st_contract.version_max_major = 0;
    st_contract.version_max_minor = 0;
    st_contract.shader_dialect = NXGL_SHADER_DIALECT_ESSL100;
    st_contract.drawable_ready_timeout_ms = 5000;
    if (nxgl_graphics_present_gate_init(&st_gate) != 0 ||
        nxgl_graphics_gate_result_init(&st_result) != 0)
        return;
    nxgl_graphics_contract_adapter_set_resolver_ex(
        st_contract_resolve, NULL);
    st_contract_ready = 1;
}

void st_graphics_contract_set_sdl_window(void *window)
{
    st_contract_window = window;
}

int st_graphics_contract_pre_present(uintptr_t context_token)
{
    if (!st_contract_ready || !st_contract_window || !context_token)
        return 0;

    if (st_gate.status == NXGL_GRAPHICS_GATE_PROVED)
        return 1;
    if (st_gate.status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
        st_context_token == context_token)
        return 1;

    /* Unity may make a shared window context current and replace it before
     * its first present.  No health exists yet, so reset and prove the actual
     * context that will own the page flip. */
    if (st_gate.status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT)
        nxgl_graphics_present_gate_reset(&st_gate);
    st_context_token = context_token;

    nxgl_graphics_gate_status status =
        nxgl_graphics_contract_adapter_pre_present(
            &st_contract, (uintptr_t)st_contract_window, context_token,
            &st_gate, &st_result);
    if (status != NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT) {
        fprintf(stderr,
                "[st/graphics] pre-present contract rejected: %s\n",
                nxgl_graphics_reason_name(st_result.reason));
        return 0;
    }

    char diagnostic[512];
    if (nxgl_graphics_present_gate_prepresent_line(
            &st_gate, diagnostic, sizeof diagnostic))
        fprintf(stderr, "%s\n", diagnostic);
    return 1;
}

int st_graphics_contract_after_present(uintptr_t context_token)
{
    char receipt[4096];
    char json[8192];
    if (!st_contract_ready || !st_contract_window || !context_token)
        return 0;

    nxgl_graphics_gate_status status =
        nxgl_graphics_contract_adapter_after_present(
            &st_contract, (uintptr_t)st_contract_window,
            context_token, &st_gate, &st_result,
            receipt, sizeof receipt);
    if (receipt[0]) {
        fprintf(stderr, "%s\n", receipt);
        if (nxgl_graphics_contract_evidence_json(
                &st_contract, &st_result.evidence,
                json, sizeof json))
            fprintf(stderr, "[st/graphics] evidence-json=%s\n", json);
    }
    if (status == NXGL_GRAPHICS_GATE_REJECTED) {
        fprintf(stderr,
                "[st/graphics] post-present contract rejected: %s\n",
                nxgl_graphics_reason_name(st_result.reason));
        return 0;
    }
    return 1;
}
