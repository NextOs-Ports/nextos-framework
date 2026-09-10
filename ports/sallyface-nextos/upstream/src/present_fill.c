/*
 * Sally Face/Adventure Creator composes a centered 16:9 picture into its
 * final backbuffer even when Android and EGL coherently advertise 640x480.
 * Viewport interception cannot remove pixels already composed by the game.
 * In fill mode this adapter copies only that centered picture to a private
 * GLES2 texture and draws it once over the complete default backbuffer.
 * Native mode is a strict no-op rollback; a 16:9 drawable is an identity.
 */

#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>

#include "display_contract.h"
#include "present_fill.h"

typedef struct {
    GLint enabled;
    GLint size;
    GLint stride;
    GLint type;
    GLint normalized;
    GLint buffer;
    void *pointer;
    GLfloat current[4];
} sf_fill_attrib_state;

typedef struct {
    GLint framebuffer;
    GLint program;
    GLint active_texture;
    GLint texture0;
    GLint array_buffer;
    GLint element_buffer;
    GLint viewport[4];
    GLint scissor_box[4];
    GLboolean color_mask[4];
    GLboolean depth_mask;
    GLboolean blend;
    GLboolean cull;
    GLboolean depth;
    GLboolean stencil;
    GLboolean scissor;
    sf_fill_attrib_state attrib[2];
} sf_fill_gl_state;

static void *(*resolve_gl)(const char *);
static GLuint fill_program;
static GLuint fill_texture;
static GLint fill_sampler = -1;
static int texture_width;
static int texture_height;
static int failure_reported;
static int identity_reported;
static unsigned long filled_frames;

#define SF_GL_FUNCTIONS(X)                                                   \
    X(void, active_texture, (GLenum))                                        \
    X(void, attach_shader, (GLuint, GLuint))                                 \
    X(void, bind_attrib_location, (GLuint, GLuint, const GLchar *))           \
    X(void, bind_buffer, (GLenum, GLuint))                                   \
    X(void, bind_framebuffer, (GLenum, GLuint))                              \
    X(void, bind_texture, (GLenum, GLuint))                                  \
    X(void, color_mask, (GLboolean, GLboolean, GLboolean, GLboolean))         \
    X(void, compile_shader, (GLuint))                                        \
    X(void, copy_tex_sub_image_2d,                                           \
      (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))          \
    X(GLuint, create_program, (void))                                        \
    X(GLuint, create_shader, (GLenum))                                       \
    X(void, delete_program, (GLuint))                                        \
    X(void, delete_shader, (GLuint))                                         \
    X(void, depth_mask, (GLboolean))                                         \
    X(void, disable, (GLenum))                                               \
    X(void, disable_vertex_attrib_array, (GLuint))                           \
    X(void, draw_arrays, (GLenum, GLint, GLsizei))                           \
    X(void, enable, (GLenum))                                                \
    X(void, enable_vertex_attrib_array, (GLuint))                            \
    X(void, gen_textures, (GLsizei, GLuint *))                               \
    X(void, get_booleanv, (GLenum, GLboolean *))                             \
    X(void, get_integerv, (GLenum, GLint *))                                 \
    X(void, get_program_iv, (GLuint, GLenum, GLint *))                       \
    X(void, get_program_info_log, (GLuint, GLsizei, GLsizei *, GLchar *))     \
    X(void, get_shader_iv, (GLuint, GLenum, GLint *))                        \
    X(void, get_shader_info_log, (GLuint, GLsizei, GLsizei *, GLchar *))      \
    X(GLint, get_uniform_location, (GLuint, const GLchar *))                 \
    X(void, get_vertex_attrib_fv, (GLuint, GLenum, GLfloat *))               \
    X(void, get_vertex_attrib_iv, (GLuint, GLenum, GLint *))                 \
    X(void, get_vertex_attrib_pointer_v, (GLuint, GLenum, void **))          \
    X(GLboolean, is_enabled, (GLenum))                                       \
    X(GLboolean, is_program, (GLuint))                                       \
    X(GLboolean, is_texture, (GLuint))                                       \
    X(void, link_program, (GLuint))                                          \
    X(void, shader_source, (GLuint, GLsizei, const GLchar *const *,           \
                            const GLint *))                                  \
    X(void, tex_image_2d,                                                     \
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,         \
       const void *))                                                        \
    X(void, tex_parameter_i, (GLenum, GLenum, GLint))                        \
    X(void, uniform_1i, (GLint, GLint))                                      \
    X(void, use_program, (GLuint))                                           \
    X(void, vertex_attrib_4fv, (GLuint, const GLfloat *))                    \
    X(void, vertex_attrib_pointer,                                            \
      (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))             \
    X(void, viewport, (GLint, GLint, GLsizei, GLsizei))                      \
    X(void, scissor_box_set, (GLint, GLint, GLsizei, GLsizei))

