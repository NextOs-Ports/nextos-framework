/*
 * gles3.c -- the GLES 3.x surface Unity 6 expects, implemented on GLES 2.0.
 *
 * Unity 6 removed the GLES2 graphics device from the engine, so on a Mali-450
 * (Utgard, GLES 2.0 only) the only way in is to let Unity create its GLES3
 * device and answer every ES3 entry point ourselves.  Unity resolves them
 * through eglGetProcAddress, which the port already owns, so nothing in
 * libunity has to be patched to route the calls here.
 *
 * The list this file covers was MEASURED, not guessed: st_gl_sym() logs every
 * unresolved gl* name (00-recon/gles3-missing.txt, 112 entries).  Before that
 * logging existed the failure was a bare pc=0 with no clue which call it was.
 *
 * Three tiers:
 *   1. Real work -- the driver has an extension that does the job (VAO,
 *      mapbuffer, discard_framebuffer) or the call decomposes into ES2 calls
 *      (TexStorage2D, instanced draws, ProgramUniform*).
 *   2. Honest no-ops -- things a single-render-target 2D game never needs
 *      (draw buffers, debug groups, multisample storage).
 *   3. A catch-all so an unimplemented ES3 call returns 0 instead of being a
 *      NULL pointer Unity jumps into.  It names itself once in the log; that
 *      line is the to-do list for the next pass.
 *
 * Still missing on purpose: std140 uniform block emulation and a real
 * glBlitFramebuffer path.  Neither is enabled from an unverified port; each
 * addition must be justified by this game's measured runtime calls and then
 * proven on the target device.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "nx_elf.h"
#include "gb.h"

extern void *st_gl_raw_sym(const char *name);

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef char GLchar;
typedef ptrdiff_t GLintptr, GLsizeiptr;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;

extern GLuint st_gles3_draw_framebuffer(void);
extern GLuint st_gles3_read_framebuffer(void);
extern void st_gles3_texture_storage_2d(GLenum target, GLsizei levels,
                                        GLenum internal, GLsizei width,
                                        GLsizei height);

#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02
#define GL_EXTENSIONS            0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS        0x821D
#define GL_MAJOR_VERSION         0x821B
#define GL_MINOR_VERSION         0x821C
#define GL_WRITE_ONLY_OES        0x88B9
#define GL_TEXTURE_2D            0x0DE1
#define GL_TEXTURE_3D            0x806F
#define GL_TEXTURE_2D_ARRAY      0x8C1A
#define GL_TEXTURE_CUBE_MAP      0x8513
#define GL_TEXTURE0              0x84C0
#define GL_ACTIVE_TEXTURE        0x84E0
#define GL_TEXTURE_BINDING_2D    0x8069
#define GL_TEXTURE_BINDING_3D    0x806A
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_UNSIGNED_BYTE         0x1401
#define GL_INVALID_INDEX         0xFFFFFFFFu
#define GL_COLOR                 0x1800
#define GL_DEPTH                 0x1801
#define GL_STENCIL               0x1802
#define GL_DEPTH_STENCIL         0x84F9
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_DEPTH_BUFFER_BIT      0x00000100
#define GL_STENCIL_BUFFER_BIT    0x00000400
#define GL_COLOR_CLEAR_VALUE     0x0C22
#define GL_DEPTH_CLEAR_VALUE     0x0B73
#define GL_STENCIL_CLEAR_VALUE   0x0B91
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA

/* ES3 capability queries that must describe the bridge rather than the ES2
 * driver underneath it.  In particular, advertising a GLES3 renderer while
 * forwarding the UBO limits to Utgard leaves Unity reading uninitialised
 * output: those enums do not exist in GLES2. */
#define GL_MAX_VERTEX_UNIFORM_VECTORS          0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS        0x8DFD
#define GL_MAX_VARYING_VECTORS                 0x8DFC
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS       0x8B4A
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS     0x8B49
#define GL_MAX_VARYING_COMPONENTS              0x8B4B
#define GL_MAX_VERTEX_OUTPUT_COMPONENTS        0x9122
#define GL_MAX_FRAGMENT_INPUT_COMPONENTS       0x9125
#define GL_MAX_DRAW_BUFFERS                    0x8824
#define GL_MAX_COLOR_ATTACHMENTS               0x8CDF
#define GL_MAX_SAMPLES                         0x8D57
#define GL_MAX_3D_TEXTURE_SIZE                 0x8073
#define GL_MAX_ARRAY_TEXTURE_LAYERS            0x88FF
#define GL_NUM_PROGRAM_BINARY_FORMATS          0x87FE
#define GL_MAX_VERTEX_UNIFORM_BLOCKS           0x8A2B
#define GL_MAX_FRAGMENT_UNIFORM_BLOCKS         0x8A2D
#define GL_MAX_COMBINED_UNIFORM_BLOCKS         0x8A2E
#define GL_MAX_UNIFORM_BUFFER_BINDINGS         0x8A2F
#define GL_MAX_UNIFORM_BLOCK_SIZE              0x8A30
#define GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS   0x8A31
#define GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS 0x8A33
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT     0x8A34

/* Program/uniform reflection. */
#define GL_ACTIVE_UNIFORMS                     0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH           0x8B87
#define GL_UNIFORM_TYPE                        0x8A37
#define GL_UNIFORM_SIZE                        0x8A38
#define GL_UNIFORM_NAME_LENGTH                 0x8A39
#define GL_UNIFORM_BLOCK_INDEX                 0x8A3A
#define GL_UNIFORM_OFFSET                      0x8A3B
#define GL_UNIFORM_ARRAY_STRIDE                0x8A3C
#define GL_UNIFORM_MATRIX_STRIDE               0x8A3D
#define GL_UNIFORM_IS_ROW_MAJOR                0x8A3E
#define GL_UNIFORM_BLOCK_BINDING               0x8A3F
#define GL_UNIFORM_BLOCK_DATA_SIZE             0x8A40
#define GL_UNIFORM_BLOCK_NAME_LENGTH           0x8A41
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS       0x8A42
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES 0x8A43
#define GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER   0x8A44
#define GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER 0x8A46
#define GL_ATOMIC_COUNTER_BUFFER_INDEX         0x9301

#define GL_UNIFORM                             0x92E1
#define GL_UNIFORM_BLOCK                       0x92E2
#define GL_ACTIVE_RESOURCES                    0x92F5
#define GL_MAX_NAME_LENGTH                     0x92F6
#define GL_MAX_NUM_ACTIVE_VARIABLES            0x92F7
#define GL_NAME_LENGTH                         0x92F9
#define GL_TYPE                                0x92FA
#define GL_ARRAY_SIZE                          0x92FB
#define GL_OFFSET                              0x92FC
#define GL_BLOCK_INDEX                         0x92FD
#define GL_ARRAY_STRIDE                        0x92FE
#define GL_MATRIX_STRIDE                       0x92FF
#define GL_IS_ROW_MAJOR                        0x9300
#define GL_LOCATION                            0x930E
#define GL_REFERENCED_BY_VERTEX_SHADER         0x9306
#define GL_REFERENCED_BY_FRAGMENT_SHADER       0x930A

#define GL_NUM_SAMPLE_COUNTS                   0x9380
#define GL_SAMPLES                             0x80A9

/* Resolved lazily from the driver the port already opened. */
static void *drv(const char *name)
{
    return st_gl_raw_sym(name);
}

