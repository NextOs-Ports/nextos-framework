/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-DISPLAY-01: pure content-rect policy. No GL, no I/O, no ambient state. */

#include "nxgl_display.h"

#include <stdio.h>
#include <string.h>

#define NXGL_DISPLAY_MAX_EXTENT 65535

static int nxgl_display_extent_valid(int32_t value) {
  return value > 0 && value <= NXGL_DISPLAY_MAX_EXTENT;
}

static int nxgl_display_limits_valid(const nxgl_display_request *request) {
  const int32_t values[4] = {request->min_width, request->min_height,
                             request->max_width, request->max_height};
  size_t index;
  for (index = 0; index < 4; ++index) {
    if (values[index] < 0 || values[index] > NXGL_DISPLAY_MAX_EXTENT) {
      return 0;
    }
  }
  if ((request->min_width != 0) != (request->min_height != 0)) {
    return 0;
  }
  if ((request->max_width != 0) != (request->max_height != 0)) {
    return 0;
  }
  if (request->max_width != 0 && request->min_width != 0 &&
      (request->max_width < request->min_width ||
       request->max_height < request->min_height)) {
    return 0;
  }
  return 1;
}

static int32_t nxgl_display_clamp(int32_t value, int32_t low, int32_t high) {
  if (low != 0 && value < low) {
    value = low;
  }
  if (high != 0 && value > high) {
    value = high;
  }
  return value;
}

/* Largest integer scale-preserving box of aspect (aw:ah) that fits in
 * (bw x bh) when contain is non-zero, or that covers it when contain is
 * zero. Integer arithmetic only: no float rounding drift between the
 * presentation transform and its inverse. */
static void nxgl_display_fit(int32_t aw, int32_t ah, int32_t bw, int32_t bh,
                             int contain, int32_t *out_w, int32_t *out_h) {
  const int64_t wide = (int64_t)aw * (int64_t)bh;
  const int64_t tall = (int64_t)ah * (int64_t)bw;
  int32_t width;
  int32_t height;
  const int width_limited = contain ? (wide >= tall) : (wide <= tall);
  if (width_limited) {
    width = bw;
    height = (int32_t)(((int64_t)bw * (int64_t)ah) / (int64_t)aw);
    if (height < 1) {
      height = 1;
    }
  } else {
    height = bh;
    width = (int32_t)(((int64_t)bh * (int64_t)aw) / (int64_t)ah);
    if (width < 1) {
      width = 1;
    }
  }
  *out_w = width;
  *out_h = height;
}

