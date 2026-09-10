/* Port-local regression harness for FP2's vendored frame-proof adapter.
 * GL is fully fake; each process runs one scenario because the adapter keeps
 * deliberate process-lifetime state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/nxgl_frame_proof_adapter.h"

static size_t visible_pixels;
static int rgb_with_zero_alpha;

static void fake_read_pixels(int x, int y, int width, int height,
                             unsigned format, unsigned type, void *data)
{
    (void)x;
    (void)y;
    (void)format;
    (void)type;
    unsigned char *pixels = data;
    size_t count = (size_t)width * (size_t)height;
    for (size_t i = 0; i < count; i++) {
        int coloured = rgb_with_zero_alpha || i < visible_pixels;
        pixels[i * 4 + 0] = coloured ? 200 : 0;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = rgb_with_zero_alpha ? 0 : 255;
    }
}

static unsigned fake_get_error(void)
{
    return 0;
}

static const unsigned char *fake_get_string(unsigned name)
{
    switch (name) {
    case 0x1F01: /* GL_RENDERER */
        return (const unsigned char *)"Fake-GPU";
    case 0x1F02: /* GL_VERSION */
        return (const unsigned char *)"OpenGL ES 3.2 fake";
    case 0x1F03: /* GL_EXTENSIONS */
        return (const unsigned char *)"";
    default:
        return NULL;
    }
}

static void fake_get_integerv(unsigned name, int *value)
{
    switch (name) {
    case 0x0D05: /* GL_PACK_ALIGNMENT */
        *value = 4;
        break;
    case 0x8CA6: /* GL_FRAMEBUFFER_BINDING */
    case 0x8CAA: /* GL_READ_FRAMEBUFFER_BINDING */
    case 0x88ED: /* GL_PIXEL_PACK_BUFFER_BINDING */
    case 0x0D02: /* GL_PACK_ROW_LENGTH */
    case 0x0D03: /* GL_PACK_SKIP_ROWS */
    case 0x0D04: /* GL_PACK_SKIP_PIXELS */
        *value = 0;
        break;
    default:
        *value = 0;
        break;
    }
}

static void *fake_resolver(const char *name)
{
    if (strcmp(name, "glReadPixels") == 0)
        return (void *)fake_read_pixels;
    if (strcmp(name, "glGetError") == 0)
        return (void *)fake_get_error;
    if (strcmp(name, "glGetString") == 0)
        return (void *)fake_get_string;
    if (strcmp(name, "glGetIntegerv") == 0)
        return (void *)fake_get_integerv;
    return NULL;
}

static unsigned long presented;

static void advance_to(unsigned long target, size_t visible, int alpha_zero)
{
    visible_pixels = visible;
    rgb_with_zero_alpha = alpha_zero;
    while (presented < target) {
        nxgl_frame_proof_before_present(100, 100);
        presented++;
    }
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "partial-three";
    nxgl_frame_proof_set_resolver(fake_resolver);
    nxgl_frame_proof_set_video_context(100, 100, "KMSDRM", "Fake-GPU",
                                       "OpenGL ES 3.2 fake");
    nxgl_frame_proof_launch_receipt();

    if (strcmp(scenario, "partial-three") == 0) {
        advance_to(600, 20, 0);       /* 0.2% visible, three samples. */
    } else if (strcmp(scenario, "zero-three") == 0) {
        advance_to(600, 0, 0);
    } else if (strcmp(scenario, "black-partial-black") == 0) {
        advance_to(30, 0, 0);
        advance_to(120, 20, 0);
        advance_to(600, 0, 0);
    } else if (strcmp(scenario, "ok-then-zero-three") == 0) {
        advance_to(30, 10000, 0);
        advance_to(1800, 0, 0);
    } else if (strcmp(scenario, "rgb-alpha-zero") == 0) {
        advance_to(600, 0, 1);
    } else if (strcmp(scenario, "proof-off") == 0) {
        advance_to(600, 0, 0);
    } else {
        fprintf(stderr, "unknown scenario: %s\n", scenario);
        return 2;
    }

    nxgl_frame_proof_publish();
    return 0;
}
