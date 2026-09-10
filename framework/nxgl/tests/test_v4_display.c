/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-DISPLAY-01 host test: content rect, exact inverse input map, finite
 * policies, no-op default and the negatives. Pure: no GL, no device. */
#include "nxgl_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int condition, const char *message) {
  if (!condition) {
    (void)fprintf(stderr, "nxgl_display: %s\n", message);
    exit(1);
  }
}

static nxgl_display_request request(nxgl_display_policy policy,
                                    int32_t internal_width,
                                    int32_t internal_height) {
  nxgl_display_request value;
  memset(&value, 0, sizeof(value));
  value.struct_size = sizeof(value);
  value.api_version = NXGL_DISPLAY_API_VERSION;
  value.policy = policy;
  value.internal_width = internal_width;
  value.internal_height = internal_height;
  return value;
}

static nxgl_display_plan build(const nxgl_display_request *req, int32_t width,
                               int32_t height) {
  nxgl_display_plan plan;
  check(nxgl_display_plan_build(req, width, height, &plan) == NXGL_DISPLAY_OK,
        "a valid request was refused");
  return plan;
}

static void round_trip_corners(const nxgl_display_plan *plan) {
  int32_t content_x = 0;
  int32_t content_y = 0;
  int32_t panel_x = 0;
  int32_t panel_y = 0;
  const int32_t corners[4][2] = {
    {0, 0},
    {plan->content_width - 1, 0},
    {0, plan->content_height - 1},
    {plan->content_width - 1, plan->content_height - 1}
  };
  size_t index;
  for (index = 0; index < 4; ++index) {
    check(nxgl_display_map_output(plan, corners[index][0], corners[index][1],
                                  &panel_x, &panel_y) == 1,
          "a content corner did not map to the panel");
    check(panel_x >= plan->content_rect.x && panel_y >= plan->content_rect.y &&
          panel_x < plan->content_rect.x + plan->content_rect.width &&
          panel_y < plan->content_rect.y + plan->content_rect.height,
          "a content corner landed outside the content rect");
    check(nxgl_display_map_input(plan, panel_x, panel_y, &content_x,
                                 &content_y) == 1,
          "a content corner did not survive the inverse map");
    if (plan->content_rect.width >= plan->content_width &&
        plan->content_rect.height >= plan->content_height) {
      /* 1:1 or upscale: the inverse is exact. A downscale is many-to-one and
       * the contract deliberately does not claim exactness there. */
      check(content_x == corners[index][0] && content_y == corners[index][1],
            "the inverse transform is not exact at a corner");
    } else {
      check(content_x >= 0 && content_x < plan->content_width &&
            content_y >= 0 && content_y < plan->content_height,
            "a downscaled corner left the content space");
    }
  }
}