static int gl3_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("ST_GLES3_TRACE");
        enabled = value && *value && strcmp(value, "0") != 0;
    }
    return enabled;
}

static void gl3_trace_query(const char *call, GLuint object, GLenum pname,
                            GLint value)
{
    if (gl3_trace_enabled())
        fprintf(stderr, "[st/gles3] %s object=%u pname=0x%x -> %d\n",
                call, object, pname, value);
}

#define D(var, name, ret, args)                                              \
    static ret (*var) args;                                                  \
    if (!var)                                                                \
        var = drv(name);

/* ---------------------------------------------------------------- VAO ----
 * Utgard ships GL_OES_vertex_array_object, so this is a straight rename. */
static void gl3_GenVertexArrays(GLsizei n, GLuint *a)
{
    D(f, "glGenVertexArraysOES", void, (GLsizei, GLuint *))
    if (f) f(n, a);
    else if (a) memset(a, 0, (size_t)n * sizeof *a);
}
static void gl3_BindVertexArray(GLuint a)
{
    D(f, "glBindVertexArrayOES", void, (GLuint))
    if (f) f(a);
}
static void gl3_DeleteVertexArrays(GLsizei n, const GLuint *a)
{
    D(f, "glDeleteVertexArraysOES", void, (GLsizei, const GLuint *))
    if (f) f(n, a);
}
static GLboolean gl3_IsVertexArray(GLuint a)
{
    D(f, "glIsVertexArrayOES", GLboolean, (GLuint))
    return f ? f(a) : 0;
}

/* ------------------------------------------------------- buffer mapping ----
 * GL_OES_mapbuffer maps the WHOLE buffer; a range map is that plus an offset.
 * Unity only ever writes through these, so the access bits are dropped. */
static void *gl3_MapBufferRange(GLenum target, GLintptr offset,
                                GLsizeiptr length, GLbitfield access)
{
    (void)length; (void)access;
    D(f, "glMapBufferOES", void *, (GLenum, GLenum))
    if (!f)
        return NULL;
    char *p = f(target, GL_WRITE_ONLY_OES);
    return p ? p + offset : NULL;
}
static GLboolean gl3_UnmapBuffer(GLenum target)
{
    D(f, "glUnmapBufferOES", GLboolean, (GLenum))
    return f ? f(target) : 1;
}
static void gl3_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l)
{
    (void)t; (void)o; (void)l;   /* OES_mapbuffer has no partial flush */
}

/* ------------------------------------------------------------ textures ----
 * Mali exposes real 3D textures through GL_OES_texture_3D. It has no array
 * textures, but Merchant of the Skies's measured runtime shader set has no sampler2DArray.
 * Unity still creates a one-layer built-in fallback array during device setup;
 * collapse that object to a normal 2D texture so the engine's native resource
 * flow can complete without pretending that arbitrary arrays work. */
GLenum st_gles3_texture_target(GLenum target)
{
    return target == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_2D : target;
}

GLenum st_gles3_texture_format(GLenum format)
{
    switch (format) {
    case 0x8058: /* GL_RGBA8 */
    case 0x8C43: /* GL_SRGB8_ALPHA8 */
        return 0x1908; /* GL_RGBA */
    case 0x8051: /* GL_RGB8 */
    case 0x8C41: /* GL_SRGB8 */
        return 0x1907; /* GL_RGB */
    case 0x8229: /* GL_R8 */
    case 0x1903: /* GL_RED */
        /* GL_LUMINANCE would be the obvious mapping, but it samples as
         * (L,L,L,1.0): the alpha channel becomes a CONSTANT.  Unity uploads
         * Alpha8 assets as R8 and relies on an ES3 texture swizzle to read
         * them back through .a, which ES2 does not have.  Merchant's
         * TextMeshPro SDF shader reads the glyph distance with
         * texture2D(_MainTex, uv).w and discards below a threshold, so a
         * constant alpha never discards and every glyph renders as a solid
         * filled quad.  GL_LUMINANCE_ALPHA with the byte duplicated puts the
         * value in BOTH .r and .a, which satisfies shaders that read either.
         * The upload path expands the pixels to match. */
        return 0x190A; /* GL_LUMINANCE_ALPHA */
    case 0x822B: /* GL_RG8 */
    case 0x8227: /* GL_RG */
        return 0x190A; /* GL_LUMINANCE_ALPHA */
    default:
        return format;
    }
}

static void gl3_TexImage3D(GLenum target, GLint level, GLint internal,
                           GLsizei width, GLsizei height, GLsizei depth,
                           GLint border, GLenum format, GLenum type,
                           const void *pixels)
{
    GLenum physical = st_gles3_texture_target(target);
    GLenum es2_format = st_gles3_texture_format(format);
    GLenum es2_internal = st_gles3_texture_format(internal);
    if (target == GL_TEXTURE_2D_ARRAY) {
        D(image_2d, "glTexImage2D", void,
          (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
           const void *))
        if (image_2d && depth > 0)
            image_2d(physical, level, (GLint)es2_internal, width, height,
                     border, es2_format, type, pixels);
        return;
    }
    D(image_3d, "glTexImage3DOES", void,
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum,
       GLenum, const void *))
    if (image_3d)
        image_3d(physical, level, (GLint)es2_internal, width, height, depth,
                 border, es2_format, type, pixels);
}

static void gl3_TexSubImage3D(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLint zoffset, GLsizei width,
                              GLsizei height, GLsizei depth, GLenum format,
                              GLenum type, const void *pixels)
{
    GLenum physical = st_gles3_texture_target(target);
    GLenum es2_format = st_gles3_texture_format(format);
    if (target == GL_TEXTURE_2D_ARRAY) {
        D(image_2d, "glTexSubImage2D", void,
          (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
           const void *))
        if (image_2d && zoffset == 0 && depth > 0)
            image_2d(physical, level, xoffset, yoffset, width, height,
                     es2_format, type, pixels);
        return;
    }
    D(image_3d, "glTexSubImage3DOES", void,
      (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
       GLenum, GLenum, const void *))
    if (image_3d)
        image_3d(physical, level, xoffset, yoffset, zoffset, width, height,
                 depth, es2_format, type, pixels);
}

static void gl3_CompressedTexImage3D(GLenum target, GLint level,
                                     GLenum internal, GLsizei width,
                                     GLsizei height, GLsizei depth,
                                     GLint border, GLsizei image_size,
                                     const void *pixels)
{
    if (target == GL_TEXTURE_2D_ARRAY) {
        D(image_2d, "glCompressedTexImage2D", void,
          (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
           const void *))
        if (image_2d && depth == 1)
            image_2d(GL_TEXTURE_2D, level, internal, width, height, border,
                     image_size, pixels);
        return;
    }
    D(image_3d, "glCompressedTexImage3DOES", void,
      (GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei,
       const void *))
    if (image_3d)
        image_3d(target, level, internal, width, height, depth, border,
                 image_size, pixels);
}

static void gl3_CompressedTexSubImage3D(GLenum target, GLint level,
                                        GLint xoffset, GLint yoffset,
                                        GLint zoffset, GLsizei width,
                                        GLsizei height, GLsizei depth,
                                        GLenum format, GLsizei image_size,
                                        const void *pixels)
{
    if (target == GL_TEXTURE_2D_ARRAY) {
        D(image_2d, "glCompressedTexSubImage2D", void,
          (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei,
           const void *))
        if (image_2d && zoffset == 0 && depth == 1)
            image_2d(GL_TEXTURE_2D, level, xoffset, yoffset, width, height,
                     format, image_size, pixels);
        return;
    }
    D(image_3d, "glCompressedTexSubImage3DOES", void,
      (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
       GLenum, GLsizei, const void *))
    if (image_3d)
        image_3d(target, level, xoffset, yoffset, zoffset, width, height,
                 depth, format, image_size, pixels);
}