nxgl_display_status nxgl_display_plan_build(const nxgl_display_request *request,
                                            int32_t drawable_width,
                                            int32_t drawable_height,
                                            nxgl_display_plan *out_plan) {
  nxgl_display_plan plan;
  int32_t width;
  int32_t height;

  if (request == NULL || out_plan == NULL) {
    return NXGL_DISPLAY_INVALID_ARGUMENT;
  }
  if (request->struct_size != sizeof(*request) ||
      request->api_version != NXGL_DISPLAY_API_VERSION) {
    return NXGL_DISPLAY_INVALID_ARGUMENT;
  }
  switch (request->policy) {
    case NXGL_DISPLAY_POLICY_GAME:
    case NXGL_DISPLAY_POLICY_PRESERVE:
    case NXGL_DISPLAY_POLICY_ADAPTIVE:
    case NXGL_DISPLAY_POLICY_FILL:
    case NXGL_DISPLAY_POLICY_STRETCH:
      break;
    default:
      return NXGL_DISPLAY_UNKNOWN_POLICY;
  }
  if (!nxgl_display_extent_valid(drawable_width) ||
      !nxgl_display_extent_valid(drawable_height)) {
    return NXGL_DISPLAY_INVALID_DRAWABLE;
  }
  if (!nxgl_display_limits_valid(request)) {
    return NXGL_DISPLAY_INVALID_LIMITS;
  }

  memset(&plan, 0, sizeof(plan));
  plan.struct_size = sizeof(plan);
  plan.api_version = NXGL_DISPLAY_API_VERSION;
  plan.policy = request->policy;
  plan.drawable_width = drawable_width;
  plan.drawable_height = drawable_height;

  if (request->policy == NXGL_DISPLAY_POLICY_GAME) {
    /* The framework installs nothing at all. The plan still describes the
     * measured drawable so a receipt can be emitted without lying about a
     * viewport nobody set. */
    plan.noop = 1;
    plan.content_width = drawable_width;
    plan.content_height = drawable_height;
    plan.content_rect.width = drawable_width;
    plan.content_rect.height = drawable_height;
    *out_plan = plan;
    return NXGL_DISPLAY_OK;
  }

  if (!nxgl_display_extent_valid(request->internal_width) ||
      !nxgl_display_extent_valid(request->internal_height)) {
    return NXGL_DISPLAY_INVALID_INTERNAL;
  }

  switch (request->policy) {
    case NXGL_DISPLAY_POLICY_STRETCH:
      plan.content_width = request->internal_width;
      plan.content_height = request->internal_height;
      plan.content_rect.x = 0;
      plan.content_rect.y = 0;
      plan.content_rect.width = drawable_width;
      plan.content_rect.height = drawable_height;
      break;
    case NXGL_DISPLAY_POLICY_ADAPTIVE:
      width = nxgl_display_clamp(drawable_width, request->min_width,
                                 request->max_width);
      height = nxgl_display_clamp(drawable_height, request->min_height,
                                  request->max_height);
      plan.content_width = width;
      plan.content_height = height;
      /* The reflowed image keeps 1:1 pixels when it fits; when a declared
       * limit made it smaller than the panel it is centred, never stretched. */
      if (width > drawable_width || height > drawable_height) {
        nxgl_display_fit(width, height, drawable_width, drawable_height, 1,
                         &plan.content_rect.width, &plan.content_rect.height);
      } else {
        plan.content_rect.width = width;
        plan.content_rect.height = height;
      }
      plan.content_rect.x = (drawable_width - plan.content_rect.width) / 2;
      plan.content_rect.y = (drawable_height - plan.content_rect.height) / 2;
      break;
    case NXGL_DISPLAY_POLICY_FILL:
      plan.content_width = request->internal_width;
      plan.content_height = request->internal_height;
      nxgl_display_fit(request->internal_width, request->internal_height,
                       drawable_width, drawable_height, 0,
                       &plan.content_rect.width, &plan.content_rect.height);
      plan.content_rect.x = (drawable_width - plan.content_rect.width) / 2;
      plan.content_rect.y = (drawable_height - plan.content_rect.height) / 2;
      plan.cropped = (plan.content_rect.width > drawable_width ||
                      plan.content_rect.height > drawable_height) ? 1 : 0;
      break;
    case NXGL_DISPLAY_POLICY_PRESERVE:
    default:
      plan.content_width = request->internal_width;
      plan.content_height = request->internal_height;
      nxgl_display_fit(request->internal_width, request->internal_height,
                       drawable_width, drawable_height, 1,
                       &plan.content_rect.width, &plan.content_rect.height);
      plan.content_rect.x = (drawable_width - plan.content_rect.width) / 2;
      plan.content_rect.y = (drawable_height - plan.content_rect.height) / 2;
      break;
  }

  if (plan.content_rect.width < 1 || plan.content_rect.height < 1) {
    return NXGL_DISPLAY_INVALID_DRAWABLE;
  }
  plan.pillarbox_left = plan.content_rect.x > 0 ? plan.content_rect.x : 0;
  plan.pillarbox_right = drawable_width -
                         (plan.content_rect.x + plan.content_rect.width);
  if (plan.pillarbox_right < 0) {
    plan.pillarbox_right = 0;
  }
  plan.letterbox_top = plan.content_rect.y > 0 ? plan.content_rect.y : 0;
  plan.letterbox_bottom = drawable_height -
                          (plan.content_rect.y + plan.content_rect.height);
  if (plan.letterbox_bottom < 0) {
    plan.letterbox_bottom = 0;
  }
  *out_plan = plan;
  return NXGL_DISPLAY_OK;
}

static int nxgl_display_plan_valid(const nxgl_display_plan *plan) {
  return plan != NULL && plan->struct_size == sizeof(*plan) &&
         plan->api_version == NXGL_DISPLAY_API_VERSION &&
         plan->content_rect.width > 0 && plan->content_rect.height > 0 &&
         plan->content_width > 0 && plan->content_height > 0;
}

int nxgl_display_map_input(const nxgl_display_plan *plan,
                           int32_t panel_x, int32_t panel_y,
                           int32_t *out_content_x, int32_t *out_content_y) {
  int64_t local_x;
  int64_t local_y;
  if (!nxgl_display_plan_valid(plan) || out_content_x == NULL ||
      out_content_y == NULL) {
    return 0;
  }
  if (plan->noop) {
    *out_content_x = panel_x;
    *out_content_y = panel_y;
    return 1;
  }
  local_x = (int64_t)panel_x - (int64_t)plan->content_rect.x;
  local_y = (int64_t)panel_y - (int64_t)plan->content_rect.y;
  if (local_x < 0 || local_y < 0 ||
      local_x >= (int64_t)plan->content_rect.width ||
      local_y >= (int64_t)plan->content_rect.height) {
    /* A touch on a letterbox/pillarbox bar, or outside a cropped view, is
     * discarded. Clamping it into the edge would invent input. */
    return 0;
  }
  *out_content_x = (int32_t)((local_x * (int64_t)plan->content_width) /
                             (int64_t)plan->content_rect.width);
  *out_content_y = (int32_t)((local_y * (int64_t)plan->content_height) /
                             (int64_t)plan->content_rect.height);
  return 1;
}

