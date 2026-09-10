/* SPDX-License-Identifier: GPL-3.0-only
 * Regression for logical GLES3 object deletion on the physical GLES2 route.
 * A deleted PBO/FBO must not remain visible to the pre-present frame proof.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../src/shim/nx_shim.h"
#include "../../src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.h"

static unsigned read_calls;
static unsigned physical_es3_queries;

static void fake_bind_framebuffer(GLenum target, GLuint framebuffer)
{
    (void)target;
    (void)framebuffer;
}

static void fake_delete_framebuffers(GLsizei count, const GLuint *ids)
{
    (void)count;
    (void)ids;
}

static void fake_delete_buffers(GLsizei count, const GLuint *ids)
{
    (void)count;
    (void)ids;
}

static void fake_get_integerv(GLenum pname, GLint *value)
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
        physical_es3_queries++;
        return;
    case GL_FRAMEBUFFER_BINDING:
        *value = 0;
        return;
    default:
        *value = 0;
        return;
    }
}

static const GLubyte *fake_get_string(GLenum pname)
{
    if (pname == GL_RENDERER)
        return (const GLubyte *)"Mali-450 delete fixture";
    if (pname == GL_VERSION)
        return (const GLubyte *)"OpenGL ES 2.0 fixture";
    if (pname == GL_EXTENSIONS)
        return (const GLubyte *)"";
    return NULL;
}

static GLenum fake_get_error(void)
{
    return GL_NO_ERROR;
}

static void fake_read_pixels(
        GLint x, GLint y, GLsizei width, GLsizei height,
        GLenum format, GLenum type, void *destination)
{
    unsigned char *pixels = destination;
    (void)x;
    (void)y;
    (void)format;
    (void)type;
    read_calls++;
    for (int i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = 96;
        pixels[i * 4 + 1] = 48;
        pixels[i * 4 + 2] = 24;
        pixels[i * 4 + 3] = 255;
    }
}

void *nx_mali_sym(const char *name)
{
    if (strcmp(name, "glBindFramebuffer") == 0)
        return (void *)fake_bind_framebuffer;
    if (strcmp(name, "glDeleteFramebuffers") == 0)
        return (void *)fake_delete_framebuffers;
    if (strcmp(name, "glDeleteBuffers") == 0)
        return (void *)fake_delete_buffers;
    if (strcmp(name, "glGetIntegerv") == 0)
        return (void *)fake_get_integerv;
    if (strcmp(name, "glGetString") == 0)
        return (void *)fake_get_string;
    if (strcmp(name, "glGetError") == 0)
        return (void *)fake_get_error;
    return NULL;
}

void *nx_mali_egl_sym(const char *name)
{
    (void)name;
    return NULL;
}

void nx_log(const char *format, ...)
{
    (void)format;
}

static void *facade_resolver(const char *name)
{
    if (strcmp(name, "glGetIntegerv") == 0)
        return (void *)glGetIntegerv;
    if (strcmp(name, "glGetString") == 0)
        return (void *)glGetString;
    if (strcmp(name, "glGetError") == 0)
        return (void *)glGetError;
    if (strcmp(name, "glReadPixels") == 0)
        return (void *)fake_read_pixels;
    return NULL;
}

int main(void)
{
    GLuint pbo = 41;
    GLuint fbo = 73;
    GLint value = -1;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value);
    if (value != (GLint)pbo)
        return 1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
    if (value != (GLint)fbo)
        return 2;

    glDeleteBuffers(1, &pbo);
    glDeleteFramebuffers(1, &fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value);
    if (value != 0)
        return 3;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
    if (value != 0)
        return 4;

    nxgl_frame_proof_set_resolver(facade_resolver);
    nxgl_frame_proof_launch_receipt();
    nxgl_frame_proof_set_video_context(
            64, 48, "mali", "Mali-450 delete fixture",
            "OpenGL ES 3.0 facade over GLES2");
    nxgl_frame_proof_sample_at(64, 48, NXGL_PROOF_BEFORE_PRESENT);
    nxgl_frame_proof_publish();

    printf("MALI450 DELETE-UNBIND E2E: reads=%u physical_es3_queries=%u\n",
           read_calls, physical_es3_queries);
    return read_calls == 1 && physical_es3_queries == 0 ? 0 : 5;
}