/* Immutable storage does not exist in ES2: allocate the mip chain by hand. */
static void gl3_TexStorage2D(GLenum target, GLsizei levels, GLenum internal,
                             GLsizei w, GLsizei h)
{
    D(f, "glTexImage2D", void,
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
       const void *))
    if (!f)
        return;
    /* ES2 wants an unsized internal format equal to the format. */
    GLenum physical = st_gles3_texture_target(target);
    GLenum fmt = st_gles3_texture_format(internal);
    if (fmt == internal)
        fmt = 0x1908; /* unknown sized format: conservative RGBA */
    st_gles3_texture_storage_2d(target, levels, internal, w, h);
    for (GLsizei l = 0; l < levels; l++) {
        f(physical, l, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, NULL);
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }
}

static void gl3_TexStorage3D(GLenum target, GLsizei levels, GLenum internal,
                             GLsizei width, GLsizei height, GLsizei depth)
{
    GLenum format = st_gles3_texture_format(internal);
    if (format == internal)
        format = 0x1908;
    for (GLsizei level = 0; level < levels; level++) {
        gl3_TexImage3D(target, level, (GLint)format, width, height, depth, 0,
                       format, GL_UNSIGNED_BYTE, NULL);
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
        if (target == GL_TEXTURE_3D && depth > 1) depth >>= 1;
    }
}

static void gl3_FramebufferTextureLayer(GLenum target, GLenum attachment,
                                        GLuint texture, GLint level,
                                        GLint layer)
{
    if (layer != 0)
        return;
    D(f, "glFramebufferTexture2D", void,
      (GLenum, GLenum, GLenum, GLuint, GLint))
    if (f)
        f(target, attachment, GL_TEXTURE_2D, texture, level);
}

/* -------------------------------------------------------- framebuffers ----
 * GL_EXT_discard_framebuffer is the same operation under the old name. */
static void gl3_InvalidateFramebuffer(GLenum target, GLsizei n,
                                      const GLenum *att)
{
    D(f, "glDiscardFramebufferEXT", void, (GLenum, GLsizei, const GLenum *))
    if (f) f(target, n, att);
}
/* One render target only: selecting it is the only valid choice. */
static void gl3_DrawBuffers(GLsizei n, const GLenum *b) { (void)n; (void)b; }
static void gl3_ReadBuffer(GLenum b) { (void)b; }

/* Typed diagnostic entry point while the real ES3 read->draw framebuffer copy
 * is being implemented.  The old generic fallback discarded all arguments,
 * so a physical trace could not even prove whether Unity reached this path. */
