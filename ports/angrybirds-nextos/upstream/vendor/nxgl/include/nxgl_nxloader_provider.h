/* SPDX-License-Identifier: GPL-3.0-only */
/* Explicit, process-lifetime nxgl graphics provider for nxloader 0.7.2. */
#ifndef NXGL_NXLOADER_PROVIDER_H
#define NXGL_NXLOADER_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "nxgl_graphics_contract.h"
#include "nxloader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_NXLOADER_PROVIDER_API_VERSION 2u
#define NXGL_NXLOADER_EVIDENCE_BINDING_API_VERSION 1u

typedef enum nxgl_nxloader_backend {
  NXGL_NXLOADER_BACKEND_EGL = 1,
  NXGL_NXLOADER_BACKEND_SDL2 = 2,
  NXGL_NXLOADER_BACKEND_SDL3 = 3
} nxgl_nxloader_backend;

enum nxgl_nxloader_provider_flags {
  /* With no flags, install is a no-op and the registry stays byte-for-byte
   * unchanged. This is the default-off adoption contract. */
  NXGL_NXLOADER_PROVIDER_GRAPHICS_EVIDENCE = 1u << 0
};

typedef enum nxgl_nxloader_provider_status {
  NXGL_NXLOADER_PROVIDER_DISABLED = 0,
  NXGL_NXLOADER_PROVIDER_ARMED,
  NXGL_NXLOADER_PROVIDER_PROBING,
  NXGL_NXLOADER_PROVIDER_PASS,
  NXGL_NXLOADER_PROVIDER_FAIL
} nxgl_nxloader_provider_status;

/* Called synchronously once with the exact JSON receipt. It must not call GL.
 * Returning nonzero makes the barrier fail closed. */
typedef int (*nxgl_nxloader_receipt_sink_fn)(void *userdata,
                                             const char *json,
                                             size_t json_size);

/* Optional notification after a failed barrier. Returning cannot reopen it;
 * every intercepted glCreateShader remains blocked. */
typedef void (*nxgl_nxloader_failure_fn)(void *userdata,
                                         nxgl_graphics_reason reason);

/* Release identity supplied by the port adapter, not inferred from ambient
 * process variables.  framework_commit, cfw, device, port_version and
 * artifact_sha256 are mandatory. artifact_sha256 identifies the exact game
 * ELF selected for this run; generation identifies the complete immutable
 * runtime generation separately.
 *
 * run_id, generation and port_id may be left empty.  In that case the provider
 * consumes only nxbootstrap 0.6.32's run-bound NXBOOTSTRAP_HEALTH_* exports.
 * If an explicit value and the corresponding bootstrap value both exist they
 * must match exactly.  No other NX_* environment variable is a fallback. */
typedef struct nxgl_nxloader_evidence_binding {
  size_t struct_size;
  uint32_t api_version;
  char run_id[96];
  char generation[72];
  char port_id[64];
  char framework_commit[64];
  char cfw[64];
  char device[64];
  char port_version[32];
  char artifact_sha256[72];
} nxgl_nxloader_evidence_binding;

typedef struct nxgl_nxloader_provider_config {
  size_t struct_size;
  uint32_t api_version;
  uint32_t flags;
  nxgl_nxloader_backend backend;
  nxgl_graphics_contract contract;
  nxgl_nxloader_evidence_binding binding;
  nxgl_nxloader_receipt_sink_fn receipt_sink;
  nxgl_nxloader_failure_fn failure;
  void *userdata;
} nxgl_nxloader_provider_config;

/* Install after the port's ordinary EGL/SDL/GLES provider and before the first
 * nxloader_module_resolve(). The function captures the originals, then adds a
 * higher-priority provider for exactly MakeCurrent, GetProcAddress and
 * glCreateShader. Installation is process-global and may happen once. It does
 * not alter nxloader 0.7.2 or its nxloader_config ABI.
 *
 * Returns NXLOADER_OK, or an nxloader error. flags==0 is a successful no-op. */
nxloader_result nxgl_nxloader_provider_install(
    nxloader_registry *registry,
    const nxgl_nxloader_provider_config *config);

/* Snapshot the one-shot barrier. `evidence` and `json` are optional. JSON is
 * empty until a probe finishes. Returns the current state. */
nxgl_nxloader_provider_status nxgl_nxloader_provider_get_status(
    nxgl_graphics_evidence *evidence, char *json, size_t json_cap);

/* Explicit final lifecycle acknowledgement for nxbootstrap 0.6.32. This is
 * NEVER called by MakeCurrent or by the shader barrier: the port adapter calls
 * it only after its own lifecycle/main-loop readiness boundary. It requires a
 * completed PASS evidence barrier, validates the exported
 * NXBOOTSTRAP_HEALTH_* tuple, and publishes the one-line receipt with a 0600
 * exclusive temporary plus no-replace rename inside the validated private
 * runtime directory. Symlinks, replay-shaped fields and a second call fail.
 * Returns NXLOADER_OK, EINVAL, ESTATE, EIO or EUNSUPPORTED. */
nxloader_result nxgl_nxloader_provider_mark_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_NXLOADER_PROVIDER_H */