int main(void) {
  nxgl_display_request req;
  nxgl_display_plan plan;
  nxgl_display_policy policy;
  char receipt[256];
  int32_t x = 0;
  int32_t y = 0;

  /* Absence of a declared policy is GAME: the framework installs nothing and
   * every already approved port keeps its behaviour. */
  check(nxgl_display_policy_parse(NULL, &policy) == NXGL_DISPLAY_OK &&
        policy == NXGL_DISPLAY_POLICY_GAME,
        "an absent policy is not the no-op default");
  check(nxgl_display_policy_parse("", &policy) == NXGL_DISPLAY_OK &&
        policy == NXGL_DISPLAY_POLICY_GAME,
        "an empty policy is not the no-op default");
  check(nxgl_display_policy_parse("preserve", &policy) == NXGL_DISPLAY_OK &&
        policy == NXGL_DISPLAY_POLICY_PRESERVE, "preserve did not parse");
  check(nxgl_display_policy_parse("mali", &policy) ==
        NXGL_DISPLAY_UNKNOWN_POLICY, "an unknown policy was accepted");
  check(nxgl_display_policy_parse("Preserve", &policy) ==
        NXGL_DISPLAY_UNKNOWN_POLICY, "policy parsing is not exact");

  req = request(NXGL_DISPLAY_POLICY_GAME, 0, 0);
  plan = build(&req, 1280, 720);
  check(plan.noop == 1 && plan.letterbox_top == 0 && plan.pillarbox_left == 0,
        "the game policy produced a viewport");
  check(nxgl_display_map_input(&plan, 4242, -7, &x, &y) == 1 && x == 4242 &&
        y == -7, "the game policy altered input coordinates");

  /* preserve on a 16:9 panel with a 4:3 game: exact pillarbox, no letterbox. */
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480);
  plan = build(&req, 1280, 720);
  check(plan.content_rect.width == 960 && plan.content_rect.height == 720,
        "preserve did not fit 4:3 inside 16:9");
  check(plan.content_rect.x == 160 && plan.content_rect.y == 0,
        "preserve did not centre the content rect");
  check(plan.pillarbox_left == 160 && plan.pillarbox_right == 160 &&
        plan.letterbox_top == 0 && plan.letterbox_bottom == 0,
        "preserve reported the wrong bars");
  round_trip_corners(&plan);
  check(nxgl_display_map_input(&plan, 10, 10, &x, &y) == 0,
        "a touch on the pillarbox bar was accepted");
  check(nxgl_display_map_input(&plan, 1275, 360, &x, &y) == 0,
        "a touch on the right pillarbox bar was accepted");

  /* preserve on 480p with a 16:9 game: exact letterbox. */
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 1920, 1080);
  plan = build(&req, 640, 480);
  check(plan.content_rect.width == 640 && plan.content_rect.height == 360,
        "preserve did not letterbox 16:9 inside 4:3");
  check(plan.letterbox_top == 60 && plan.letterbox_bottom == 60,
        "preserve reported the wrong letterbox");
  round_trip_corners(&plan);
  check(nxgl_display_map_input(&plan, 320, 5, &x, &y) == 0,
        "a touch on the letterbox bar was accepted");

  /* preserve on an ultrawide panel. */
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480);
  plan = build(&req, 2560, 720);
  check(plan.content_rect.width == 960 && plan.content_rect.height == 720 &&
        plan.pillarbox_left == 800 && plan.pillarbox_right == 800,
        "preserve is wrong on an ultrawide panel");
  round_trip_corners(&plan);

  /* stretch fills the panel and only exists when declared. */
  req = request(NXGL_DISPLAY_POLICY_STRETCH, 640, 480);
  plan = build(&req, 1280, 720);
  check(plan.content_rect.x == 0 && plan.content_rect.y == 0 &&
        plan.content_rect.width == 1280 && plan.content_rect.height == 720,
        "stretch did not fill the panel");
  check(plan.letterbox_top == 0 && plan.pillarbox_left == 0,
        "stretch produced bars");
  round_trip_corners(&plan);

  /* fill covers the panel and crops the excess. */
  req = request(NXGL_DISPLAY_POLICY_FILL, 640, 480);
  plan = build(&req, 1280, 720);
  check(plan.content_rect.width == 1280 && plan.content_rect.height == 960,
        "fill did not cover the panel");
  check(plan.content_rect.y == -120 && plan.cropped == 1,
        "fill did not report a centred crop");
  check(nxgl_display_map_input(&plan, 640, 0, &x, &y) == 1,
        "a touch inside a cropped view was discarded");
  round_trip_corners(&plan);

  /* adaptive reflows to the drawable inside declared limits. */
  req = request(NXGL_DISPLAY_POLICY_ADAPTIVE, 640, 480);
  req.min_width = 320;
  req.min_height = 240;
  req.max_width = 960;
  req.max_height = 540;
  plan = build(&req, 1280, 720);
  check(plan.content_width == 960 && plan.content_height == 540,
        "adaptive ignored its declared maximum");
  check(plan.content_rect.width == 960 && plan.content_rect.height == 540 &&
        plan.content_rect.x == 160 && plan.content_rect.y == 90,
        "adaptive did not centre the limited content");
  round_trip_corners(&plan);
  plan = build(&req, 480, 320);
  check(plan.content_width == 480 && plan.content_height == 320 &&
        plan.content_rect.x == 0 && plan.content_rect.y == 0,
        "adaptive did not reflow to a small panel");

  /* Negatives: unknown policy, invalid drawable, invalid internal size,
   * incoherent limits and a foreign struct size all fail closed. */
  req = request((nxgl_display_policy)99, 640, 480);
  check(nxgl_display_plan_build(&req, 1280, 720, &plan) ==
        NXGL_DISPLAY_UNKNOWN_POLICY, "an unknown policy was accepted");
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480);
  check(nxgl_display_plan_build(&req, 0, 720, &plan) ==
        NXGL_DISPLAY_INVALID_DRAWABLE, "a zero drawable was accepted");
  check(nxgl_display_plan_build(&req, 1280, -1, &plan) ==
        NXGL_DISPLAY_INVALID_DRAWABLE, "a negative drawable was accepted");
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 0, 480);
  check(nxgl_display_plan_build(&req, 1280, 720, &plan) ==
        NXGL_DISPLAY_INVALID_INTERNAL, "a zero internal size was accepted");
  req = request(NXGL_DISPLAY_POLICY_ADAPTIVE, 640, 480);
  req.min_width = 800;
  req.min_height = 600;
  req.max_width = 640;
  req.max_height = 480;
  check(nxgl_display_plan_build(&req, 1280, 720, &plan) ==
        NXGL_DISPLAY_INVALID_LIMITS, "max below min was accepted");
  req = request(NXGL_DISPLAY_POLICY_ADAPTIVE, 640, 480);
  req.min_width = 320;
  check(nxgl_display_plan_build(&req, 1280, 720, &plan) ==
        NXGL_DISPLAY_INVALID_LIMITS, "a half-declared limit was accepted");
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480);
  req.struct_size = sizeof(req) - 1u;
  check(nxgl_display_plan_build(&req, 1280, 720, &plan) ==
        NXGL_DISPLAY_INVALID_ARGUMENT, "a foreign struct size was accepted");

  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480);
  plan = build(&req, 1280, 720);
  check(nxgl_display_receipt(&plan, receipt, sizeof(receipt)) > 0,
        "the display receipt is empty");
  check(strstr(receipt, "DISPLAY: policy=preserve") == receipt,
        "the display receipt does not name its policy");
  check(strstr(receipt, "content=160,0+960x720") != NULL,
        "the display receipt does not carry the content rect");

  /* The content-rect edges are the exact boundary of the content space. */
  req = request(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480);
  plan = build(&req, 1280, 720);
  check(nxgl_display_map_input(&plan, plan.content_rect.x,
                               plan.content_rect.y, &x, &y) == 1 &&
        x == 0 && y == 0,
        "the content rect origin is not content (0,0)");
  check(nxgl_display_map_input(&plan,
                               plan.content_rect.x + plan.content_rect.width - 1,
                               plan.content_rect.y + plan.content_rect.height - 1,
                               &x, &y) == 1 &&
        x == plan.content_width - 1 && y == plan.content_height - 1,
        "the content rect far corner is not the last content pixel");
  check(nxgl_display_map_input(&plan,
                               plan.content_rect.x + plan.content_rect.width,
                               plan.content_rect.y, &x, &y) == 0,
        "one pixel past the content rect was accepted");

  (void)printf("nxgl_display: ALL PASS\n");
  return 0;
}