static void gl3_BlitFramebuffer(GLint src_x0, GLint src_y0,
                                GLint src_x1, GLint src_y1,
                                GLint dst_x0, GLint dst_y0,
                                GLint dst_x1, GLint dst_y1,
                                GLbitfield mask, GLenum filter)
{
    static unsigned long calls;
    calls++;
    if (gl3_trace_enabled() && calls <= 64)
        fprintf(stderr,
                "[st/gles3] blit #%lu src=%d,%d..%d,%d "
                "dst=%d,%d..%d,%d mask=%#x filter=%#x\n",
                calls, src_x0, src_y0, src_x1, src_y1,
                dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

/* ES3 clear-buffer calls select the attachment directly.  Merchant of the Skies has one
 * color target, so temporarily set the corresponding ES2 clear value, clear,
 * and restore it. */
static void gl3_ClearBufferfv(GLenum buffer, GLint drawbuffer,
                              const GLfloat *value)
{
    if (!value || drawbuffer != 0)
        return;
    D(get_float, "glGetFloatv", void, (GLenum, GLfloat *))
    D(clear, "glClear", void, (GLbitfield))
    if (!get_float || !clear)
        return;
    if (buffer == GL_COLOR) {
        D(clear_color, "glClearColor", void,
          (GLfloat, GLfloat, GLfloat, GLfloat))
        if (!clear_color) return;
        GLfloat old[4] = { 0, 0, 0, 0 };
        get_float(GL_COLOR_CLEAR_VALUE, old);
        clear_color(value[0], value[1], value[2], value[3]);
        clear(GL_COLOR_BUFFER_BIT);
        clear_color(old[0], old[1], old[2], old[3]);
    } else if (buffer == GL_DEPTH) {
        D(clear_depth, "glClearDepthf", void, (GLfloat))
        if (!clear_depth) return;
        GLfloat old = 1.0f;
        get_float(GL_DEPTH_CLEAR_VALUE, &old);
        clear_depth(value[0]);
        clear(GL_DEPTH_BUFFER_BIT);
        clear_depth(old);
    }
}

static void gl3_ClearBufferiv(GLenum buffer, GLint drawbuffer,
                              const GLint *value)
{
    if (!value || drawbuffer != 0)
        return;
    if (buffer == GL_STENCIL) {
        D(get_integer, "glGetIntegerv", void, (GLenum, GLint *))
        D(clear_stencil, "glClearStencil", void, (GLint))
        D(clear, "glClear", void, (GLbitfield))
        if (!get_integer || !clear_stencil || !clear) return;
        GLint old = 0;
        get_integer(GL_STENCIL_CLEAR_VALUE, &old);
        clear_stencil(value[0]);
        clear(GL_STENCIL_BUFFER_BIT);
        clear_stencil(old);
    } else if (buffer == GL_COLOR) {
        GLfloat converted[4] = {
            (GLfloat)value[0], (GLfloat)value[1],
            (GLfloat)value[2], (GLfloat)value[3]
        };
        gl3_ClearBufferfv(buffer, drawbuffer, converted);
    }
}

static void gl3_ClearBufferuiv(GLenum buffer, GLint drawbuffer,
                               const GLuint *value)
{
    if (!value || drawbuffer != 0)
        return;
    GLfloat converted[4] = {
        (GLfloat)value[0], (GLfloat)value[1],
        (GLfloat)value[2], (GLfloat)value[3]
    };
    gl3_ClearBufferfv(buffer, drawbuffer, converted);
}

static void gl3_ClearBufferfi(GLenum buffer, GLint drawbuffer,
                              GLfloat depth, GLint stencil)
{
    if (buffer != GL_DEPTH_STENCIL || drawbuffer != 0)
        return;
    D(get_float, "glGetFloatv", void, (GLenum, GLfloat *))
    D(get_integer, "glGetIntegerv", void, (GLenum, GLint *))
    D(clear_depth, "glClearDepthf", void, (GLfloat))
    D(clear_stencil, "glClearStencil", void, (GLint))
    D(clear, "glClear", void, (GLbitfield))
    if (!get_float || !get_integer || !clear_depth || !clear_stencil || !clear)
        return;
    GLfloat old_depth = 1.0f;
    GLint old_stencil = 0;
    get_float(GL_DEPTH_CLEAR_VALUE, &old_depth);
    get_integer(GL_STENCIL_CLEAR_VALUE, &old_stencil);
    clear_depth(depth);
    clear_stencil(stencil);
    clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    clear_depth(old_depth);
    clear_stencil(old_stencil);
}

static void gl3_ColorMaski(GLuint index, GLboolean red, GLboolean green,
                           GLboolean blue, GLboolean alpha)
{
    if (index != 0)
        return;
    D(f, "glColorMask", void,
      (GLboolean, GLboolean, GLboolean, GLboolean))
    if (f) f(red, green, blue, alpha);
}

static void gl3_BlendEquationi(GLuint index, GLenum mode)
{
    if (index != 0) return;
    D(f, "glBlendEquation", void, (GLenum))
    if (f) f(mode);
}

static void gl3_BlendEquationSeparatei(GLuint index, GLenum rgb, GLenum alpha)
{
    if (index != 0) return;
    D(f, "glBlendEquationSeparate", void, (GLenum, GLenum))
    if (f) f(rgb, alpha);
}

static void gl3_BlendFuncSeparatei(GLuint index, GLenum src_rgb,
                                   GLenum dst_rgb, GLenum src_alpha,
                                   GLenum dst_alpha)
{
    if (index != 0) return;
    D(f, "glBlendFuncSeparate", void,
      (GLenum, GLenum, GLenum, GLenum))
    if (f) f(src_rgb, dst_rgb, src_alpha, dst_alpha);
}

static void gl3_BlendBarrier(void) { }

static void gl3_RenderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                GLenum internal,
                                                GLsizei width, GLsizei height)
{
    (void)samples;
    D(f, "glRenderbufferStorage", void,
      (GLenum, GLenum, GLsizei, GLsizei))
    if (f) f(target, internal, width, height);
}

static void gl3_FramebufferTexture(GLenum target, GLenum attachment,
                                   GLuint texture, GLint level)
{
    D(f, "glFramebufferTexture2D", void,
      (GLenum, GLenum, GLenum, GLuint, GLint))
    if (f) f(target, attachment, GL_TEXTURE_2D, texture, level);
}

/* ------------------------------------------------------------- samplers --
 * ES2 stores sampling parameters on texture objects while ES3 can bind a
 * separate sampler per texture unit.  Preserve Unity's sampler objects and
 * materialise their explicitly-set parameters onto the currently-bound ES2
 * texture whenever either side of that pair changes. */
#define GL3_MAX_SAMPLERS 256
#define GL3_MAX_TEXTURE_UNITS 32
#define GL3_MAX_SAMPLER_PARAMS 16

typedef struct {
    GLenum pname;
    GLint value;
} gl3_sampler_param;

typedef struct {
    int alive;
    int count;
    gl3_sampler_param params[GL3_MAX_SAMPLER_PARAMS];
} gl3_sampler;

static gl3_sampler gl3_samplers[GL3_MAX_SAMPLERS];
static GLuint gl3_bound_samplers[GL3_MAX_TEXTURE_UNITS];

static void gl3_apply_sampler(GLuint unit, GLenum target)
{
    if (unit >= GL3_MAX_TEXTURE_UNITS)
        return;
    GLuint id = gl3_bound_samplers[unit];
    if (id == 0 || id >= GL3_MAX_SAMPLERS || !gl3_samplers[id].alive)
        return;
    D(tex_parameter, "glTexParameteri", void, (GLenum, GLenum, GLint))
    if (!tex_parameter)
        return;
    gl3_sampler *sampler = &gl3_samplers[id];
    for (int i = 0; i < sampler->count; i++)
        tex_parameter(target, sampler->params[i].pname,
                      sampler->params[i].value);
}

/* Called by egl.c after the real glBindTexture. */
void st_gles3_texture_bound(GLenum active_texture, GLenum target,
                            GLuint texture)
{
    if (active_texture < GL_TEXTURE0 || texture == 0)
        return;
    GLuint unit = active_texture - GL_TEXTURE0;
    gl3_apply_sampler(unit, target);
}

static void gl3_GenSamplers(GLsizei count, GLuint *ids)
{
    if (!ids || count <= 0)
        return;
    for (GLsizei i = 0; i < count; i++) {
        ids[i] = 0;
        for (GLuint id = 1; id < GL3_MAX_SAMPLERS; id++) {
            if (gl3_samplers[id].alive)
                continue;
            memset(&gl3_samplers[id], 0, sizeof gl3_samplers[id]);
            gl3_samplers[id].alive = 1;
            ids[i] = id;
            break;
        }
    }
}

static void gl3_DeleteSamplers(GLsizei count, const GLuint *ids)
{
    if (!ids || count <= 0)
        return;
    for (GLsizei i = 0; i < count; i++) {
        GLuint id = ids[i];
        if (id == 0 || id >= GL3_MAX_SAMPLERS)
            continue;
        memset(&gl3_samplers[id], 0, sizeof gl3_samplers[id]);
        for (GLuint unit = 0; unit < GL3_MAX_TEXTURE_UNITS; unit++)
            if (gl3_bound_samplers[unit] == id)
                gl3_bound_samplers[unit] = 0;
    }
}

static void gl3_SamplerParameteri(GLuint id, GLenum pname, GLint value)
{
    if (id == 0 || id >= GL3_MAX_SAMPLERS || !gl3_samplers[id].alive)
        return;
    gl3_sampler *sampler = &gl3_samplers[id];
    for (int i = 0; i < sampler->count; i++) {
        if (sampler->params[i].pname == pname) {
            sampler->params[i].value = value;
            return;
        }
    }
    if (sampler->count < GL3_MAX_SAMPLER_PARAMS) {
        sampler->params[sampler->count].pname = pname;
        sampler->params[sampler->count].value = value;
        sampler->count++;
    }
}

static void gl3_BindSampler(GLuint unit, GLuint id)
{
    if (unit >= GL3_MAX_TEXTURE_UNITS ||
        (id != 0 && (id >= GL3_MAX_SAMPLERS || !gl3_samplers[id].alive)))
        return;
    gl3_bound_samplers[unit] = id;
    if (id == 0)
        return;

    D(get_integer, "glGetIntegerv", void, (GLenum, GLint *))
    D(active_texture, "glActiveTexture", void, (GLenum))
    if (!get_integer || !active_texture)
        return;
    GLint previous = GL_TEXTURE0;
    GLint texture_2d = 0, texture_cube = 0;
    get_integer(GL_ACTIVE_TEXTURE, &previous);
    active_texture(GL_TEXTURE0 + unit);
    get_integer(GL_TEXTURE_BINDING_2D, &texture_2d);
    get_integer(GL_TEXTURE_BINDING_CUBE_MAP, &texture_cube);
    if (texture_2d)
        gl3_apply_sampler(unit, GL_TEXTURE_2D);
    if (texture_cube)
        gl3_apply_sampler(unit, GL_TEXTURE_CUBE_MAP);
    active_texture((GLenum)previous);
}

/* ----------------------------------------------------------- instancing ----
 * Utgard has no instancing extension at all, so replay the draw.  A 2D build
 * uses this for a handful of UI/particle batches; measured instance counts
 * here are small.  Without a divisor the per-instance attributes cannot be
 * advanced, so this is only correct for gl_InstanceID-free shaders -- which is
 * what this game's shader corpus showed (0 uses of gl_InstanceID). */
static void gl3_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
                                    GLsizei primcount)
{
    D(f, "glDrawArrays", void, (GLenum, GLint, GLsizei))
    if (!f) return;
    for (GLsizei i = 0; i < primcount; i++)
        f(mode, first, count);
}
static void gl3_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                      const void *idx, GLsizei primcount)
{
    D(f, "glDrawElements", void, (GLenum, GLsizei, GLenum, const void *))
    if (!f) return;
    for (GLsizei i = 0; i < primcount; i++)
        f(mode, count, type, idx);
}

