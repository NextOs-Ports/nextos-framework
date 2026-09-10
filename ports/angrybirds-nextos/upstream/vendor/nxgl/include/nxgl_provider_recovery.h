/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_PROVIDER_RECOVERY_H
#define NXGL_PROVIDER_RECOVERY_H

#include "nxgl.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This is an optional adapter helper. Linking or initializing it never enables
 * provider replacement: callers must set enabled=1 after a measured failure. */
#define NXGL_PROVIDER_RECOVERY_PATH_MAX 4096u
#define NXGL_PROVIDER_RECOVERY_SYMBOL_MAX 64u
#define NXGL_PROVIDER_RECOVERY_ARG_MAX 128u
#define NXGL_PROVIDER_RECOVERY_MARKER \
  "NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED"

typedef enum nxgl_provider_probe_mode_v2 {
  NXGL_PROVIDER_PROBE_V2_NONE = 0,
  /* Resolve EGL and every adapter-declared GLES entry point from one DSO. */
  NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY = 1,
  /* Additionally prove eglInitialize(EGL_DEFAULT_DISPLAY) on the live host. */
  NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY = 2
} nxgl_provider_probe_mode_v2;

typedef enum nxgl_provider_recovery_reason_v2 {
  NXGL_PROVIDER_RECOVERY_V2_NONE = 0,
  NXGL_PROVIDER_RECOVERY_V2_DISABLED,
  NXGL_PROVIDER_RECOVERY_V2_CANDIDATE_UNAVAILABLE,
  NXGL_PROVIDER_RECOVERY_V2_PROVIDER_ALREADY_LOADED,
  NXGL_PROVIDER_RECOVERY_V2_TRANSPORT_MISMATCH,
  NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING,
  NXGL_PROVIDER_RECOVERY_V2_ENGINE_SYMBOLS_MISSING,
  NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS,
  NXGL_PROVIDER_RECOVERY_V2_EGL_INITIALIZE_FAILED,
  /* eglTerminate failed, or this process was permanently poisoned by an
   * earlier terminate failure. No later otherwise-valid enabled probe or
   * otherwise-valid authorized re-exec may proceed. */
  NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED,
  NXGL_PROVIDER_RECOVERY_V2_NOT_AUTHORIZED,
  NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN,
  NXGL_PROVIDER_RECOVERY_V2_ALREADY_APPLIED,
  NXGL_PROVIDER_RECOVERY_V2_INHERITED_PROVIDER_OVERRIDE,
  NXGL_PROVIDER_RECOVERY_V2_PROVIDER_CHANGED,
  NXGL_PROVIDER_RECOVERY_V2_ENVIRONMENT_FAILED,
  NXGL_PROVIDER_RECOVERY_V2_EXEC_FAILED
} nxgl_provider_recovery_reason_v2;

typedef struct nxgl_sdl_provider_probe_options_v2 {
  uint32_t api_version;
  size_t struct_size;
  int enabled;
  nxgl_provider_probe_mode_v2 mode;
  int reject_if_already_loaded;
  /* Required only for EGL_DEFAULT_DISPLAY. The helper must never initialize
   * and terminate a display while an inherited SDL/EGL stack may still own
   * that display. SYMBOLS_ONLY performs no EGL lifecycle call. */
  int video_torn_down;
  const char *video_backend;
  /* An absolute path or loader-resolvable SONAME. The receipt always contains
   * the canonical DSO that actually owns the resolved EGL/GLES symbols.
   * Receipt identity detects accidental operational replacement only; it is
   * not adversarial TOCTOU protection. Through probe -> re-exec, the caller
   * must keep this object in a local tree not writable by untrusted actors and
   * coordinate same-process writers/threads. */
  const char *provider;
  const char *const *required_engine_gles_symbols;
  size_t required_engine_gles_symbol_count;
} nxgl_sdl_provider_probe_options_v2;

typedef struct nxgl_sdl_provider_probe_receipt_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_provider_recovery_reason_v2 reason;
  nxgl_provider_probe_mode_v2 mode;
  int usable;
  int exports_egl;
  int exports_engine_gles;
  int egl_initialized;
  int egl_major;
  int egl_minor;
  uint64_t device;
  uint64_t inode;
  uint64_t size;
  int64_t mtime_seconds;
  int64_t mtime_nanoseconds;
  char provider_path[NXGL_PROVIDER_RECOVERY_PATH_MAX];
} nxgl_sdl_provider_probe_receipt_v2;

typedef struct nxgl_sdl_provider_reexec_options_v2 {
  uint32_t api_version;
  size_t struct_size;
  int enabled;
  /* Must be the positive result of nxgl_plan_sdl_provider_pair() or
   * nxgl_plan_sdl_precontext_recovery_v2(). */
  nxgl_sdl_provider_pair_plan authorization;
  const nxgl_sdl_provider_probe_receipt_v2 *provider;
  /* The adapter owns lifecycle teardown. Re-exec is refused unless it attests
   * that the failed SDL/window/context stack has already been fully closed. */
  int video_torn_down;
  char *const *argv;
} nxgl_sdl_provider_reexec_options_v2;

typedef struct nxgl_sdl_provider_reexec_result_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_provider_recovery_reason_v2 reason;
  int environment_restored;
  int system_error;
} nxgl_sdl_provider_reexec_result_v2;

void nxgl_sdl_provider_probe_options_v2_init(
    nxgl_sdl_provider_probe_options_v2 *options);
void nxgl_sdl_provider_probe_receipt_v2_init(
    nxgl_sdl_provider_probe_receipt_v2 *receipt);
void nxgl_sdl_provider_reexec_options_v2_init(
    nxgl_sdl_provider_reexec_options_v2 *options);
void nxgl_sdl_provider_reexec_result_v2_init(
    nxgl_sdl_provider_reexec_result_v2 *result);

/* Probe one adapter-selected candidate. This function never scans firmware
 * directories, chooses a provider from a device/CFW name, mutates the
 * environment or retries video. Invalid/BUSY calls leave receipt untouched.
 * A failed live eglTerminate permanently rejects every later otherwise-valid
 * enabled probe and authorized re-exec with EGL_TERMINATE_FAILED. */
int nxgl_probe_sdl_provider_v2(
    const nxgl_sdl_provider_probe_options_v2 *options,
    nxgl_sdl_provider_probe_receipt_v2 *receipt);

/* Bind SDL's EGL and GL variables to the same canonical, still-identical DSO
 * and re-exec /proc/self/exe once. This function never changes LD_PRELOAD.
 * It does not return after a successful exec. If exec fails, every variable
 * introduced by the helper is removed before the error is returned.
 * Invalid/BUSY calls leave result and the environment untouched. */
int nxgl_reexec_sdl_provider_pair_v2(
    const nxgl_sdl_provider_reexec_options_v2 *options,
    nxgl_sdl_provider_reexec_result_v2 *result);

const char *nxgl_provider_recovery_reason_name_v2(
    nxgl_provider_recovery_reason_v2 reason);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_PROVIDER_RECOVERY_H */
