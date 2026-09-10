/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * V4-DISPLAY-01: pure presentation policy.
 *
 * Given a declared internal resolution, a policy and the drawable actually
 * measured this run, produce the content rect and the two transforms
 * (presentation and its exact inverse for touch/cursor).  There is no GL, no
 * I/O, no environment and no decision by device, CFW, GPU or game name here.
 *
 * The contract resolves the V3.1 specification's own contradiction: the
 * absence of a declared policy is NXGL_DISPLAY_POLICY_GAME - the framework
 * does not intervene and every already approved port keeps byte-identical
 * behaviour.  NXGL_DISPLAY_POLICY_PRESERVE letterboxes and is therefore an
 * explicit opt-in, never a default, because it changes pixels.
 */
#ifndef NXGL_DISPLAY_H
#define NXGL_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_DISPLAY_API_VERSION 1u

typedef enum nxgl_display_policy {
  /* The port controls its own presentation. Absence of a declared policy
   * resolves here: the framework installs nothing. */
  NXGL_DISPLAY_POLICY_GAME = 0,
  /* Keep the internal aspect; letterbox/pillarbox the remainder. */
  NXGL_DISPLAY_POLICY_PRESERVE = 1,
  /* Reflow to the real drawable inside the declared min/max limits. */
  NXGL_DISPLAY_POLICY_ADAPTIVE = 2,
  /* Fill the panel keeping aspect; the excess is cropped symmetrically. */
  NXGL_DISPLAY_POLICY_FILL = 3,
  /* Stretch to the panel. Only when the port declares it tolerates
   * distortion. Never a default. */
  NXGL_DISPLAY_POLICY_STRETCH = 4
} nxgl_display_policy;

typedef enum nxgl_display_status {
  NXGL_DISPLAY_OK = 0,
  NXGL_DISPLAY_INVALID_ARGUMENT = 1,
  NXGL_DISPLAY_UNKNOWN_POLICY = 2,
  NXGL_DISPLAY_INVALID_INTERNAL = 3,
  NXGL_DISPLAY_INVALID_DRAWABLE = 4,
  NXGL_DISPLAY_INVALID_LIMITS = 5
} nxgl_display_status;

/* Declared by the port. Limits apply to ADAPTIVE only and are ignored (but
 * still validated when non-zero) by the other policies. */
typedef struct nxgl_display_request {
  size_t struct_size;
  uint32_t api_version;
  nxgl_display_policy policy;
  int32_t internal_width;
  int32_t internal_height;
  int32_t min_width;
  int32_t min_height;
  int32_t max_width;
  int32_t max_height;
} nxgl_display_request;

typedef struct nxgl_display_rect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
} nxgl_display_rect;

typedef struct nxgl_display_plan {
  size_t struct_size;
  uint32_t api_version;
  nxgl_display_policy policy;
  /* Physical drawable measured this run. */
  int32_t drawable_width;
  int32_t drawable_height;
  /* Resolution the game renders at. Equals internal_* except for ADAPTIVE. */
  int32_t content_width;
  int32_t content_height;
  /* Where that content lands inside the drawable. */
  nxgl_display_rect content_rect;
  /* Letterbox (top/bottom) and pillarbox (left/right) bar thickness. */
  int32_t letterbox_top;
  int32_t letterbox_bottom;
  int32_t pillarbox_left;
  int32_t pillarbox_right;
  /* Non-zero when the port asked to keep aspect but the panel forced a
   * crop of the internal image (FILL). */
  int32_t cropped;
  /* Non-zero when the framework installs nothing at all. */
  int32_t noop;
} nxgl_display_plan;

nxgl_display_status nxgl_display_plan_build(const nxgl_display_request *request,
                                            int32_t drawable_width,
                                            int32_t drawable_height,
                                            nxgl_display_plan *out_plan);

/* Panel coordinates -> content space. Returns 0 and leaves the outputs
 * untouched when the point falls on a letterbox/pillarbox bar or outside the
 * content rect: such a touch is discarded, never clamped into the edge. */
int nxgl_display_map_input(const nxgl_display_plan *plan,
                           int32_t panel_x, int32_t panel_y,
                           int32_t *out_content_x, int32_t *out_content_y);

/* Content space -> panel coordinates (the presentation transform). */
int nxgl_display_map_output(const nxgl_display_plan *plan,
                            int32_t content_x, int32_t content_y,
                            int32_t *out_panel_x, int32_t *out_panel_y);

const char *nxgl_display_policy_name(nxgl_display_policy policy);
/* Parse a declared policy name. An empty or NULL name is GAME; an unknown
 * name fails closed. */
nxgl_display_status nxgl_display_policy_parse(const char *name,
                                              nxgl_display_policy *out_policy);

/* One-line receipt. Returns the number of bytes that would be written. */
size_t nxgl_display_receipt(const nxgl_display_plan *plan,
                            char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_DISPLAY_H */