/* --------------------------------------------------------- enumeration ----
 * ES3 replaced the single GL_EXTENSIONS string with indexed queries.  Unity
 * uses glGetStringi to build its capability table, so answering NULL would
 * make it believe the driver has no extensions at all. */
static const char **ext_list;
static int ext_n;

static void build_ext_list(void)
{
    if (ext_list)
        return;
    D(f, "glGetString", const char *, (GLenum))
    const char *s = f ? f(GL_EXTENSIONS) : NULL;
    if (!s) { ext_n = 0; ext_list = calloc(1, sizeof *ext_list); return; }
    char *copy = strdup(s);
    int cap = 8;
    ext_list = calloc((size_t)cap, sizeof *ext_list);
    for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
        if (ext_n == cap) {
            cap *= 2;
            ext_list = realloc(ext_list, (size_t)cap * sizeof *ext_list);
        }
        ext_list[ext_n++] = tok;
    }
}

static const char *gl3_GetStringi(GLenum name, GLuint index)
{
    if (name != GL_EXTENSIONS)
        return NULL;
    build_ext_list();
    return (int)index < ext_n ? ext_list[index] : NULL;
}

/* This is the single compatibility claim the Unity 6 renderer needs.  Vendor,
 * renderer and extension strings remain the physical Mali driver's own; only
 * the core/GLSL versions describe the API surface implemented by this file. */
static const GLubyte *gl3_GetString(GLenum name)
{
    static const GLubyte version[] =
        "OpenGL ES 3.0 Merchant of the Skies GLES2 bridge";
    static const GLubyte shading[] =
        "OpenGL ES GLSL ES 3.00 Merchant of the Skies GLES2 bridge";
    if (name == GL_VERSION)
        return version;
    if (name == GL_SHADING_LANGUAGE_VERSION)
        return shading;
    D(f, "glGetString", const GLubyte *, (GLenum))
    if (name == GL_VENDOR || name == GL_RENDERER || name == GL_EXTENSIONS)
        return f ? f(name) : NULL;
    return f ? f(name) : NULL;
}

static void gl3_GetIntegeri_v(GLenum target, GLuint index, GLint *data)
{
    (void)index;
    D(f, "glGetIntegerv", void, (GLenum, GLint *))
    if (!data)
        return;
    if (target == GL_MAX_UNIFORM_BUFFER_BINDINGS) {
        *data = 0;
        return;
    }
    if (f) f(target, data);
    else *data = 0;
}

/* Unity's GLES3 device asks the ES3 limit enums even though the physical
 * context is GLES2.  Never pass an unknown ES3 enum to Utgard and then leave
 * Unity's output untouched.  The bridge deliberately exposes no UBOs: all
 * Merchant of the Skies GLES3 shaders carry Unity's loose-uniform branch, which the
 * shader adapter selects before compiling them as ESSL100. */
static GLint gl3_driver_limit(GLenum pname, GLint fallback)
{
    D(f, "glGetIntegerv", void, (GLenum, GLint *))
    GLint value = fallback;
    if (f)
        f(pname, &value);
    return value;
}

static void gl3_GetIntegerv(GLenum pname, GLint *data)
{
    if (!data)
        return;

    GLint value;
    switch (pname) {
    case GL_MAJOR_VERSION: value = 3; break;
    case GL_MINOR_VERSION: value = 0; break;
    case GL_DRAW_FRAMEBUFFER_BINDING:
        value = (GLint)st_gles3_draw_framebuffer();
        break;
    case GL_READ_FRAMEBUFFER_BINDING:
        value = (GLint)st_gles3_read_framebuffer();
        break;
    case GL_NUM_EXTENSIONS:
        build_ext_list();
        value = ext_n;
        break;
    case GL_NUM_PROGRAM_BINARY_FORMATS:
    case GL_MAX_VERTEX_UNIFORM_BLOCKS:
    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
    case GL_MAX_COMBINED_UNIFORM_BLOCKS:
    case GL_MAX_UNIFORM_BUFFER_BINDINGS:
    case GL_MAX_UNIFORM_BLOCK_SIZE:
    case GL_MAX_SAMPLES:
        value = 0;
        break;
    case GL_MAX_3D_TEXTURE_SIZE:
        value = gl3_driver_limit(GL_MAX_3D_TEXTURE_SIZE, 256);
        if (value < 256)
            value = 256;
        break;
    case GL_MAX_ARRAY_TEXTURE_LAYERS:
        /* Only the one-layer native fallback objects are representable. */
        value = 1;
        break;
    case GL_TEXTURE_BINDING_2D_ARRAY:
        value = gl3_driver_limit(GL_TEXTURE_BINDING_2D, 0);
        break;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
        /* Keep defensive callers away from divide-by-zero even though the
         * corresponding binding count is zero. */
        value = 1;
        break;
    case GL_MAX_DRAW_BUFFERS:
    case GL_MAX_COLOR_ATTACHMENTS:
        value = 1;
        break;
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
    case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
        value = 4 * gl3_driver_limit(GL_MAX_VERTEX_UNIFORM_VECTORS, 128);
        break;
    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
    case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
        value = 4 * gl3_driver_limit(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 16);
        break;
    case GL_MAX_VARYING_COMPONENTS:
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS:
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS:
        value = 4 * gl3_driver_limit(GL_MAX_VARYING_VECTORS, 8);
        break;
    default: {
        D(f, "glGetIntegerv", void, (GLenum, GLint *))
        if (f) f(pname, data);
        else *data = 0;
        if (gl3_trace_enabled())
            fprintf(stderr, "[st/gles3] glGetIntegerv passthrough "
                    "pname=0x%x -> %d\n", pname, *data);
        return;
    }
    }
    *data = value;
    gl3_trace_query("glGetIntegerv", 0, pname, value);
}

/* ------------------------------------------------ uniform reflection ----
 * With uniform blocks disabled, ES3's batch reflection still has to describe
 * the loose uniforms exported by the linked ES2 program.  Reconstruct those
 * answers from glGetActiveUniform instead of fabricating a std140 layout. */
static int gl3_active_uniform(GLuint program, GLuint index, GLsizei name_cap,
                              GLsizei *name_len, GLint *size, GLenum *type,
                              GLchar *name)
{
    D(get, "glGetActiveUniform", void,
      (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *))
    if (!get)
        return 0;
    GLsizei local_len = 0;
    GLint local_size = 0;
    GLenum local_type = 0;
    GLchar dummy_name[1] = { '\0' };
    GLchar *owned_name = NULL;
    if (!name) {
        D(get_program, "glGetProgramiv", void, (GLuint, GLenum, GLint *))
        GLint max_len = 0;
        if (get_program)
            get_program(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_len);
        if (max_len > 1 && max_len <= 65536)
            owned_name = malloc((size_t)max_len);
        if (owned_name) {
            name = owned_name;
            name_cap = max_len;
        } else {
            name = dummy_name;
            name_cap = 1;
        }
    }
    get(program, index, name_cap, &local_len, &local_size, &local_type, name);
    if (name_len) *name_len = local_len;
    if (size) *size = local_size;
    if (type) *type = local_type;
    free(owned_name);
    return 1;
}

