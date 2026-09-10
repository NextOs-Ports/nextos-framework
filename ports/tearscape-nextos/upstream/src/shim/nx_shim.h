/* NextOS GLES3->GLES2 shim for Godot 4 on Mali-450 — common decls. */
#ifndef NX_SHIM_H
#define NX_SHIM_H

#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *nx_mali_sym(const char *name);      /* dlsym on the real Mali GLESv2 */
void *nx_mali_egl_sym(const char *name);  /* dlsym on the real Mali EGL    */
void nx_log(const char *fmt, ...);

/* Per-frame draw telemetry shared across the GLES and EGL shims.  This is
 * deliberately diagnostic-only: collecting it must not alter GL state. */
typedef struct {
    unsigned long physical_draws;
    unsigned long api_draws;
    unsigned long instanced_calls;
    unsigned long instanced_instances;
    unsigned long instanced_vertices;
    unsigned long instanced_max;
    unsigned long instanced_arrays;
    unsigned long instanced_elements;
    unsigned long batched_calls;
    unsigned long batched_vertices;
    unsigned long batch_fallbacks;
    unsigned long instanced_hist[8];
} NxDrawStats;

#define NX_STUB_LOG(name) do { \
    static int nx_once; \
    if (!nx_once) { nx_once = 1; nx_log("STUB %s", name); } \
} while (0)

#endif
