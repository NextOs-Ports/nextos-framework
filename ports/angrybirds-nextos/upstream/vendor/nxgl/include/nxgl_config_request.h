/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_config_request -- adapter-declared EGLConfig requirements (V3).
 *
 * WHY THIS EXISTS, AND WHY IT IS NEVER GLOBAL
 * -------------------------------------------
 * One port (the Huntdown adapter, field case 25/08/2026) needs an RGBA8888
 * EGLConfig: with an RGB565/no-alpha config its final blit into the default
 * framebuffer returns GL_INVALID_FRAMEBUFFER_OPERATION (0x506) and the panel
 * stays black with sound. That requirement is a fact about THAT engine, not
 * about any device, firmware or GPU.
 *
 * RULE: an EGLConfig requirement is declared by the adapter that measured the
 * need for it, and by that adapter ONLY. RGBA8888 -- or any other attribute
 * set -- must NEVER become a global nxgl default: a forced RGBA8888 default
 * would silently break every port that today runs happily on the config its
 * firmware hands out (and on some panels a config the compositor cannot scan
 * out). nxgl_config_request_default() therefore returns "don't care" for
 * every attribute, and a static test in tests/run-v3-graphics-host.sh pins
 * that forever.
 *
 * Nothing here touches EGL. The matcher and the formatter are pure: the
 * adapter fills in what it requires, the caller fills in what eglGetConfigAttrib
 * actually observed, and the verdict plus the one-line "EGLCONFIG:" receipt
 * fall out. Selection stays with the adapter; nxgl only judges and reports.
 */
#ifndef NXGL_CONFIG_REQUEST_H
#define NXGL_CONFIG_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_CONFIG_REQUEST_API_VERSION 1u

/* "Don't care" marker for every attribute below. */
#define NXGL_CONFIG_DONT_CARE (-1)

/* What one adapter requires of the EGLConfig it is about to accept.
 * Every size field follows the EGL "at least" convention: a request of 8
 * is satisfied by an observed 8 or more; NXGL_CONFIG_DONT_CARE accepts
 * anything, including 0. */
typedef struct nxgl_config_request {
  uint32_t api_version; /* NXGL_CONFIG_REQUEST_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_config_request) */
  int red;
  int green;
  int blue;
  int alpha;
  int depth;
  int stencil;
  int samples;
  /* 1 = the config must carry a nonzero EGL_NATIVE_VISUAL_ID; 0 = don't
   * care. Nonzero visual matters when the frame must be scanned out by the
   * native window system rather than blitted by the port itself. */
  int require_native_visual;
} nxgl_config_request;

/* What was actually measured on the chosen EGLConfig, straight from
 * eglGetConfigAttrib. Negative values mean "could not be observed". */
typedef struct nxgl_config_observed {
  uint32_t api_version; /* NXGL_CONFIG_REQUEST_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_config_observed) */
  int red;
  int green;
  int blue;
  int alpha;
  int depth;
  int stencil;
  int samples;
  int native_visual_id; /* EGL_NATIVE_VISUAL_ID, 0 when the config has none */
} nxgl_config_observed;

/* All attributes "don't care", require_native_visual=0. This is the ONLY
 * default this module will ever ship; see the header comment. */
nxgl_config_request nxgl_config_request_default(void);

/* Zero-initialized observation with version/size stamped. */
nxgl_config_observed nxgl_config_observed_init(void);

/* Pure matcher. Returns 1 when every requested attribute is satisfied by the
 * observation, 0 when at least one is violated, and -1 on a malformed call
 * (NULL pointer or version/size mismatch). Never calls EGL. */
int nxgl_config_satisfies(const nxgl_config_request *request,
                          const nxgl_config_observed *observed);

/* Formats exactly one receipt line (no trailing newline):
 *
 *   EGLCONFIG: requested=r8g8b8a8-d*-s*-m*-nv0 \
 *     observed=r8g8b8a8-d24-s8-m0-nv0x21 verdict=satisfied
 *
 * "*" marks a don't-care attribute. On violation the verdict names the first
 * failing attribute: verdict=violated:alpha. Returns the number of characters
 * that snprintf would have written, or -1 on a malformed call. */
int nxgl_format_config_receipt(const nxgl_config_request *request,
                               const nxgl_config_observed *observed,
                               char *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_CONFIG_REQUEST_H */
