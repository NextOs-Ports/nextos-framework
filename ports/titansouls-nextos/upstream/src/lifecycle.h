/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TITANSOULS_LIFECYCLE_H
#define TITANSOULS_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_LIFECYCLE_API_VERSION 1u

typedef enum ts_lifecycle_module {
    TS_LIFECYCLE_MODULE_FMODEX = 0,
    TS_LIFECYCLE_MODULE_GAME = 1
} ts_lifecycle_module;

typedef struct ts_lifecycle_runtime ts_lifecycle_runtime;

typedef struct ts_lifecycle_ops {
    uint32_t api_version;
    size_t struct_size;

    /* Verify that nxloader has already completed this module's relocation,
     * finalization and init_array.  This callback must not initialize it a
     * second time. */
    int (*verify_module_initialized)(void *userdata,
                                     ts_lifecycle_module module);

    /* Create the adapter-owned NativeActivity/android_app objects after both
     * guest modules are initialized. */
    int (*create_activity)(void *userdata);

    /* Called from ts_lifecycle_graphics_ready() at the real guest-triggered
     * EGL current point.  It verifies that the strong nxgl-v2 receipt was
     * published; it must not fabricate a Surface or GL event. */
    int (*verify_graphics_ready)(void *userdata);

    /* Synchronous delegated owner.  It must reproduce the native command
     * sequence and invoke android_main directly on the calling thread.  When
     * the guest makes its real EGL context current, this callback must call
     * ts_lifecycle_graphics_ready(runtime) before returning.  The runtime
     * handle is borrowed and becomes invalid when this callback returns. */
    int (*run_android_main)(void *userdata, ts_lifecycle_runtime *runtime);

    void *userdata;
} ts_lifecycle_ops;

/* Report the real graphics-ready boundary from inside run_android_main().
 * Exactly one successful report is required. */
int ts_lifecycle_graphics_ready(ts_lifecycle_runtime *runtime);

/* Validate and synchronously execute the Titan Souls nxandroid profile.
 * Returns zero only after android_main has returned normally and the context
 * has been destroyed without rollback/reentrancy errors. */
int ts_lifecycle_run(const ts_lifecycle_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* TITANSOULS_LIFECYCLE_H */
