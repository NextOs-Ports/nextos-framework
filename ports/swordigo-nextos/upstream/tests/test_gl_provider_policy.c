/* SPDX-License-Identifier: GPL-3.0-only */
#include "gl_provider_policy.h"

#include <stdio.h>

static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

int main(void) {
  /* Exact AeUX regression: this is a GBM provider, not Wayland-only. */
  CHECK(gl_provider_name_compatible(
      "kmsdrm", "libmali-bifrost-g31-rxp0-wayland-gbm.so"));
  CHECK(gl_provider_name_compatible(
      NULL, "libmali-bifrost-g31-rxp0-wayland-gbm.so"));
  CHECK(gl_provider_name_compatible("kmsdrm", "libmali-utgard-gbm.so"));
  CHECK(gl_provider_name_compatible("kmsdrm", "libMali.so"));
  CHECK(!gl_provider_name_compatible("kmsdrm", "libmali-wayland.so"));
  CHECK(!gl_provider_name_compatible("kmsdrm", "libmali-x11.so"));
  CHECK(gl_provider_name_compatible("wayland", "libmali-wayland.so"));
  CHECK(!gl_provider_name_compatible("wayland", "libmali-x11.so"));
  CHECK(gl_provider_name_compatible("x11", "libmali-x11.so"));
  CHECK(!gl_provider_name_compatible("x11", "libmali-wayland.so"));
  CHECK(!gl_provider_name_compatible("kmsdrm", "libmali-dummy-gbm.so"));
  CHECK(!gl_provider_name_compatible("kmsdrm", "libmali-stub.so"));
  CHECK(!gl_provider_name_compatible("kmsdrm", "libmali-headless.so"));
  CHECK(!gl_provider_name_compatible("kmsdrm", NULL));

  CHECK(gl_provider_plan_sdl_pair(
            "kmsdrm", NULL, "libmali-bifrost-g31-rxp0-gbm.so",
            1, 1, 1, 1, 1) == GL_PROVIDER_SDL_PAIR_BIND_COHERENT);
  CHECK(gl_provider_plan_sdl_pair(
            "kmsdrm", "Mali-G31", "libmali-bifrost-g31-rxp0-gbm.so",
            1, 1, 1, 1, 1) == GL_PROVIDER_SDL_PAIR_NO_ACTION);
  CHECK(gl_provider_plan_sdl_pair(
            "kmsdrm", NULL, "libmali-bifrost-g31-rxp0-gbm.so",
            1, 1, 0, 1, 1) == GL_PROVIDER_SDL_PAIR_NO_ACTION);
  CHECK(gl_provider_plan_sdl_pair(
            "kmsdrm", NULL, "libmali-bifrost-g31-rxp0-gbm.so",
            1, 1, 1, 0, 1) == GL_PROVIDER_SDL_PAIR_NO_ACTION);
  CHECK(gl_provider_plan_sdl_pair(
            "kmsdrm", NULL, "libmali-wayland.so",
            1, 1, 1, 1, 1) == GL_PROVIDER_SDL_PAIR_NO_ACTION);
  CHECK(gl_provider_plan_sdl_pair(
            "kmsdrm", NULL, "libmali-bifrost-g31-rxp0-gbm.so",
            2, 1, 1, 1, 1) == GL_PROVIDER_SDL_PAIR_INVALID);

  if (failures) {
    (void)fprintf(stderr, "%d provider policy test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "Swordigo GL provider policy tests passed\n");
  return 0;
}