static GLuint gl3_uniform_index(GLuint program, const GLchar *wanted)
{
    if (!wanted)
        return GL_INVALID_INDEX;
    D(get_program, "glGetProgramiv", void, (GLuint, GLenum, GLint *))
    if (!get_program)
        return GL_INVALID_INDEX;
    GLint count = 0, max_len = 0;
    get_program(program, GL_ACTIVE_UNIFORMS, &count);
    get_program(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_len);
    if (count <= 0)
        return GL_INVALID_INDEX;
    if (max_len < 2) max_len = 256;
    if (max_len > 65536) return GL_INVALID_INDEX;
    char *name = malloc((size_t)max_len);
    if (!name)
        return GL_INVALID_INDEX;
    GLuint result = GL_INVALID_INDEX;
    size_t wanted_len = strlen(wanted);
    for (GLint i = 0; i < count; i++) {
        GLsizei length = 0;
        name[0] = '\0';
        if (!gl3_active_uniform(program, (GLuint)i, max_len, &length,
                                NULL, NULL, name))
            break;
        int same = strcmp(name, wanted) == 0;
        /* GLES reports the first element of an active array as name[0].
         * glGetUniformIndices also accepts the base spelling. */
        if (!same && length >= 3 && strcmp(name + length - 3, "[0]") == 0 &&
            wanted_len == (size_t)length - 3 &&
            memcmp(name, wanted, wanted_len) == 0)
            same = 1;
        if (same) {
            result = (GLuint)i;
            break;
        }
    }
    free(name);
    return result;
}

static GLuint gl3_GetUniformBlockIndex(GLuint program, const GLchar *name)
{
    if (gl3_trace_enabled())
        fprintf(stderr, "[st/gles3] glGetUniformBlockIndex program=%u "
                "name=%s -> GL_INVALID_INDEX\n", program,
                name ? name : "(null)");
    return GL_INVALID_INDEX;
}

static void gl3_GetUniformIndices(GLuint program, GLsizei count,
                                  const GLchar *const *names, GLuint *indices)
{
    if (!indices || count <= 0)
        return;
    for (GLsizei i = 0; i < count; i++)
        indices[i] = gl3_uniform_index(program, names ? names[i] : NULL);
}

static void gl3_GetActiveUniformsiv(GLuint program, GLsizei count,
                                    const GLuint *indices, GLenum pname,
                                    GLint *params)
{
    if (!params || count <= 0)
        return;
    for (GLsizei i = 0; i < count; i++) {
        GLint size = 0;
        GLenum type = 0;
        GLsizei name_len = 0;
        GLuint index = indices ? indices[i] : GL_INVALID_INDEX;
        GLint value = 0;
        if (index != GL_INVALID_INDEX)
            gl3_active_uniform(program, index, 0, &name_len, &size, &type,
                               NULL);
        switch (pname) {
        case GL_UNIFORM_TYPE: value = (GLint)type; break;
        case GL_UNIFORM_SIZE: value = size; break;
        case GL_UNIFORM_NAME_LENGTH: value = name_len + 1; break;
        case GL_UNIFORM_BLOCK_INDEX:
        case GL_UNIFORM_OFFSET:
        case GL_ATOMIC_COUNTER_BUFFER_INDEX: value = -1; break;
        case GL_UNIFORM_ARRAY_STRIDE:
        case GL_UNIFORM_MATRIX_STRIDE:
        case GL_UNIFORM_IS_ROW_MAJOR: value = 0; break;
        default: value = 0; break;
        }
        params[i] = value;
    }
    if (gl3_trace_enabled())
        fprintf(stderr, "[st/gles3] glGetActiveUniformsiv program=%u "
                "count=%d pname=0x%x\n", program, count, pname);
}

static void gl3_GetActiveUniformBlockiv(GLuint program, GLuint block,
                                        GLenum pname, GLint *params)
{
    if (!params)
        return;
    /* There are no valid block indices in the bridge.  A one-byte empty name
     * is the only non-zero size that keeps defensive allocation code safe. */
    *params = pname == GL_UNIFORM_BLOCK_NAME_LENGTH ? 1 : 0;
    gl3_trace_query("glGetActiveUniformBlockiv", program, pname, *params);
    (void)block;
}

static void gl3_GetActiveUniformBlockName(GLuint program, GLuint block,
                                          GLsizei buf_size, GLsizei *length,
                                          GLchar *name)
{
    (void)program; (void)block;
    if (length) *length = 0;
    if (name && buf_size > 0) name[0] = '\0';
}

static void gl3_BindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
    if (gl3_trace_enabled())
        fprintf(stderr, "[st/gles3] ignored glBindBufferBase "
                "target=0x%x index=%u buffer=%u (UBO disabled)\n",
                target, index, buffer);
}

static void gl3_BindBufferRange(GLenum target, GLuint index, GLuint buffer,
                                GLintptr offset, GLsizeiptr size)
{
    if (gl3_trace_enabled())
        fprintf(stderr, "[st/gles3] ignored glBindBufferRange "
                "target=0x%x index=%u buffer=%u offset=%ld size=%ld "
                "(UBO disabled)\n", target, index, buffer, (long)offset,
                (long)size);
}

static void gl3_UniformBlockBinding(GLuint program, GLuint block,
                                    GLuint binding)
{
    if (gl3_trace_enabled())
        fprintf(stderr, "[st/gles3] ignored glUniformBlockBinding "
                "program=%u block=%u binding=%u (UBO disabled)\n",
                program, block, binding);
}

/* Modern program-interface reflection is not needed by Unity's GLES path,
 * but it is resolved eagerly.  Give every output a deterministic value so a
 * diagnostic or optional path can never inherit stack garbage. */
static void gl3_GetProgramInterfaceiv(GLuint program, GLenum interface,
                                      GLenum pname, GLint *params)
{
    if (!params)
        return;
    GLint value = pname == GL_MAX_NAME_LENGTH ? 1 : 0;
    /* Loose uniforms remain discoverable through the classic GLES2 API.
     * Do not claim blocks merely because the renderer facade is GLES3. */
    if (interface == GL_UNIFORM &&
        (pname == GL_ACTIVE_RESOURCES || pname == GL_MAX_NAME_LENGTH)) {
        D(get, "glGetProgramiv", void, (GLuint, GLenum, GLint *))
        if (get)
            get(program, pname == GL_ACTIVE_RESOURCES
                         ? GL_ACTIVE_UNIFORMS : GL_ACTIVE_UNIFORM_MAX_LENGTH,
                &value);
    } else if (interface == GL_UNIFORM_BLOCK) {
        value = pname == GL_MAX_NAME_LENGTH ? 1 : 0;
    } else if (pname == GL_MAX_NUM_ACTIVE_VARIABLES) {
        value = 0;
    }
    *params = value;
    gl3_trace_query("glGetProgramInterfaceiv", program, pname, value);
}

static GLuint gl3_GetProgramResourceIndex(GLuint program, GLenum interface,
                                          const GLchar *name)
{
    if (interface == GL_UNIFORM)
        return gl3_uniform_index(program, name);
    return GL_INVALID_INDEX;
}

static void gl3_GetProgramResourceName(GLuint program, GLenum interface,
                                       GLuint index, GLsizei buf_size,
                                       GLsizei *length, GLchar *name)
{
    if (length) *length = 0;
    if (name && buf_size > 0) name[0] = '\0';
    if (interface == GL_UNIFORM && name && buf_size > 0)
        gl3_active_uniform(program, index, buf_size, length, NULL, NULL, name);
}

