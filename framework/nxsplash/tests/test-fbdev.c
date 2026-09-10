/* SPDX-License-Identifier: MIT */
#define main nxsplash_embedded_main
#include "../src/nxsplash.c"
#undef main

static int check_layout(void) {
  struct fb_fix_screeninfo fixed;
  struct fb_var_screeninfo variable;
  size_t visible_end = 0;
  memset(&fixed, 0, sizeof(fixed));
  memset(&variable, 0, sizeof(variable));
  fixed.type = FB_TYPE_PACKED_PIXELS;
  fixed.visual = FB_VISUAL_TRUECOLOR;
  fixed.line_length = 640u * 4u;
  fixed.smem_len = fixed.line_length * 480u;
  variable.xres = variable.xres_virtual = 640u;
  variable.yres = variable.yres_virtual = 480u;
  variable.bits_per_pixel = 32u;
  variable.red = (struct fb_bitfield){16u, 8u, 0u};
  variable.green = (struct fb_bitfield){8u, 8u, 0u};
  variable.blue = (struct fb_bitfield){0u, 8u, 0u};
  variable.transp = (struct fb_bitfield){24u, 8u, 0u};
  if (!framebuffer_layout_valid(&fixed, &variable, &visible_end) ||
      visible_end != (size_t)fixed.smem_len)
    return 0;
  variable.red.msb_right = 1u;
  if (framebuffer_layout_valid(&fixed, &variable, &visible_end))
    return 0;
  return 1;
}

static int check_pixels(void) {
  FramebufferTarget target;
  Color colors[2] = {{77u, 232u, 151u, 255u}, {230u, 238u, 242u, 255u}};
  unsigned char pixels[8] = {0};
  static const unsigned char expected[8] = {
      151u, 232u, 77u, 255u, 242u, 238u, 230u, 255u,
  };
  memset(&target, 0, sizeof(target));
  target.mapping = pixels;
  target.fixed.line_length = 8u;
  target.variable.xres = target.variable.xres_virtual = 2u;
  target.variable.yres = target.variable.yres_virtual = 1u;
  target.variable.bits_per_pixel = 32u;
  target.variable.red = (struct fb_bitfield){16u, 8u, 0u};
  target.variable.green = (struct fb_bitfield){8u, 8u, 0u};
  target.variable.blue = (struct fb_bitfield){0u, 8u, 0u};
  target.variable.transp = (struct fb_bitfield){24u, 8u, 0u};
  target.surface.pixels = colors;
  target.surface.width = 2;
  target.surface.height = 1;
  if (!framebuffer_present(&target))
    return 0;
  return memcmp(pixels, expected, sizeof(expected)) == 0;
}

static int check_writeback(void) {
  FramebufferTarget target;
  Color color = {77u, 232u, 151u, 255u};
  unsigned char pixels[4] = {0};
  unsigned char actual[4] = {0};
  static const unsigned char expected[4] = {151u, 232u, 77u, 255u};
  char path[] = "/tmp/nxsplash-fbdev-test.XXXXXX";
  int descriptor = mkstemp(path);
  int ok = 0;
  if (descriptor < 0 || ftruncate(descriptor, sizeof(actual)) != 0)
    goto out;
  memset(&target, 0, sizeof(target));
  target.descriptor = descriptor;
  target.mapping = pixels;
  target.mapping_length = sizeof(pixels);
  target.writeback = 1;
  target.fixed.line_length = 4u;
  target.variable.xres = target.variable.xres_virtual = 1u;
  target.variable.yres = target.variable.yres_virtual = 1u;
  target.variable.bits_per_pixel = 32u;
  target.variable.red = (struct fb_bitfield){16u, 8u, 0u};
  target.variable.green = (struct fb_bitfield){8u, 8u, 0u};
  target.variable.blue = (struct fb_bitfield){0u, 8u, 0u};
  target.variable.transp = (struct fb_bitfield){24u, 8u, 0u};
  target.surface.pixels = &color;
  target.surface.width = 1;
  target.surface.height = 1;
  if (!framebuffer_present(&target) ||
      lseek(descriptor, 0, SEEK_SET) < 0 ||
      read(descriptor, actual, sizeof(actual)) != (ssize_t)sizeof(actual))
    goto out;
  ok = memcmp(actual, expected, sizeof(expected)) == 0;
out:
  if (descriptor >= 0)
    close(descriptor);
  unlink(path);
  return ok;
}

int main(void) {
  if (!check_layout() || !check_pixels() || !check_writeback()) {
    fputs("nxsplash fbdev test: FAIL\n", stderr);
    return 1;
  }
  puts("nxsplash fbdev test: PASS");
  return 0;
}
