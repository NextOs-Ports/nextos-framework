/* SPDX-License-Identifier: GPL-3.0-only
 * End-to-end proof of the production failure shape: the adapter resolves an
 * ES3 facade while every real state/read call beneath it is physical GLES2.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/shim/nx_gles3_frame_proof_state.h"
#include "../../src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.h"

static unsigned physical_es3_queries;
static unsigned read_calls;

static void physical_gles2_get_integerv(unsigned pname, int *value)
{
    switch (pname) {
    case GL_VIEWPORT:
        value[0] = 0;
        value[1] = 0;
        value[2] = 64;
        value[3] = 48;
        return;
    case GL_PACK_ALIGNMENT:
        *value = 4;
        return;
    case GL_READ_FRAMEBUFFER_BINDING:
    case GL_PIXEL_PACK_BUFFER_BINDING:
    case GL_PACK_ROW_LENGTH:
    case GL_PACK_SKIP_ROWS:
    case GL_PACK_SKIP_PIXELS:
        /* Real Mali-450 behavior: INVALID_ENUM and caller bytes unchanged. */
        physical_es3_queries++;
        return;
    case GL_FRAMEBUFFER_BINDING:
        *value = 0;
        return;
    default:
        return;
    }
}

static void facade_get_integerv(unsigned pname, int *value)
{
    if (!nx_gles3_frame_proof_state_query(pname, value, 0, 0))
        physical_gles2_get_integerv(pname, value);
}

static const unsigned char *facade_get_string(unsigned pname)
{
    if (pname == GL_RENDERER)
        return (const unsigned char *)"Mali-450 facade fixture";
    if (pname == GL_VERSION)
        return (const unsigned char *)"OpenGL ES 3.0 (facade over GLES2)";
    if (pname == GL_EXTENSIONS)
        return (const unsigned char *)"";
    return NULL;
}

static unsigned facade_get_error(void)
{
    return 0;
}

static void physical_gles2_read_pixels(
        int x, int y, int width, int height,
        unsigned format, unsigned type, void *destination)
{
    unsigned char *pixels = (unsigned char *)destination;
    (void)x;
    (void)y;
    (void)format;
    (void)type;
    read_calls++;
    for (int i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = 80;
        pixels[i * 4 + 1] = 40;
        pixels[i * 4 + 2] = 20;
        pixels[i * 4 + 3] = 255;
    }
}

static void *facade_resolver(const char *name)
{
    if (strcmp(name, "glGetIntegerv") == 0)
        return (void *)facade_get_integerv;
    if (strcmp(name, "glGetString") == 0)
        return (void *)facade_get_string;
    if (strcmp(name, "glGetError") == 0)
        return (void *)facade_get_error;
    if (strcmp(name, "glReadPixels") == 0)
        return (void *)physical_gles2_read_pixels;
    return NULL;
}

int main(void)
{
    nxgl_frame_proof_set_resolver(facade_resolver);
    nxgl_frame_proof_launch_receipt();
    nxgl_frame_proof_set_video_context(
        64, 48, "mali", "Mali-450 facade fixture",
        "OpenGL ES 3.0 (facade over GLES2)");
    /* 0.2.16: a compositor resize after registration updates only the
     * recorded window; invalid sizes are ignored and the strings survive. */
    nxgl_frame_proof_set_video_size(0, 96);
    nxgl_frame_proof_set_video_size(128, 96);
    nxgl_frame_proof_sample_at(128, 96, NXGL_PROOF_BEFORE_PRESENT);
    nxgl_frame_proof_publish();

    printf("MALI450 FRAME-PROOF E2E: physical_es3_queries=%u reads=%u\n",
           physical_es3_queries, read_calls);
    return physical_es3_queries == 0 && read_calls == 1 ? 0 : 1;
}
