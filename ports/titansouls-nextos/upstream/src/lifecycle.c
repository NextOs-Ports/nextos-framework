/* SPDX-License-Identifier: GPL-3.0-only */
#include "lifecycle.h"

#include <stdio.h>
#include <string.h>

#include "nxandroid.h"

#define TS_ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

enum ts_lifecycle_internal_status {
    TS_LIFECYCLE_CALLBACK_INVALID_STEP = -1001,
    TS_LIFECYCLE_CALLBACK_GRAPHICS_MISSING = -1002,
    TS_LIFECYCLE_CALLBACK_GRAPHICS_DUPLICATE = -1003,
    TS_LIFECYCLE_CALLBACK_GRAPHICS_OUTSIDE_RUNTIME = -1004
};

struct ts_lifecycle_runtime {
    const ts_lifecycle_ops *ops;
    int delegated_active;
    int graphics_reported;
    int graphics_verified;
    int graphics_status;
};

typedef struct ts_lifecycle_adapter {
    const ts_lifecycle_ops *ops;
    ts_lifecycle_runtime runtime;
} ts_lifecycle_adapter;

static const nxandroid_module_spec ts_modules[] = {
    {"libfmodex.so", NXANDROID_JNI_NONE},
    {"libTestSuite.so", NXANDROID_JNI_NONE},
};

#define TS_STEP(step_phase, step_module, step_contract)                     \
    {                                                                       \
        (step_phase), (step_module), 0u, (step_contract), NULL,             \
            NXANDROID_TERMINAL_NONE, 0u, 0u                                \
    }

static const nxandroid_step ts_steps[] = {
    TS_STEP(NXANDROID_PHASE_MODULE_INITIALIZED,
            TS_LIFECYCLE_MODULE_FMODEX,
            "titansouls-fmodex-init-array-complete-v1"),
    TS_STEP(NXANDROID_PHASE_MODULE_INITIALIZED,
            TS_LIFECYCLE_MODULE_GAME,
            "titansouls-game-init-array-complete-v1"),
    TS_STEP(NXANDROID_PHASE_ACTIVITY_CREATE,
            NXANDROID_NO_MODULE,
            "titansouls-nativeactivity-created-v1"),
    /* nxandroid's delegated-runtime contract intentionally owns its hidden
     * graphics, input, loop and terminal phases.  Declaring GRAPHICS_REQUEST
     * or GL_READY before this step would be an invalid profile.  The adapter
     * instead proves the real guest-triggered GL boundary through the narrow
     * ts_lifecycle_graphics_ready() callback while android_main is running. */
    TS_STEP(NXANDROID_PHASE_RUNTIME_DELEGATED,
            NXANDROID_NO_MODULE,
            "titansouls-android-main-delegated-runtime-v1"),
};

#undef TS_STEP

static const nxandroid_profile ts_profile = {
    NXANDROID_API_VERSION,
    sizeof(nxandroid_profile),
    ts_modules,
    TS_ARRAY_SIZE(ts_modules),
    ts_steps,
    TS_ARRAY_SIZE(ts_steps),
    NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME,
};

static int lifecycle_ops_valid(const ts_lifecycle_ops *ops)
{
    return ops && ops->api_version == TS_LIFECYCLE_API_VERSION &&
           ops->struct_size >= sizeof(*ops) &&
           ops->verify_module_initialized && ops->create_activity &&
           ops->verify_graphics_ready && ops->run_android_main;
}

int ts_lifecycle_graphics_ready(ts_lifecycle_runtime *runtime)
{
    int status;

    if (!runtime || !runtime->ops || !runtime->delegated_active)
        return TS_LIFECYCLE_CALLBACK_GRAPHICS_OUTSIDE_RUNTIME;
    if (runtime->graphics_reported)
        return TS_LIFECYCLE_CALLBACK_GRAPHICS_DUPLICATE;

    runtime->graphics_reported = 1;
    status = runtime->ops->verify_graphics_ready(runtime->ops->userdata);
    runtime->graphics_status = status;
    if (status != 0)
        return status;
    runtime->graphics_verified = 1;
    return 0;
}

static int lifecycle_invoke(void *userdata, const nxandroid_step *step)
{
    ts_lifecycle_adapter *adapter = (ts_lifecycle_adapter *)userdata;
    int status;

    if (!adapter || !adapter->ops || !step)
        return TS_LIFECYCLE_CALLBACK_INVALID_STEP;
    switch (step->phase) {
    case NXANDROID_PHASE_MODULE_INITIALIZED:
        if (step->module_index > TS_LIFECYCLE_MODULE_GAME)
            return TS_LIFECYCLE_CALLBACK_INVALID_STEP;
        return adapter->ops->verify_module_initialized(
            adapter->ops->userdata,
            (ts_lifecycle_module)step->module_index);

    case NXANDROID_PHASE_ACTIVITY_CREATE:
        return adapter->ops->create_activity(adapter->ops->userdata);

    case NXANDROID_PHASE_RUNTIME_DELEGATED:
        memset(&adapter->runtime, 0, sizeof(adapter->runtime));
        adapter->runtime.ops = adapter->ops;
        adapter->runtime.delegated_active = 1;
        status = adapter->ops->run_android_main(adapter->ops->userdata,
                                                &adapter->runtime);
        adapter->runtime.delegated_active = 0;
        if (status != 0)
            return status;
        if (!adapter->runtime.graphics_reported ||
            !adapter->runtime.graphics_verified)
            return adapter->runtime.graphics_status != 0
                       ? adapter->runtime.graphics_status
                       : TS_LIFECYCLE_CALLBACK_GRAPHICS_MISSING;
        return 0;

    default:
        return TS_LIFECYCLE_CALLBACK_INVALID_STEP;
    }
}

int ts_lifecycle_run(const ts_lifecycle_ops *ops)
{
    ts_lifecycle_adapter adapter;
    nxandroid_ops android_ops;
    nxandroid_context *context = NULL;
    nxandroid_result result;
    nxandroid_result destroy_result;
    size_t bad_step = NXANDROID_NO_MODULE;
    int adapter_status = 0;

    if (!lifecycle_ops_valid(ops))
        return -1;
    result = nxandroid_profile_validate(&ts_profile, &bad_step);
    if (result != NXANDROID_OK) {
        fprintf(stderr,
                "[titansouls/lifecycle] invalid profile step=%zu: %s\n",
                bad_step, nxandroid_result_string(result));
        return -1;
    }

    memset(&adapter, 0, sizeof(adapter));
    adapter.ops = ops;
    memset(&android_ops, 0, sizeof(android_ops));
    android_ops.api_version = NXANDROID_API_VERSION;
    android_ops.struct_size = sizeof(android_ops);
    android_ops.invoke = lifecycle_invoke;
    android_ops.rollback = NULL;
    android_ops.userdata = &adapter;
    result = nxandroid_context_create(&ts_profile, &android_ops, &context);
    if (result != NXANDROID_OK) {
        fprintf(stderr, "[titansouls/lifecycle] context create failed: %s\n",
                nxandroid_result_string(result));
        return -1;
    }

    result = nxandroid_context_run(context);
    if (result != NXANDROID_OK)
        adapter_status = nxandroid_context_get_adapter_status(context);
    destroy_result = nxandroid_context_destroy(&context);
    if (result != NXANDROID_OK || destroy_result != NXANDROID_OK) {
        fprintf(stderr,
                "[titansouls/lifecycle] run=%s adapter=%d destroy=%s\n",
                nxandroid_result_string(result), adapter_status,
                nxandroid_result_string(destroy_result));
        return -1;
    }
    return 0;
}