static void gl3_GetProgramResourceiv(GLuint program, GLenum interface,
                                     GLuint index, GLsizei prop_count,
                                     const GLenum *props, GLsizei buf_size,
                                     GLsizei *length, GLint *params)
{
    GLsizei written = prop_count < buf_size ? prop_count : buf_size;
    if (written < 0) written = 0;
    GLint size = 0;
    GLenum type = 0;
    GLsizei name_len = 0;
    if (interface == GL_UNIFORM)
        gl3_active_uniform(program, index, 0, &name_len, &size, &type, NULL);
    for (GLsizei i = 0; params && i < written; i++) {
        GLint value = 0;
        switch (props ? props[i] : 0) {
        case GL_NAME_LENGTH: value = name_len + 1; break;
        case GL_TYPE: value = (GLint)type; break;
        case GL_ARRAY_SIZE: value = size; break;
        case GL_BLOCK_INDEX:
        case GL_OFFSET: value = -1; break;
        case GL_ARRAY_STRIDE:
        case GL_MATRIX_STRIDE:
        case GL_IS_ROW_MAJOR: value = 0; break;
        case GL_REFERENCED_BY_VERTEX_SHADER:
        case GL_REFERENCED_BY_FRAGMENT_SHADER:
            value = interface == GL_UNIFORM;
            break;
        case GL_LOCATION: {
            value = -1;
            if (interface == GL_UNIFORM) {
                D(get_location, "glGetUniformLocation", GLint,
                  (GLuint, const GLchar *))
                D(get_program, "glGetProgramiv", void,
                  (GLuint, GLenum, GLint *))
                GLint max_len = 0;
                if (get_location && get_program) {
                    get_program(program, GL_ACTIVE_UNIFORM_MAX_LENGTH,
                                &max_len);
                    if (max_len > 1 && max_len <= 65536) {
                        char *uniform_name = malloc((size_t)max_len);
                        if (uniform_name) {
                            uniform_name[0] = '\0';
                            gl3_active_uniform(program, index, max_len, NULL,
                                               NULL, NULL, uniform_name);
                            value = get_location(program, uniform_name);
                            free(uniform_name);
                        }
                    }
                }
            }
            break;
        }
        default: value = 0; break;
        }
        params[i] = value;
    }
    if (length) *length = written;
}

static void gl3_GetInternalformativ(GLenum target, GLenum internal,
                                    GLenum pname, GLsizei count, GLint *params)
{
    (void)target; (void)internal;
    if (!params || count <= 0)
        return;
    for (GLsizei i = 0; i < count; i++) params[i] = 0;
    if (pname == GL_NUM_SAMPLE_COUNTS || pname == GL_SAMPLES)
        params[0] = 0;
}

static void gl3_GetObjectLabel(GLenum identifier, GLuint name, GLsizei buf_size,
                               GLsizei *length, GLchar *label)
{
    (void)identifier; (void)name;
    if (length) *length = 0;
    if (label && buf_size > 0) label[0] = '\0';
}

static void gl3_GetTexLevelParameteriv(GLenum target, GLint level,
                                       GLenum pname, GLint *params)
{
    (void)target; (void)level; (void)pname;
    if (params) *params = 0;
}

static void gl3_GetTexLevelParameterfv(GLenum target, GLint level,
                                       GLenum pname, GLfloat *params)
{
    (void)target; (void)level; (void)pname;
    if (params) *params = 0.0f;
}

/* ------------------------------------------------------ program uniforms ----
 * EXT_separate_shader_objects is absent: bind, set, restore. */
#define GL_CURRENT_PROGRAM 0x8B8D

#define PROGRAM_UNIFORM(suffix, type)                                        \
    static void gl3_ProgramUniform##suffix(GLuint prog, GLint loc,           \
                                           GLsizei count, const type *v)     \
    {                                                                        \
        D(getiv, "glGetIntegerv", void, (GLenum, GLint *))                   \
        D(use, "glUseProgram", void, (GLuint))                               \
        D(set, "glUniform" #suffix, void, (GLint, GLsizei, const type *))    \
        if (!getiv || !use || !set) return;                                  \
        GLint prev = 0;                                                      \
        getiv(GL_CURRENT_PROGRAM, &prev);                                    \
        use(prog);                                                           \
        set(loc, count, v);                                                  \
        use((GLuint)prev);                                                   \
    }

PROGRAM_UNIFORM(1fv, GLfloat)
PROGRAM_UNIFORM(2fv, GLfloat)
PROGRAM_UNIFORM(3fv, GLfloat)
PROGRAM_UNIFORM(4fv, GLfloat)
PROGRAM_UNIFORM(1iv, GLint)
PROGRAM_UNIFORM(2iv, GLint)
PROGRAM_UNIFORM(3iv, GLint)
PROGRAM_UNIFORM(4iv, GLint)

#define PROGRAM_UNIFORM_MATRIX(suffix)                                       \
    static void gl3_ProgramUniformMatrix##suffix(GLuint prog, GLint loc,     \
                                                 GLsizei count,              \
                                                 GLboolean transpose,        \
                                                 const GLfloat *v)           \
    {                                                                        \
        D(getiv, "glGetIntegerv", void, (GLenum, GLint *))                   \
        D(use, "glUseProgram", void, (GLuint))                               \
        D(set, "glUniformMatrix" #suffix, void,                              \
          (GLint, GLsizei, GLboolean, const GLfloat *))                      \
        if (!getiv || !use || !set) return;                                  \
        GLint prev = 0;                                                      \
        getiv(GL_CURRENT_PROGRAM, &prev);                                    \
        use(prog);                                                           \
        set(loc, count, transpose, v);                                       \
        use((GLuint)prev);                                                   \
    }

PROGRAM_UNIFORM_MATRIX(2fv)
PROGRAM_UNIFORM_MATRIX(3fv)
PROGRAM_UNIFORM_MATRIX(4fv)

/* -------------------------------------------------------------- stubs ----
 * Queries, fences and debug output have no ES2 equivalent and nothing in a 2D
 * build depends on their answers.  They must still exist: Unity stores the
 * pointer and calls it. */
static void gl3_GenQueries(GLsizei n, GLuint *ids)
{
    for (GLsizei i = 0; i < n; i++) ids[i] = (GLuint)(i + 1);
}
static void gl3_DeleteQueries(GLsizei n, const GLuint *ids) { (void)n; (void)ids; }
static void gl3_BeginQuery(GLenum t, GLuint id) { (void)t; (void)id; }
static void gl3_EndQuery(GLenum t) { (void)t; }
static void gl3_GetQueryObjectuiv(GLuint id, GLenum p, GLuint *v)
{
    (void)id; (void)p; if (v) *v = 0;
}
static void gl3_GetQueryiv(GLenum t, GLenum p, GLint *v)
{
    (void)t; (void)p; if (v) *v = 0;
}
static void *gl3_FenceSync(GLenum c, GLbitfield f) { (void)c; (void)f; return (void *)1; }
static GLenum gl3_ClientWaitSync(void *s, GLbitfield f, uint64_t t)
{
    (void)s; (void)f; (void)t; return 0x911A /* ALREADY_SIGNALED */;
}
static void gl3_DeleteSync(void *s) { (void)s; }

/* Reporting zero formats keeps Unity from building a program-binary cache --
 * which on this device grows without bound and wedges the process in D state
 * (see reference_unityshadercache_estoura_e_congela_troca_de_cena). */