int nxgl_display_map_output(const nxgl_display_plan *plan,
                            int32_t content_x, int32_t content_y,
                            int32_t *out_panel_x, int32_t *out_panel_y) {
  if (!nxgl_display_plan_valid(plan) || out_panel_x == NULL ||
      out_panel_y == NULL) {
    return 0;
  }
  if (plan->noop) {
    *out_panel_x = content_x;
    *out_panel_y = content_y;
    return 1;
  }
  if (content_x < 0 || content_y < 0 || content_x >= plan->content_width ||
      content_y >= plan->content_height) {
    return 0;
  }
  /* Map to the CENTRE of the content cell, not to its edge. With the
   * floor-based inverse above this is what makes panel<->content exact for
   * every 1:1 or upscaled presentation; a downscale is inherently many-to-one
   * and the contract does not pretend otherwise. */
  *out_panel_x = plan->content_rect.x +
                 (int32_t)((((int64_t)content_x * 2 + 1) *
                            (int64_t)plan->content_rect.width) /
                           ((int64_t)plan->content_width * 2));
  *out_panel_y = plan->content_rect.y +
                 (int32_t)((((int64_t)content_y * 2 + 1) *
                            (int64_t)plan->content_rect.height) /
                           ((int64_t)plan->content_height * 2));
  return 1;
}

const char *nxgl_display_policy_name(nxgl_display_policy policy) {
  switch (policy) {
    case NXGL_DISPLAY_POLICY_GAME: return "game";
    case NXGL_DISPLAY_POLICY_PRESERVE: return "preserve";
    case NXGL_DISPLAY_POLICY_ADAPTIVE: return "adaptive";
    case NXGL_DISPLAY_POLICY_FILL: return "fill";
    case NXGL_DISPLAY_POLICY_STRETCH: return "stretch";
    default: return "unknown";
  }
}

nxgl_display_status nxgl_display_policy_parse(const char *name,
                                              nxgl_display_policy *out_policy) {
  static const struct {
    const char *name;
    nxgl_display_policy policy;
  } table[] = {
    {"game", NXGL_DISPLAY_POLICY_GAME},
    {"preserve", NXGL_DISPLAY_POLICY_PRESERVE},
    {"adaptive", NXGL_DISPLAY_POLICY_ADAPTIVE},
    {"fill", NXGL_DISPLAY_POLICY_FILL},
    {"stretch", NXGL_DISPLAY_POLICY_STRETCH}
  };
  size_t index;
  if (out_policy == NULL) {
    return NXGL_DISPLAY_INVALID_ARGUMENT;
  }
  if (name == NULL || name[0] == '\0') {
    /* Absence of a declared policy is a no-op, never a silent letterbox. */
    *out_policy = NXGL_DISPLAY_POLICY_GAME;
    return NXGL_DISPLAY_OK;
  }
  for (index = 0; index < sizeof(table) / sizeof(table[0]); ++index) {
    if (strcmp(name, table[index].name) == 0) {
      *out_policy = table[index].policy;
      return NXGL_DISPLAY_OK;
    }
  }
  return NXGL_DISPLAY_UNKNOWN_POLICY;
}

size_t nxgl_display_receipt(const nxgl_display_plan *plan,
                            char *buf, size_t cap) {
  char scratch[256];
  int written;
  if (plan == NULL || plan->struct_size != sizeof(*plan)) {
    return 0;
  }
  written = snprintf(
      scratch, sizeof(scratch),
      "DISPLAY: policy=%s internal=%dx%d drawable=%dx%d content=%d,%d+%dx%d "
      "letterbox=%d/%d pillarbox=%d/%d cropped=%d noop=%d",
      nxgl_display_policy_name(plan->policy), plan->content_width,
      plan->content_height, plan->drawable_width, plan->drawable_height,
      plan->content_rect.x, plan->content_rect.y, plan->content_rect.width,
      plan->content_rect.height, plan->letterbox_top, plan->letterbox_bottom,
      plan->pillarbox_left, plan->pillarbox_right, plan->cropped, plan->noop);
  if (written < 0) {
    return 0;
  }
  if (buf != NULL && cap > 0) {
    size_t copy = (size_t)written < cap - 1 ? (size_t)written : cap - 1;
    memcpy(buf, scratch, copy);
    buf[copy] = '\0';
  }
  return (size_t)written;
}
