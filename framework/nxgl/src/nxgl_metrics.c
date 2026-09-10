/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"

#include <string.h>

static int nxgl_metrics_dimension_valid(int value) {
  return value > 0 && value <= NXGL_SURFACE_DIMENSION_MAX;
}

int nxgl_calculate_surface_metrics_v2(
    const nxgl_surface_metrics_input_v2 *input,
    nxgl_surface_metrics_v2 *metrics) {
  nxgl_surface_metrics_v2 calculated;

  if (!input || !metrics || input->api_version != NXGL_API_VERSION_V2 ||
      input->struct_size < sizeof(*input) ||
      !nxgl_metrics_dimension_valid(input->display_width) ||
      !nxgl_metrics_dimension_valid(input->display_height) ||
      !nxgl_metrics_dimension_valid(input->drawable_width) ||
      !nxgl_metrics_dimension_valid(input->drawable_height) ||
      !nxgl_metrics_dimension_valid(input->viewport_width) ||
      !nxgl_metrics_dimension_valid(input->viewport_height) ||
      !nxgl_metrics_dimension_valid(input->render_target_width) ||
      !nxgl_metrics_dimension_valid(input->render_target_height) ||
      input->viewport_x < 0 ||
      input->viewport_x > NXGL_SURFACE_DIMENSION_MAX ||
      input->viewport_y < 0 ||
      input->viewport_y > NXGL_SURFACE_DIMENSION_MAX ||
      input->viewport_width > input->drawable_width ||
      input->viewport_height > input->drawable_height ||
      input->viewport_x > input->drawable_width - input->viewport_width ||
      input->viewport_y > input->drawable_height - input->viewport_height)
    return NXGL_ERROR_INVALID_ARGUMENT;

  memset(&calculated, 0, sizeof(calculated));
  calculated.api_version = NXGL_API_VERSION_V2;
  calculated.struct_size = sizeof(calculated);
  calculated.display_width = input->display_width;
  calculated.display_height = input->display_height;
  calculated.drawable_width = input->drawable_width;
  calculated.drawable_height = input->drawable_height;
  calculated.viewport_x = input->viewport_x;
  calculated.viewport_y = input->viewport_y;
  calculated.viewport_width = input->viewport_width;
  calculated.viewport_height = input->viewport_height;
  calculated.render_target_width = input->render_target_width;
  calculated.render_target_height = input->render_target_height;
  calculated.drawable_per_display_scale_x =
      (double)input->drawable_width / (double)input->display_width;
  calculated.drawable_per_display_scale_y =
      (double)input->drawable_height / (double)input->display_height;
  calculated.render_target_per_viewport_scale_x =
      (double)input->render_target_width / (double)input->viewport_width;
  calculated.render_target_per_viewport_scale_y =
      (double)input->render_target_height / (double)input->viewport_height;
  *metrics = calculated;
  return NXGL_SUCCESS;
}