static void gl3_GetProgramBinary(GLuint p, GLsizei bufsz, GLsizei *len,
                                 GLenum *fmt, void *bin)
{
    (void)p; (void)bufsz; (void)fmt; (void)bin;
    if (len) *len = 0;
}
static void gl3_ProgramBinary(GLuint p, GLenum fmt, const void *bin, GLsizei n)
{
    (void)p; (void)fmt; (void)bin; (void)n;
}

/* ------------------------------------------------------------ dispatch ----*/

static const struct { const char *name; void *fn; } TABLE[] = {
    { "glGetString",              gl3_GetString },
    { "glGetIntegerv",            gl3_GetIntegerv },
    { "glGenVertexArrays",        gl3_GenVertexArrays },
    { "glBindVertexArray",        gl3_BindVertexArray },
    { "glDeleteVertexArrays",     gl3_DeleteVertexArrays },
    { "glIsVertexArray",          gl3_IsVertexArray },
    { "glMapBufferRange",         gl3_MapBufferRange },
    { "glUnmapBuffer",            gl3_UnmapBuffer },
    { "glFlushMappedBufferRange", gl3_FlushMappedBufferRange },
    { "glTexImage3D",             gl3_TexImage3D },
    { "glTexSubImage3D",          gl3_TexSubImage3D },
    { "glCompressedTexImage3D",   gl3_CompressedTexImage3D },
    { "glCompressedTexSubImage3D", gl3_CompressedTexSubImage3D },
    { "glTexStorage2D",           gl3_TexStorage2D },
    { "glTexStorage3D",           gl3_TexStorage3D },
    { "glFramebufferTextureLayer", gl3_FramebufferTextureLayer },
    { "glInvalidateFramebuffer",  gl3_InvalidateFramebuffer },
    { "glDrawBuffers",            gl3_DrawBuffers },
    { "glReadBuffer",             gl3_ReadBuffer },
    { "glBlitFramebuffer",        gl3_BlitFramebuffer },
    { "glClearBufferfv",          gl3_ClearBufferfv },
    { "glClearBufferiv",          gl3_ClearBufferiv },
    { "glClearBufferuiv",         gl3_ClearBufferuiv },
    { "glClearBufferfi",          gl3_ClearBufferfi },
    { "glColorMaski",             gl3_ColorMaski },
    { "glBlendEquationi",         gl3_BlendEquationi },
    { "glBlendEquationSeparatei", gl3_BlendEquationSeparatei },
    { "glBlendFuncSeparatei",     gl3_BlendFuncSeparatei },
    { "glBlendBarrier",           gl3_BlendBarrier },
    { "glRenderbufferStorageMultisample", gl3_RenderbufferStorageMultisample },
    { "glFramebufferTexture",     gl3_FramebufferTexture },
    { "glGenSamplers",            gl3_GenSamplers },
    { "glDeleteSamplers",         gl3_DeleteSamplers },
    { "glBindSampler",            gl3_BindSampler },
    { "glSamplerParameteri",      gl3_SamplerParameteri },
    { "glDrawArraysInstanced",    gl3_DrawArraysInstanced },
    { "glDrawElementsInstanced",  gl3_DrawElementsInstanced },
    { "glGetStringi",             gl3_GetStringi },
    { "glGetIntegeri_v",          gl3_GetIntegeri_v },
    { "glGetUniformBlockIndex",   gl3_GetUniformBlockIndex },
    { "glGetUniformIndices",      gl3_GetUniformIndices },
    { "glGetActiveUniformsiv",    gl3_GetActiveUniformsiv },
    { "glGetActiveUniformBlockiv", gl3_GetActiveUniformBlockiv },
    { "glGetActiveUniformBlockName", gl3_GetActiveUniformBlockName },
    { "glBindBufferBase",         gl3_BindBufferBase },
    { "glBindBufferRange",        gl3_BindBufferRange },
    { "glUniformBlockBinding",    gl3_UniformBlockBinding },
    { "glGetProgramInterfaceiv",  gl3_GetProgramInterfaceiv },
    { "glGetProgramResourceIndex", gl3_GetProgramResourceIndex },
    { "glGetProgramResourceName", gl3_GetProgramResourceName },
    { "glGetProgramResourceiv",   gl3_GetProgramResourceiv },
    { "glGetInternalformativ",    gl3_GetInternalformativ },
    { "glGetObjectLabel",         gl3_GetObjectLabel },
    { "glGetTexLevelParameteriv", gl3_GetTexLevelParameteriv },
    { "glGetTexLevelParameterfv", gl3_GetTexLevelParameterfv },
    { "glGenQueries",             gl3_GenQueries },
    { "glDeleteQueries",          gl3_DeleteQueries },
    { "glBeginQuery",             gl3_BeginQuery },
    { "glEndQuery",               gl3_EndQuery },
    { "glGetQueryObjectuiv",      gl3_GetQueryObjectuiv },
    { "glGetQueryiv",             gl3_GetQueryiv },
    { "glFenceSync",              gl3_FenceSync },
    { "glClientWaitSync",         gl3_ClientWaitSync },
    { "glDeleteSync",             gl3_DeleteSync },
    { "glGetProgramBinary",       gl3_GetProgramBinary },
    { "glProgramBinary",          gl3_ProgramBinary },
    { "glProgramUniform1fv",      gl3_ProgramUniform1fv },
    { "glProgramUniform2fv",      gl3_ProgramUniform2fv },
    { "glProgramUniform3fv",      gl3_ProgramUniform3fv },
    { "glProgramUniform4fv",      gl3_ProgramUniform4fv },
    { "glProgramUniform1iv",      gl3_ProgramUniform1iv },
    { "glProgramUniform2iv",      gl3_ProgramUniform2iv },
    { "glProgramUniform3iv",      gl3_ProgramUniform3iv },
    { "glProgramUniform4iv",      gl3_ProgramUniform4iv },
    { "glProgramUniformMatrix2fv", gl3_ProgramUniformMatrix2fv },
    { "glProgramUniformMatrix3fv", gl3_ProgramUniformMatrix3fv },
    { "glProgramUniformMatrix4fv", gl3_ProgramUniformMatrix4fv },
};

/* Anything ES3 we have not implemented yet: returning 0 is wrong, but it is
 * recoverable and debuggable, while the NULL pointer Unity would otherwise
 * store is an immediate pc=0 with no name attached. */
static long gl3_unimplemented(void) { return 0; }

void *st_gles3_sym(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof TABLE / sizeof *TABLE; i++)
        if (strcmp(TABLE[i].name, name) == 0)
            return TABLE[i].fn;
    return NULL;
}

/* A few functions exist in GLES2 but need bridge-specific answers when called
 * through Unity's GLES3 table.  egl.c asks this before the raw-driver lookup;
 * the normal emulation table remains a fallback for names Utgard lacks. */
void *st_gles3_override_sym(const char *name)
{
    if (name) {
        if (strcmp(name, "glGetIntegerv") == 0)
            return gl3_GetIntegerv;
        if (strcmp(name, "glGetString") == 0)
            return gl3_GetString;
    }
    return NULL;
}

void *st_gles3_fallback(const char *name)
{
    static const char *seen[256];
    static int seen_n;
    int known = 0;
    for (int i = 0; i < seen_n; i++)
        if (strcmp(seen[i], name) == 0) { known = 1; break; }
    if (!known && seen_n < 256) {
        seen[seen_n++] = strdup(name);
        fprintf(stderr, "[st/gl] STUB %s (returns 0)\n", name);
        fflush(stderr);
    }
    return (void *)gl3_unimplemented;
}
