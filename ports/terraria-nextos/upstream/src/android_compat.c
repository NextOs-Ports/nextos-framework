#include "android_compat.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

struct ter_android_int_constant {
  const char *name;
  int value;
};

/* android.content.pm.ActivityInfo. These values are part of Android's public
 * API. Unity compares several of them while resolving autorotation; aliasing
 * every field to zero makes portrait, sensor and unspecified indistinguishable
 * from landscape and repeatedly re-enters graphics recreation. */
static const struct ter_android_int_constant g_orientation_constants[] = {
    {"SCREEN_ORIENTATION_UNSPECIFIED", -1},
    {"SCREEN_ORIENTATION_LANDSCAPE", 0},
    {"SCREEN_ORIENTATION_PORTRAIT", 1},
    {"SCREEN_ORIENTATION_USER", 2},
    {"SCREEN_ORIENTATION_BEHIND", 3},
    {"SCREEN_ORIENTATION_SENSOR", 4},
    {"SCREEN_ORIENTATION_NOSENSOR", 5},
    {"SCREEN_ORIENTATION_SENSOR_LANDSCAPE", 6},
    {"SCREEN_ORIENTATION_SENSOR_PORTRAIT", 7},
    {"SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8},
    {"SCREEN_ORIENTATION_REVERSE_PORTRAIT", 9},
    {"SCREEN_ORIENTATION_FULL_SENSOR", 10},
    {"SCREEN_ORIENTATION_USER_LANDSCAPE", 11},
    {"SCREEN_ORIENTATION_USER_PORTRAIT", 12},
    {"SCREEN_ORIENTATION_FULL_USER", 13},
    {"SCREEN_ORIENTATION_LOCKED", 14},
};

int ter_android_orientation_constant(const char *field_name, int *value) {
  if (!field_name || !value) return 0;
  for (size_t index = 0;
       index < sizeof(g_orientation_constants) /
                   sizeof(g_orientation_constants[0]);
       ++index) {
    if (strcmp(field_name, g_orientation_constants[index].name) == 0) {
      *value = g_orientation_constants[index].value;
      return 1;
    }
  }
  return 0;
}

static int text_equal_ci(const char *left, const char *right) {
  if (!left || !right) return 0;
  while (*left && *right) {
    if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
      return 0;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static int text_contains_ci(const char *text, const char *needle) {
  if (!text || !needle || !*needle) return 0;
  for (; *text; ++text) {
    const char *cursor = text;
    const char *match = needle;
    while (*cursor && *match &&
           tolower((unsigned char)*cursor) ==
               tolower((unsigned char)*match)) {
      ++cursor;
      ++match;
    }
    if (!*match) return 1;
  }
  return 0;
}

static int is_sdl_owner_override(const char *value) {
  return text_equal_ci(value, "kmsdrm") || text_equal_ci(value, "sdl") ||
         text_equal_ci(value, "wayland") || text_equal_ci(value, "x11");
}

static int is_vendor_owner_override(const char *value) {
  return text_equal_ci(value, "fbdev") || text_equal_ci(value, "mali") ||
         text_equal_ci(value, "directfb") || text_equal_ci(value, "fbcon");
}

static int is_fbdev_class_driver(const char *driver) {
  return text_equal_ci(driver, "mali") || text_equal_ci(driver, "fbcon") ||
         text_equal_ci(driver, "directfb") || text_equal_ci(driver, "fbdev");
}

static int is_powervr_provider(const char *vendor) {
  return text_contains_ci(vendor, "powervr") ||
         text_contains_ci(vendor, "imagination") ||
         text_contains_ci(vendor, "imgtec");
}

int ter_video_use_sdl_owner(const char *sdl_driver, const char *video_override,
                            int force_shim, const char *egl_vendor) {
  if (video_override && *video_override) {
    if (is_sdl_owner_override(video_override)) return 1;
    if (is_vendor_owner_override(video_override)) return 0;
  }
  if (force_shim) return 1;
  if (!sdl_driver || !*sdl_driver) return 0;
  if (!is_fbdev_class_driver(sdl_driver)) return 1;

  /* muOS builds for PowerVR can expose the historical SDL driver name
   * "mali". The name describes the downstream SDL backend, not the active
   * EGL ABI. A Mali fbdev_window is invalid to PowerVR, so let that SDL
   * backend create its own native window instead. */
  if (is_powervr_provider(egl_vendor)) return 1;
  return 0;
}

const char *ter_android_package_from_marker(const char *marker_json) {
  static const char package_current[] = "com.and.games505.Terraria";
  static const char package_historical[] = "com.and.games505.TerrariaPaid";
  static const char key[] = "\"package_id\"";
  const char *cursor;
  const char *end;
  size_t length;

  if (!marker_json) return NULL;
  cursor = strstr(marker_json, key);
  if (!cursor) return NULL;
  cursor += sizeof(key) - 1;
  while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
  if (*cursor++ != ':') return NULL;
  while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
  if (*cursor++ != '"') return NULL;
  end = strchr(cursor, '"');
  if (!end) return NULL;
  length = (size_t)(end - cursor);

  if (length == sizeof(package_current) - 1 &&
      memcmp(cursor, package_current, length) == 0)
    return package_current;
  if (length == sizeof(package_historical) - 1 &&
      memcmp(cursor, package_historical, length) == 0)
    return package_historical;
  return NULL;
}

int ter_allow_portable_provider_retry(const char *egl_provider,
                                      const char *gles_provider,
                                      int initial_attempt_failed) {
  return initial_attempt_failed && egl_provider == NULL &&
         gles_provider == NULL;
}
