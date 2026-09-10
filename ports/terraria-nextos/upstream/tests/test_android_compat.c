#include "android_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_int(const char *label, int actual, int expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: got %d, expected %d\n", label, actual, expected);
    exit(1);
  }
}

static void expect_constant(const char *name, int expected) {
  int value = 999;
  expect_int(name, ter_android_orientation_constant(name, &value), 1);
  expect_int(name, value, expected);
}

int main(void) {
  expect_constant("SCREEN_ORIENTATION_UNSPECIFIED", -1);
  expect_constant("SCREEN_ORIENTATION_LANDSCAPE", 0);
  expect_constant("SCREEN_ORIENTATION_PORTRAIT", 1);
  expect_constant("SCREEN_ORIENTATION_SENSOR", 4);
  expect_constant("SCREEN_ORIENTATION_SENSOR_LANDSCAPE", 6);
  expect_constant("SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8);
  expect_constant("SCREEN_ORIENTATION_FULL_SENSOR", 10);
  expect_constant("SCREEN_ORIENTATION_USER_LANDSCAPE", 11);
  expect_constant("SCREEN_ORIENTATION_FULL_USER", 13);

  {
    int value = 123;
    expect_int("unknown constant",
               ter_android_orientation_constant("LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT",
                                                &value),
               0);
    expect_int("unknown constant output", value, 123);
  }

  expect_int("proven Mali fbdev",
             ter_video_use_sdl_owner("mali", NULL, 0, "ARM"), 0);
  expect_int("PowerVR behind mali-named SDL",
             ter_video_use_sdl_owner("mali", NULL, 0,
                                     "Imagination Technologies"),
             1);
  expect_int("PowerVR spelling",
             ter_video_use_sdl_owner("fbdev", NULL, 0, "PowerVR"), 1);
  expect_int("KMSDRM", ter_video_use_sdl_owner("KMSDRM", NULL, 0, "ARM"), 1);
  expect_int("Wayland", ter_video_use_sdl_owner("wayland", NULL, 0, ""), 1);
  expect_int("no driver", ter_video_use_sdl_owner(NULL, NULL, 0, "PowerVR"), 0);
  expect_int("force shim", ter_video_use_sdl_owner("mali", NULL, 1, "ARM"), 1);
  expect_int("explicit Mali override wins",
             ter_video_use_sdl_owner("mali", "mali", 1,
                                     "Imagination Technologies"),
             0);
  expect_int("explicit SDL override wins",
             ter_video_use_sdl_owner("mali", "sdl", 0, "ARM"), 1);

  {
    const char *package = ter_android_package_from_marker(
        "{\n  \"package_id\": \"com.and.games505.Terraria\"\n}\n");
    expect_int("current package marker", package != NULL, 1);
    expect_int("current package value",
               package && strcmp(package, "com.and.games505.Terraria") == 0,
               1);
  }
  {
    const char *package = ter_android_package_from_marker(
        "{\"package_id\":\"com.and.games505.TerrariaPaid\"}");
    expect_int("historical package marker", package != NULL, 1);
    expect_int("historical package value",
               package &&
                   strcmp(package, "com.and.games505.TerrariaPaid") == 0,
               1);
  }
  expect_int("foreign package rejected",
             ter_android_package_from_marker(
                 "{\"package_id\":\"com.example.foreign\"}") != NULL,
             0);
  expect_int("package prefix rejected",
             ter_android_package_from_marker(
                 "{\"package_id\":\"com.and.games505.TerrariaPaid.extra\"}") !=
                 NULL,
             0);
  expect_int("malformed package marker",
             ter_android_package_from_marker("{\"package_id\":42}") != NULL,
             0);

  expect_int("portable providers after measured failure",
             ter_allow_portable_provider_retry(NULL, NULL, 1), 1);
  expect_int("portable providers not proactive",
             ter_allow_portable_provider_retry(NULL, NULL, 0), 0);
  expect_int("explicit EGL provider preserved",
             ter_allow_portable_provider_retry("libEGL.so", NULL, 1), 0);
  expect_int("explicit GLES provider preserved",
             ter_allow_portable_provider_retry(NULL, "libGLESv2.so", 1), 0);
  expect_int("explicit empty provider preserved",
             ter_allow_portable_provider_retry("", NULL, 1), 0);

  puts("android compatibility tests: PASS");
  return 0;
}