#define DECLARE_GL(ret, name, arguments) static ret (*p_##name) arguments;
SF_GL_FUNCTIONS(DECLARE_GL)
#undef DECLARE_GL

void sf_present_fill_set_resolver(void *(*resolver)(const char *))
{
    resolve_gl = resolver;
}

static int resolve_functions(void)
{
    static int attempted;
    static int complete;
    if (attempted)
        return complete;
    attempted = 1;
    if (!resolve_gl)
        return 0;

#define RESOLVE_GL(ret, name, arguments)                                     \
    p_##name = (ret (*) arguments)resolve_gl(                                \
        strcmp(#name, "scissor_box_set") == 0 ? "glScissor" :              \
        strcmp(#name, "get_program_iv") == 0 ? "glGetProgramiv" :           \
        strcmp(#name, "get_shader_iv") == 0 ? "glGetShaderiv" :             \
        strcmp(#name, "get_program_info_log") == 0 ? "glGetProgramInfoLog" :\
        strcmp(#name, "get_shader_info_log") == 0 ? "glGetShaderInfoLog" :  \
        strcmp(#name, "get_uniform_location") == 0 ? "glGetUniformLocation":\
        strcmp(#name, "get_vertex_attrib_fv") == 0 ? "glGetVertexAttribfv" :\
        strcmp(#name, "get_vertex_attrib_iv") == 0 ? "glGetVertexAttribiv" :\
        strcmp(#name, "get_vertex_attrib_pointer_v") == 0                    \
            ? "glGetVertexAttribPointerv" :                                 \
        strcmp(#name, "copy_tex_sub_image_2d") == 0                         \
            ? "glCopyTexSubImage2D" :                                       \
        strcmp(#name, "tex_image_2d") == 0 ? "glTexImage2D" :              \
        strcmp(#name, "tex_parameter_i") == 0 ? "glTexParameteri" :        \
        strcmp(#name, "uniform_1i") == 0 ? "glUniform1i" :                 \
        strcmp(#name, "vertex_attrib_4fv") == 0 ? "glVertexAttrib4fv" :     \
        strcmp(#name, "vertex_attrib_pointer") == 0                         \
            ? "glVertexAttribPointer" :                                     \
        strcmp(#name, "active_texture") == 0 ? "glActiveTexture" :         \
        strcmp(#name, "attach_shader") == 0 ? "glAttachShader" :           \
        strcmp(#name, "bind_attrib_location") == 0                          \
            ? "glBindAttribLocation" :                                      \
        strcmp(#name, "bind_buffer") == 0 ? "glBindBuffer" :               \
        strcmp(#name, "bind_framebuffer") == 0 ? "glBindFramebuffer" :     \
        strcmp(#name, "bind_texture") == 0 ? "glBindTexture" :             \
        strcmp(#name, "color_mask") == 0 ? "glColorMask" :                 \
        strcmp(#name, "compile_shader") == 0 ? "glCompileShader" :         \
        strcmp(#name, "create_program") == 0 ? "glCreateProgram" :         \
        strcmp(#name, "create_shader") == 0 ? "glCreateShader" :           \
        strcmp(#name, "delete_program") == 0 ? "glDeleteProgram" :         \
        strcmp(#name, "delete_shader") == 0 ? "glDeleteShader" :           \
        strcmp(#name, "depth_mask") == 0 ? "glDepthMask" :                 \
        strcmp(#name, "disable_vertex_attrib_array") == 0                   \
            ? "glDisableVertexAttribArray" :                                \
        strcmp(#name, "draw_arrays") == 0 ? "glDrawArrays" :               \
        strcmp(#name, "enable_vertex_attrib_array") == 0                    \
            ? "glEnableVertexAttribArray" :                                 \
        strcmp(#name, "gen_textures") == 0 ? "glGenTextures" :             \
        strcmp(#name, "get_booleanv") == 0 ? "glGetBooleanv" :             \
        strcmp(#name, "get_integerv") == 0 ? "glGetIntegerv" :             \
        strcmp(#name, "is_enabled") == 0 ? "glIsEnabled" :                 \
        strcmp(#name, "is_program") == 0 ? "glIsProgram" :                 \
        strcmp(#name, "is_texture") == 0 ? "glIsTexture" :                 \
        strcmp(#name, "link_program") == 0 ? "glLinkProgram" :             \
        strcmp(#name, "shader_source") == 0 ? "glShaderSource" :           \
        strcmp(#name, "use_program") == 0 ? "glUseProgram" :               \
        strcmp(#name, "viewport") == 0 ? "glViewport" :                    \
        strcmp(#name, "enable") == 0 ? "glEnable" :                        \
        strcmp(#name, "disable") == 0 ? "glDisable" : "");
    SF_GL_FUNCTIONS(RESOLVE_GL)
#undef RESOLVE_GL

    complete =
#define REQUIRE_GL(ret, name, arguments) p_##name &&
        SF_GL_FUNCTIONS(REQUIRE_GL)
#undef REQUIRE_GL
        1;
    return complete;
}

static GLuint compile_stage(GLenum type, const char *source)
{
    GLuint shader = p_create_shader(type);
    if (!shader)
        return 0;
    const GLchar *text = (const GLchar *)source;
    p_shader_source(shader, 1, &text, NULL);
    p_compile_shader(shader);
    GLint ok = 0;
    p_get_shader_iv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[256] = { 0 };
        p_get_shader_info_log(shader, sizeof log, NULL, log);
        fprintf(stderr, "[sf/display] fill shader failed: %.200s\n", log);
        p_delete_shader(shader);
        return 0;
    }
    return shader;
}

static int create_program(void)
{
    static const char vertex_source[] =
        "attribute vec2 a_position;\n"
        "attribute vec2 a_texcoord;\n"
        "varying vec2 v_texcoord;\n"
        "void main(){v_texcoord=a_texcoord;gl_Position=vec4(a_position,0.0,1.0);}\n";
    static const char fragment_source[] =
        "precision mediump float;\n"
        "uniform sampler2D u_frame;\n"
        "varying vec2 v_texcoord;\n"
        "void main(){vec4 c=texture2D(u_frame,v_texcoord);gl_FragColor=vec4(c.rgb,1.0);}\n";

    GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) {
        if (vertex) p_delete_shader(vertex);
        if (fragment) p_delete_shader(fragment);
        return 0;
    }
    GLuint program = p_create_program();
    if (!program) {
        p_delete_shader(vertex);
        p_delete_shader(fragment);
        return 0;
    }
    p_attach_shader(program, vertex);
    p_attach_shader(program, fragment);
    p_bind_attrib_location(program, 0, "a_position");
    p_bind_attrib_location(program, 1, "a_texcoord");
    p_link_program(program);
    p_delete_shader(vertex);
    p_delete_shader(fragment);
    GLint ok = 0;
    p_get_program_iv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar log[256] = { 0 };
        p_get_program_info_log(program, sizeof log, NULL, log);
        fprintf(stderr, "[sf/display] fill program failed: %.200s\n", log);
        p_delete_program(program);
        return 0;
    }
    fill_program = program;
    fill_sampler = p_get_uniform_location(program, "u_frame");
    return fill_sampler >= 0;
}

static void snapshot_attrib(GLuint index, sf_fill_attrib_state *state)
{
    memset(state, 0, sizeof *state);
    p_get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                           &state->enabled);
    p_get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &state->size);
    p_get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                           &state->stride);
    p_get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &state->type);
    p_get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                           &state->normalized);
    p_get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                           &state->buffer);
    p_get_vertex_attrib_pointer_v(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &state->pointer);
    p_get_vertex_attrib_fv(index, GL_CURRENT_VERTEX_ATTRIB, state->current);
}

static void snapshot_state(sf_fill_gl_state *state)
{
    memset(state, 0, sizeof *state);
    p_get_integerv(GL_FRAMEBUFFER_BINDING, &state->framebuffer);
    p_get_integerv(GL_CURRENT_PROGRAM, &state->program);
    p_get_integerv(GL_ACTIVE_TEXTURE, &state->active_texture);
    p_active_texture(GL_TEXTURE0);
    p_get_integerv(GL_TEXTURE_BINDING_2D, &state->texture0);
    p_active_texture((GLenum)state->active_texture);
    p_get_integerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
    p_get_integerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state->element_buffer);
    p_get_integerv(GL_VIEWPORT, state->viewport);
    p_get_integerv(GL_SCISSOR_BOX, state->scissor_box);
    p_get_booleanv(GL_COLOR_WRITEMASK, state->color_mask);
    p_get_booleanv(GL_DEPTH_WRITEMASK, &state->depth_mask);
    state->blend = p_is_enabled(GL_BLEND);
    state->cull = p_is_enabled(GL_CULL_FACE);
    state->depth = p_is_enabled(GL_DEPTH_TEST);
    state->stencil = p_is_enabled(GL_STENCIL_TEST);
    state->scissor = p_is_enabled(GL_SCISSOR_TEST);
    snapshot_attrib(0, &state->attrib[0]);
    snapshot_attrib(1, &state->attrib[1]);
}

static void restore_cap(GLenum capability, GLboolean enabled)
{
    if (enabled)
        p_enable(capability);
    else
        p_disable(capability);
}

static void restore_attrib(GLuint index, const sf_fill_attrib_state *state)
{
    p_bind_buffer(GL_ARRAY_BUFFER, (GLuint)state->buffer);
    p_vertex_attrib_pointer(index, state->size, (GLenum)state->type,
                            state->normalized ? GL_TRUE : GL_FALSE,
                            state->stride, state->pointer);
    p_vertex_attrib_4fv(index, state->current);
    if (state->enabled)
        p_enable_vertex_attrib_array(index);
    else
        p_disable_vertex_attrib_array(index);
}

static void restore_state(const sf_fill_gl_state *state)
{
    restore_attrib(0, &state->attrib[0]);
    restore_attrib(1, &state->attrib[1]);
    p_bind_buffer(GL_ARRAY_BUFFER, (GLuint)state->array_buffer);
    p_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)state->element_buffer);
    p_use_program((GLuint)state->program);
    p_active_texture(GL_TEXTURE0);
    p_bind_texture(GL_TEXTURE_2D, (GLuint)state->texture0);
    p_active_texture((GLenum)state->active_texture);
    p_viewport(state->viewport[0], state->viewport[1], state->viewport[2],
               state->viewport[3]);
    p_scissor_box_set(state->scissor_box[0], state->scissor_box[1],
                      state->scissor_box[2], state->scissor_box[3]);
    p_color_mask(state->color_mask[0], state->color_mask[1],
                 state->color_mask[2], state->color_mask[3]);
    p_depth_mask(state->depth_mask);
    restore_cap(GL_BLEND, state->blend);
    restore_cap(GL_CULL_FACE, state->cull);
    restore_cap(GL_DEPTH_TEST, state->depth);
    restore_cap(GL_STENCIL_TEST, state->stencil);
    restore_cap(GL_SCISSOR_TEST, state->scissor);
    p_bind_framebuffer(GL_FRAMEBUFFER, (GLuint)state->framebuffer);
}

static int ensure_resources(int width, int height)
{
    if (fill_program && !p_is_program(fill_program)) {
        fill_program = 0;
        fill_sampler = -1;
    }
    if (!fill_program && !create_program())
        return 0;

    if (fill_texture && !p_is_texture(fill_texture)) {
        fill_texture = 0;
        texture_width = texture_height = 0;
    }
    if (!fill_texture)
        p_gen_textures(1, &fill_texture);
    if (!fill_texture)
        return 0;

    p_active_texture(GL_TEXTURE0);
    p_bind_texture(GL_TEXTURE_2D, fill_texture);
    p_tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (texture_width != width || texture_height != height) {
        p_tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, NULL);
        texture_width = width;
        texture_height = height;
    }
    return 1;
}

int sf_present_fill_apply(int drawable_width, int drawable_height,
                          const char *aspect_policy)
{
    if (aspect_policy && strcmp(aspect_policy, "native") == 0)
        return 0;

    sf_display_rect source;
    int geometry = sf_display_fill_source_rect(drawable_width, drawable_height,
                                               &source);
    if (geometry <= 0) {
        if (geometry == 0 && !identity_reported) {
            identity_reported = 1;
            fprintf(stderr,
                    "DISPLAY-FILL-RECEIPT {\"schema\":\"sf-display-fill-v1\","
                    "\"policy\":\"fill\",\"route\":\"identity\","
                    "\"drawable\":{\"width\":%d,\"height\":%d}}\n",
                    drawable_width, drawable_height);
        }
        return geometry;
    }
    if (!resolve_functions()) {
        if (!failure_reported) {
            failure_reported = 1;
            fprintf(stderr,
                    "[sf/display] final-frame fill unavailable: incomplete GLES2 API\n");
        }
        return -1;
    }

    sf_fill_gl_state state;
    snapshot_state(&state);
    p_bind_framebuffer(GL_FRAMEBUFFER, 0);
    if (!ensure_resources(source.width, source.height)) {
        restore_state(&state);
        if (!failure_reported) {
            failure_reported = 1;
            fprintf(stderr,
                    "[sf/display] final-frame fill unavailable: resource setup failed\n");
        }
        return -1;
    }

    p_copy_tex_sub_image_2d(GL_TEXTURE_2D, 0, 0, 0, source.x, source.y,
                            source.width, source.height);
    p_disable(GL_BLEND);
    p_disable(GL_CULL_FACE);
    p_disable(GL_DEPTH_TEST);
    p_disable(GL_STENCIL_TEST);
    p_disable(GL_SCISSOR_TEST);
    p_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_depth_mask(GL_FALSE);
    p_viewport(0, 0, drawable_width, drawable_height);
    p_use_program(fill_program);
    p_uniform_1i(fill_sampler, 0);
    p_bind_buffer(GL_ARRAY_BUFFER, 0);
    p_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    static const GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    p_vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE,
                            4 * (GLsizei)sizeof(GLfloat), vertices);
    p_vertex_attrib_pointer(1, 2, GL_FLOAT, GL_FALSE,
                            4 * (GLsizei)sizeof(GLfloat), vertices + 2);
    p_enable_vertex_attrib_array(0);
    p_enable_vertex_attrib_array(1);
    p_draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    restore_state(&state);

    filled_frames++;
    if (filled_frames == 1) {
        fprintf(stderr,
                "DISPLAY-FILL-RECEIPT {\"schema\":\"sf-display-fill-v1\","
                "\"policy\":\"fill\",\"route\":\"gles2-copy-scale\","
                "\"source\":{\"x\":%d,\"y\":%d,\"width\":%d,"
                "\"height\":%d},\"drawable\":{\"width\":%d,"
                "\"height\":%d}}\n",
                source.x, source.y, source.width, source.height,
                drawable_width, drawable_height);
    }
    return 1;
}
