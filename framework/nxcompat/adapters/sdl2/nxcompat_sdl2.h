/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXCOMPAT_SDL2_H
#define NXCOMPAT_SDL2_H

#include "nxcompat.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nxcompat_sdl2_flag {
  /* Useful for diagnostic/headless tools, never for a public game default. */
  NXCOMPAT_SDL2_ALLOW_NONDISPLAY = 1u << 0
};

typedef struct nxcompat_sdl2_options {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_backend_kind kind;
  /* Optional non-audio/non-video SDL subsystems needed by the probe. */
  uint32_t additional_sdl_init_flags;
  uint32_t nxcompat_flags;
  nxcompat_status_callback status;
  void *status_userdata;
} nxcompat_sdl2_options;

/* Transient startup probe: it releases every SDL reference it acquired before
 * returning, never releases a subsystem owned by the caller, and restores the
 * complete driver-hint snapshot byte-for-byte.  The selected backend is
 * evidence in the result/receipt, not a persistent environment mutation. Call
 * before engine threads begin using SDL. Audio failure is returned so the
 * loader can continue muted.
 * Audio validates a real default output and rejects dummy/disk. There is no
 * skip-probe mode and no driver enumeration loop: the inherited attempt may be
 * followed by exactly one normal SDL autodetect attempt. Retry may clear stale
 * SDL driver hints, but deliberately preserves PULSE_SERVER and the session
 * socket selected by nxcompat. For video, prefer nxgl as the sole owner because
 * only a real EGL/GLES drawable proves success. */
int nxcompat_sdl2_negotiate(const nxcompat_sdl2_options *options,
                            nxcompat_backend_result *result);
nxcompat_result_code
nxcompat_sdl2_negotiate_v2(const nxcompat_sdl2_options *options,
                           nxcompat_backend_result_v2 *result);
/* Audio-only evidence path.  A publishable receipt is produced only after a
 * nonzero device ID, a valid obtained spec, one close, and verified cleanup.
 * On every failure receipt->generation remains zero. */
nxcompat_result_code nxcompat_sdl2_negotiate_audio_v2(
    const nxcompat_sdl2_options *options, uint64_t generation,
    nxcompat_backend_result_v2 *result, nxcompat_audio_receipt *receipt);

#ifdef __cplusplus
}
#endif

#endif
