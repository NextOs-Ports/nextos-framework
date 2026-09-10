/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static nxgl_surface_metrics_input_v2 valid_input(void) {
  nxgl_surface_metrics_input_v2 input;
  memset(&input, 0, sizeof(input));
  input.api_version = NXGL_API_VERSION_V2;
  input.struct_size = sizeof(input);
  input.display_width = 640;
  input.display_height = 480;
  input.drawable_width = 1280;
  input.drawable_height = 720;
  input.viewport_x = 160;
  input.viewport_y = 0;
  input.viewport_width = 960;
  input.viewport_height = 720;
  input.render_target_width = 1920;
  input.render_target_height = 1080;
  return input;
}

static void test_dimensions_remain_distinct(void) {
  nxgl_surface_metrics_input_v2 input = valid_input();
  nxgl_surface_metrics_v2 metrics;
  memset(&metrics, 0, sizeof(metrics));
  CHECK(nxgl_calculate_surface_metrics_v2(&input, &metrics) == NXGL_SUCCESS);
  CHECK(metrics.api_version == NXGL_API_VERSION_V2);
  CHECK(metrics.struct_size == sizeof(metrics));
  CHECK(metrics.display_width == 640 && metrics.display_height == 480);
  CHECK(metrics.drawable_width == 1280 && metrics.drawable_height == 720);
  CHECK(metrics.viewport_x == 160 && metrics.viewport_y == 0);
  CHECK(metrics.viewport_width == 960 && metrics.viewport_height == 720);
  CHECK(metrics.render_target_width == 1920);
  CHECK(metrics.render_target_height == 1080);
  CHECK(metrics.drawable_per_display_scale_x == 2.0);
  CHECK(metrics.drawable_per_display_scale_y == 1.5);
  CHECK(metrics.render_target_per_viewport_scale_x == 2.0);
  CHECK(metrics.render_target_per_viewport_scale_y == 1.5);
}

static void test_no_resolution_or_integer_scale_assumption(void) {
  nxgl_surface_metrics_input_v2 input = valid_input();
  nxgl_surface_metrics_v2 metrics;
  input.display_width = 854;
  input.display_height = 480;
  input.drawable_width = 1280;
  input.drawable_height = 800;
  input.viewport_x = 11;
  input.viewport_y = 13;
  input.viewport_width = 1001;
  input.viewport_height = 701;
  input.render_target_width = 777;
  input.render_target_height = 555;
  CHECK(nxgl_calculate_surface_metrics_v2(&input, &metrics) == NXGL_SUCCESS);
  CHECK(metrics.drawable_per_display_scale_x > 1.49);
  CHECK(metrics.drawable_per_display_scale_x < 1.50);
  CHECK(metrics.render_target_per_viewport_scale_x > 0.77);
  CHECK(metrics.render_target_per_viewport_scale_x < 0.78);
}

static void test_invalid_input_is_atomic(void) {
  nxgl_surface_metrics_input_v2 input = valid_input();
  nxgl_surface_metrics_v2 metrics;
  nxgl_surface_metrics_v2 before;
  memset(&metrics, 0x5a, sizeof(metrics));
  memcpy(&before, &metrics, sizeof(before));
  input.viewport_x = input.drawable_width - input.viewport_width + 1;
  CHECK(nxgl_calculate_surface_metrics_v2(&input, &metrics) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&metrics, &before, sizeof(metrics)) == 0);

  input = valid_input();
  input.render_target_height = 0;
  CHECK(nxgl_calculate_surface_metrics_v2(&input, &metrics) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&metrics, &before, sizeof(metrics)) == 0);

  input = valid_input();
  input.drawable_width = NXGL_SURFACE_DIMENSION_MAX + 1;
  CHECK(nxgl_calculate_surface_metrics_v2(&input, &metrics) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&metrics, &before, sizeof(metrics)) == 0);

  input = valid_input();
  input.api_version = NXGL_API_VERSION;
  CHECK(nxgl_calculate_surface_metrics_v2(&input, &metrics) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&metrics, &before, sizeof(metrics)) == 0);
}

int main(void) {
  test_dimensions_remain_distinct();
  test_no_resolution_or_integer_scale_assumption();
  test_invalid_input_is_atomic();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl metrics test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "nxgl pure surface metrics tests passed\n");
  return 0;
}
