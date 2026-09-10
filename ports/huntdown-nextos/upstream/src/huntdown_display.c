#define _GNU_SOURCE
#include "huntdown_display.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int hd_valid_dimension(long value) {
  return value > 0 && value < 32768;
}

static int hd_env_dimension(const char *name) {
  const char *value = getenv(name);
  if (!value || !*value) return 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  return end != value && *end == '\0' && hd_valid_dimension(parsed)
             ? (int)parsed
             : 0;
}

static int hd_env_pair(const char *width_name, const char *height_name,
                       int *width, int *height) {
  int w = hd_env_dimension(width_name);
  int h = hd_env_dimension(height_name);
  if (!w || !h) return 0;
  *width = w;
  *height = h;
  return 1;
}

static int hd_read_pair(const char *path, int *width, int *height) {
  FILE *file = fopen(path, "r");
  if (!file) return 0;
  char text[128] = {0};
  int readable = fgets(text, sizeof text, file) != NULL;
  fclose(file);
  if (!readable) return 0;

  int w = 0, h = 0;
  if (sscanf(text, "%d,%d", &w, &h) != 2 &&
      sscanf(text, "%dx%d", &w, &h) != 2 &&
      sscanf(text, "%*[^0-9]%dx%d", &w, &h) != 2)
    return 0;
  if (!hd_valid_dimension(w) || !hd_valid_dimension(h)) return 0;
  *width = w;
  *height = h;
  return 1;
}

static int hd_drm_size(int *width, int *height) {
  DIR *directory = opendir("/sys/class/drm");
  if (!directory) return 0;

  int found = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL && !found) {
    if (strncmp(entry->d_name, "card", 4) != 0 ||
        !strchr(entry->d_name, '-'))
      continue;

    char path[512];
    char state[32] = {0};
    snprintf(path, sizeof path, "/sys/class/drm/%s/status", entry->d_name);
    FILE *file = fopen(path, "r");
    if (!file) continue;
    int readable = fgets(state, sizeof state, file) != NULL;
    fclose(file);
    if (!readable || strncmp(state, "connected", 9) != 0) continue;

    snprintf(path, sizeof path, "/sys/class/drm/%s/modes", entry->d_name);
    found = hd_read_pair(path, width, height);
  }
  closedir(directory);
  return found;
}

static int hd_fb_visible_size(int *width, int *height) {
  int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  struct fb_var_screeninfo variable;
  int ok = ioctl(fd, FBIOGET_VSCREENINFO, &variable) == 0 &&
           hd_valid_dimension(variable.xres) &&
           hd_valid_dimension(variable.yres);
  close(fd);
  if (!ok) return 0;
  *width = (int)variable.xres;
  *height = (int)variable.yres;
  return 1;
}

int hd_display_size_detect(int *width, int *height, const char **source) {
  static int logged;
  if (!width || !height) return 0;
  if (source) *source = NULL;
  int w = 0, h = 0;
  const char *detected = NULL;

  if (hd_env_pair("HD_SCREEN_W", "HD_SCREEN_H", &w, &h) ||
      hd_env_pair("HD_SCREEN_WIDTH", "HD_SCREEN_HEIGHT", &w, &h)) {
    detected = "HD_SCREEN override";
  } else if (hd_env_pair("DISPLAY_WIDTH", "DISPLAY_HEIGHT", &w, &h)) {
    detected = "launcher display contract";
  } else if (hd_drm_size(&w, &h)) {
    detected = "DRM connector";
  } else if (hd_fb_visible_size(&w, &h)) {
    detected = "fb0 visible geometry";
  } else if (hd_read_pair("/sys/class/graphics/fb0/mode", &w, &h) ||
             hd_read_pair("/sys/class/graphics/fb0/modes", &w, &h)) {
    detected = "fb0 mode";
  }

  if (!detected) return 0;
  *width = w;
  *height = h;
  if (source) *source = detected;
  if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
    fprintf(stderr, "[HD-DISPLAY] visível=%dx%d fonte=%s\n", w, h,
            detected);
  }
  return 1;
}
