/* SPDX-License-Identifier: MIT */
#define main nxsplash_embedded_main
#include "../src/nxsplash.c"
#undef main

static int parse_value(const char *text, unsigned long maximum,
                       unsigned long *value) {
  char *end = NULL;
  unsigned long parsed;
  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno || !end || *end || parsed > maximum)
    return 0;
  *value = parsed;
  return 1;
}

int main(int argc, char **argv) {
  SoftwareSurface surface;
  unsigned long width, height, elapsed;
  size_t count;
  char title[128];
  if (argc != 5 || !parse_value(argv[1], 8192u, &width) || width < 160u ||
      !parse_value(argv[2], 8192u, &height) || height < 120u ||
      !parse_value(argv[3], NXSPLASH_DURATION_MS, &elapsed))
    return 2;
  sanitize_title(argv[4], title, sizeof(title));
  if (!software_surface_init(&surface, (int)width, (int)height))
    return 1;
  draw_software_frame(&surface, title, (uint64_t)elapsed);
  count = (size_t)surface.width * (size_t)surface.height;
  if (fwrite(surface.pixels, sizeof(*surface.pixels), count, stdout) != count) {
    software_surface_destroy(&surface);
    return 1;
  }
  software_surface_destroy(&surface);
  return 0;
}
