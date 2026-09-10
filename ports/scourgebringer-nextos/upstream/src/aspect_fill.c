/*
 * ScourgeBringer compoe sempre um quadro 16:9 e, em painéis 4:3, deixa barras
 * pretas acima e abaixo. Este passe port-specific copia somente o retangulo
 * util do backbuffer e o apresenta em tela cheia. Toda a HUD e preservada; a
 * adaptacao inevitavel e uma escala vertical do quadro 16:9 para o painel.
 *
 * O caminho e opt-in, baseado apenas nas dimensoes fisicas, e fica inativo em
 * 16:9. Nenhuma regra por firmware/device entra aqui.
 */
#include "aspect_fill.h"
#include "sdv_egl_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*GetIntegervFn)(unsigned int, int *);
typedef void (*GetBooleanvFn)(unsigned int, unsigned char *);
typedef unsigned char (*IsEnabledFn)(unsigned int);
typedef void (*EnableDisableFn)(unsigned int);
typedef void (*BindFramebufferFn)(unsigned int, unsigned int);
typedef unsigned int (*CreateShaderFn)(unsigned int);
typedef void (*ShaderSourceFn)(unsigned int, int, const char *const *,
                               const int *);
typedef void (*CompileShaderFn)(unsigned int);
typedef void (*GetShaderivFn)(unsigned int, unsigned int, int *);
typedef void (*GetShaderInfoLogFn)(unsigned int, int, int *, char *);
typedef unsigned int (*CreateProgramFn)(void);
typedef void (*AttachShaderFn)(unsigned int, unsigned int);
typedef void (*BindAttribLocationFn)(unsigned int, unsigned int,
                                     const char *);
typedef void (*LinkProgramFn)(unsigned int);
typedef void (*GetProgramivFn)(unsigned int, unsigned int, int *);
typedef void (*GetProgramInfoLogFn)(unsigned int, int, int *, char *);
typedef int (*GetUniformLocationFn)(unsigned int, const char *);
typedef void (*Uniform1iFn)(int, int);
typedef void (*UseProgramFn)(unsigned int);
typedef void (*GenBuffersFn)(int, unsigned int *);
typedef void (*BindBufferFn)(unsigned int, unsigned int);
typedef void (*BufferDataFn)(unsigned int, intptr_t, const void *,
                             unsigned int);
typedef void (*GenTexturesFn)(int, unsigned int *);
typedef void (*ActiveTextureFn)(unsigned int);
typedef void (*BindTextureFn)(unsigned int, unsigned int);
typedef void (*TexParameteriFn)(unsigned int, unsigned int, int);
typedef void (*TexImage2DFn)(unsigned int, int, int, int, int, int,
                             unsigned int, unsigned int, const void *);
typedef void (*CopyTexSubImage2DFn)(unsigned int, int, int, int, int, int,
                                    int, int);
typedef void (*GetVertexAttribivFn)(unsigned int, unsigned int, int *);
typedef void (*GetVertexAttribPointervFn)(unsigned int, unsigned int, void **);
typedef void (*VertexAttribPointerFn)(unsigned int, int, unsigned int,
                                      unsigned char, int, const void *);
typedef void (*VertexAttribArrayFn)(unsigned int);
typedef void (*ViewportFn)(int, int, int, int);
typedef void (*ColorMaskFn)(unsigned char, unsigned char,
                            unsigned char, unsigned char);
typedef void (*DrawArraysFn)(unsigned int, int, int);

static GetIntegervFn p_get_integerv;
static GetBooleanvFn p_get_booleanv;
static IsEnabledFn p_is_enabled;
static EnableDisableFn p_enable, p_disable;
static BindFramebufferFn p_bind_framebuffer;
static CreateShaderFn p_create_shader;
static ShaderSourceFn p_shader_source;
static CompileShaderFn p_compile_shader;
static GetShaderivFn p_get_shader_iv;
static GetShaderInfoLogFn p_get_shader_log;
static CreateProgramFn p_create_program;
static AttachShaderFn p_attach_shader;
static BindAttribLocationFn p_bind_attrib_location;
static LinkProgramFn p_link_program;
static GetProgramivFn p_get_program_iv;
static GetProgramInfoLogFn p_get_program_log;
static GetUniformLocationFn p_get_uniform_location;
static Uniform1iFn p_uniform1i;
static UseProgramFn p_use_program;
static GenBuffersFn p_gen_buffers;
static BindBufferFn p_bind_buffer;
static BufferDataFn p_buffer_data;
static GenTexturesFn p_gen_textures;
static ActiveTextureFn p_active_texture;
static BindTextureFn p_bind_texture;
static TexParameteriFn p_tex_parameter_i;
static TexImage2DFn p_tex_image_2d;
static CopyTexSubImage2DFn p_copy_tex_sub_image_2d;
static GetVertexAttribivFn p_get_vertex_attrib_iv;
static GetVertexAttribPointervFn p_get_vertex_attrib_pointer;
static VertexAttribPointerFn p_vertex_attrib_pointer;
static VertexAttribArrayFn p_enable_attrib, p_disable_attrib;
static ViewportFn p_viewport;
static ColorMaskFn p_color_mask;
static DrawArraysFn p_draw_arrays;

static unsigned int fill_program, fill_vbo, fill_texture;
static int fill_sampler = -1;
static int texture_width, texture_height;
static int symbols_resolved, init_failed;
static int logged_width, logged_height;

#define RESOLVE(dst, name) do {                                             \
    void *symbol_ = sdv_egl_get_proc_address(name);                         \
    memcpy(&(dst), &symbol_, sizeof(dst));                                  \
} while (0)

static int resolve_symbols(void)
{
    if (symbols_resolved) return !init_failed;
    symbols_resolved = 1;
    RESOLVE(p_get_integerv, "glGetIntegerv");
    RESOLVE(p_get_booleanv, "glGetBooleanv");
    RESOLVE(p_is_enabled, "glIsEnabled");
    RESOLVE(p_enable, "glEnable");
    RESOLVE(p_disable, "glDisable");
    RESOLVE(p_bind_framebuffer, "glBindFramebuffer");
    RESOLVE(p_create_shader, "glCreateShader");
    RESOLVE(p_shader_source, "glShaderSource");
    RESOLVE(p_compile_shader, "glCompileShader");
    RESOLVE(p_get_shader_iv, "glGetShaderiv");
    RESOLVE(p_get_shader_log, "glGetShaderInfoLog");
    RESOLVE(p_create_program, "glCreateProgram");
    RESOLVE(p_attach_shader, "glAttachShader");
    RESOLVE(p_bind_attrib_location, "glBindAttribLocation");
    RESOLVE(p_link_program, "glLinkProgram");
    RESOLVE(p_get_program_iv, "glGetProgramiv");
    RESOLVE(p_get_program_log, "glGetProgramInfoLog");
    RESOLVE(p_get_uniform_location, "glGetUniformLocation");
    RESOLVE(p_uniform1i, "glUniform1i");
    RESOLVE(p_use_program, "glUseProgram");
    RESOLVE(p_gen_buffers, "glGenBuffers");
    RESOLVE(p_bind_buffer, "glBindBuffer");
    RESOLVE(p_buffer_data, "glBufferData");
    RESOLVE(p_gen_textures, "glGenTextures");
    RESOLVE(p_active_texture, "glActiveTexture");
    RESOLVE(p_bind_texture, "glBindTexture");
    RESOLVE(p_tex_parameter_i, "glTexParameteri");
    RESOLVE(p_tex_image_2d, "glTexImage2D");
    RESOLVE(p_copy_tex_sub_image_2d, "glCopyTexSubImage2D");
    RESOLVE(p_get_vertex_attrib_iv, "glGetVertexAttribiv");
    RESOLVE(p_get_vertex_attrib_pointer, "glGetVertexAttribPointerv");
    RESOLVE(p_vertex_attrib_pointer, "glVertexAttribPointer");
    RESOLVE(p_enable_attrib, "glEnableVertexAttribArray");
    RESOLVE(p_disable_attrib, "glDisableVertexAttribArray");
    RESOLVE(p_viewport, "glViewport");
    RESOLVE(p_color_mask, "glColorMask");
    RESOLVE(p_draw_arrays, "glDrawArrays");

    if (!p_get_integerv || !p_get_booleanv || !p_is_enabled || !p_enable ||
        !p_disable || !p_bind_framebuffer || !p_create_shader ||
        !p_shader_source || !p_compile_shader || !p_get_shader_iv ||
        !p_create_program || !p_attach_shader || !p_bind_attrib_location ||
        !p_link_program || !p_get_program_iv || !p_get_uniform_location ||
        !p_uniform1i || !p_use_program || !p_gen_buffers || !p_bind_buffer ||
        !p_buffer_data || !p_gen_textures || !p_active_texture ||
        !p_bind_texture || !p_tex_parameter_i || !p_tex_image_2d ||
        !p_copy_tex_sub_image_2d || !p_get_vertex_attrib_iv ||
        !p_get_vertex_attrib_pointer || !p_vertex_attrib_pointer ||
        !p_enable_attrib || !p_disable_attrib || !p_viewport ||
        !p_color_mask || !p_draw_arrays) {
        fprintf(stderr, "[sdv-egl] aspect-fill: simbolo GLES2 ausente\n");
        init_failed = 1;
    }
    return !init_failed;
}

static int init_fill(void)
{
    static const char *vertex_source =
        "attribute vec4 a; varying vec2 uv;"
        "void main(){gl_Position=vec4(a.xy,0.0,1.0);uv=a.zw;}";
    static const char *fragment_source =
        "precision mediump float; varying vec2 uv; uniform sampler2D s;"
        "void main(){vec4 c=texture2D(s,uv);gl_FragColor=vec4(c.rgb,1.0);}";
    static const float quad[16] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };
    int vertex_ok = 0, fragment_ok = 0, link_ok = 0;
    unsigned int vertex = p_create_shader(0x8B31u /* VERTEX_SHADER */);
    unsigned int fragment = p_create_shader(0x8B30u /* FRAGMENT_SHADER */);

    p_shader_source(vertex, 1, &vertex_source, NULL);
    p_shader_source(fragment, 1, &fragment_source, NULL);
    p_compile_shader(vertex);
    p_compile_shader(fragment);
    p_get_shader_iv(vertex, 0x8B81u /* COMPILE_STATUS */, &vertex_ok);
    p_get_shader_iv(fragment, 0x8B81u, &fragment_ok);
    fill_program = p_create_program();
    p_attach_shader(fill_program, vertex);
    p_attach_shader(fill_program, fragment);
    p_bind_attrib_location(fill_program, 0, "a");
    p_link_program(fill_program);
    p_get_program_iv(fill_program, 0x8B82u /* LINK_STATUS */, &link_ok);
    if (!vertex_ok || !fragment_ok || !link_ok) {
        char log[512] = {0};
        int used = 0;
        if (!vertex_ok && p_get_shader_log)
            p_get_shader_log(vertex, sizeof log - 1, &used, log);
        else if (!fragment_ok && p_get_shader_log)
            p_get_shader_log(fragment, sizeof log - 1, &used, log);
        else if (p_get_program_log)
            p_get_program_log(fill_program, sizeof log - 1, &used, log);
        fprintf(stderr, "[sdv-egl] aspect-fill shader falhou: %s\n", log);
        init_failed = 1;
        return 0;
    }
    fill_sampler = p_get_uniform_location(fill_program, "s");
    p_gen_buffers(1, &fill_vbo);
    p_bind_buffer(0x8892u /* ARRAY_BUFFER */, fill_vbo);
    p_buffer_data(0x8892u, (intptr_t)sizeof quad, quad,
                  0x88E4u /* STATIC_DRAW */);
    p_gen_textures(1, &fill_texture);
    if (!fill_vbo || !fill_texture) {
        fprintf(stderr, "[sdv-egl] aspect-fill: recurso GL nao criado\n");
        init_failed = 1;
        return 0;
    }
    return 1;
}

void sb_present_aspect_fill(int backbuffer_width, int backbuffer_height)
{
    const char *value = getenv("SB_ASPECT_FILL");
    int source_height;
    int source_y;
    int old_program = 0, old_active = 0, old_active_binding = 0;
    int old_tex0_binding = 0, old_array_buffer = 0, old_fbo = 0;
    int old_viewport[4] = {0, 0, backbuffer_width, backbuffer_height};
    int attrib_enabled = 0, attrib_size = 4, attrib_type = 0x1406;
    int attrib_normalized = 0, attrib_stride = 0, attrib_buffer = 0;
    void *attrib_pointer = NULL;
    unsigned char old_mask[4] = {1, 1, 1, 1};
    const unsigned int caps[] = {0x0BE2u, 0x0B71u, 0x0B90u,
                                 0x0C11u, 0x0B44u};
    unsigned char cap_enabled[sizeof caps / sizeof caps[0]];

    if (!value || !value[0] || value[0] == '0' ||
        backbuffer_width <= 0 || backbuffer_height <= 0)
        return;

    /* Em 16:9 (ou mais largo) o jogo ja ocupa a altura inteira. */
    if ((int64_t)backbuffer_width * 9 >=
        (int64_t)backbuffer_height * 16) {
        if (logged_width != backbuffer_width ||
            logged_height != backbuffer_height) {
            fprintf(stderr,
                    "[sdv-egl] aspect-fill backbuffer=%dx%d mode=native\n",
                    backbuffer_width, backbuffer_height);
            logged_width = backbuffer_width;
            logged_height = backbuffer_height;
        }
        return;
    }

    source_height = (int)((int64_t)backbuffer_width * 9 / 16);
    if (source_height <= 0 || source_height >= backbuffer_height) return;
    source_y = (backbuffer_height - source_height) / 2;
    if (!resolve_symbols()) return;

    p_get_integerv(0x8B8Du /* CURRENT_PROGRAM */, &old_program);
    p_get_integerv(0x84E0u /* ACTIVE_TEXTURE */, &old_active);
    p_get_integerv(0x8069u /* TEXTURE_BINDING_2D */, &old_active_binding);
    if (old_active != 0x84C0 /* TEXTURE0 */) {
        p_active_texture(0x84C0u);
        p_get_integerv(0x8069u, &old_tex0_binding);
        p_active_texture((unsigned)old_active);
    } else old_tex0_binding = old_active_binding;
    p_get_integerv(0x8894u /* ARRAY_BUFFER_BINDING */, &old_array_buffer);
    p_get_integerv(0x8CA6u /* FRAMEBUFFER_BINDING */, &old_fbo);
    p_get_integerv(0x0BA2u /* VIEWPORT */, old_viewport);
    p_get_booleanv(0x0C23u /* COLOR_WRITEMASK */, old_mask);
    p_get_vertex_attrib_iv(0, 0x8622u /* ARRAY_ENABLED */, &attrib_enabled);
    p_get_vertex_attrib_iv(0, 0x8623u /* ARRAY_SIZE */, &attrib_size);
    p_get_vertex_attrib_iv(0, 0x8625u /* ARRAY_TYPE */, &attrib_type);
    p_get_vertex_attrib_iv(0, 0x886Au /* ARRAY_NORMALIZED */,
                           &attrib_normalized);
    p_get_vertex_attrib_iv(0, 0x8624u /* ARRAY_STRIDE */, &attrib_stride);
    p_get_vertex_attrib_iv(0, 0x889Fu /* ARRAY_BUFFER_BINDING */,
                           &attrib_buffer);
    p_get_vertex_attrib_pointer(0, 0x8645u /* ARRAY_POINTER */,
                                &attrib_pointer);
    for (unsigned int i = 0; i < sizeof caps / sizeof caps[0]; ++i)
        cap_enabled[i] = p_is_enabled(caps[i]);

    if (!fill_program && !init_fill()) goto restore;
    p_bind_framebuffer(0x8D40u /* FRAMEBUFFER */, 0);
    p_active_texture(0x84C0u /* TEXTURE0 */);
    p_bind_texture(0x0DE1u /* TEXTURE_2D */, fill_texture);
    if (texture_width != backbuffer_width || texture_height != source_height) {
        p_tex_parameter_i(0x0DE1u, 0x2801u /* MIN_FILTER */,
                          0x2600 /* NEAREST */);
        p_tex_parameter_i(0x0DE1u, 0x2800u /* MAG_FILTER */,
                          0x2600 /* NEAREST */);
        p_tex_parameter_i(0x0DE1u, 0x2802u /* WRAP_S */,
                          0x812F /* CLAMP_TO_EDGE */);
        p_tex_parameter_i(0x0DE1u, 0x2803u /* WRAP_T */,
                          0x812F /* CLAMP_TO_EDGE */);
        p_tex_image_2d(0x0DE1u, 0, 0x1908 /* RGBA */,
                       backbuffer_width, source_height, 0,
                       0x1908u /* RGBA */, 0x1401u /* UNSIGNED_BYTE */, NULL);
        texture_width = backbuffer_width;
        texture_height = source_height;
    }
    p_copy_tex_sub_image_2d(0x0DE1u, 0, 0, 0, 0, source_y,
                            backbuffer_width, source_height);

    for (unsigned int i = 0; i < sizeof caps / sizeof caps[0]; ++i)
        p_disable(caps[i]);
    p_color_mask(1, 1, 1, 1);
    p_viewport(0, 0, backbuffer_width, backbuffer_height);
    p_use_program(fill_program);
    p_uniform1i(fill_sampler, 0);
    p_bind_buffer(0x8892u /* ARRAY_BUFFER */, fill_vbo);
    p_vertex_attrib_pointer(0, 4, 0x1406u /* FLOAT */, 0,
                            4 * (int)sizeof(float), NULL);
    p_enable_attrib(0);
    p_draw_arrays(0x0005u /* TRIANGLE_STRIP */, 0, 4);
    if (logged_width != backbuffer_width ||
        logged_height != backbuffer_height) {
        fprintf(stderr,
                "[sdv-egl] aspect-fill backbuffer=%dx%d source=0,%d %dx%d "
                "mode=stretch-to-fill\n",
                backbuffer_width, backbuffer_height, source_y,
                backbuffer_width, source_height);
        logged_width = backbuffer_width;
        logged_height = backbuffer_height;
    }

restore:
    p_bind_buffer(0x8892u, attrib_buffer >= 0 ? (unsigned)attrib_buffer : 0u);
    if (attrib_size >= 1 && attrib_size <= 4)
        p_vertex_attrib_pointer(0, attrib_size, (unsigned)attrib_type,
                                (unsigned char)attrib_normalized,
                                attrib_stride, attrib_pointer);
    if (attrib_enabled) p_enable_attrib(0); else p_disable_attrib(0);
    p_bind_buffer(0x8892u,
                  old_array_buffer >= 0 ? (unsigned)old_array_buffer : 0u);
    p_use_program(old_program >= 0 ? (unsigned)old_program : 0u);
    p_active_texture(0x84C0u);
    p_bind_texture(0x0DE1u,
                   old_tex0_binding >= 0 ? (unsigned)old_tex0_binding : 0u);
    if (old_active != 0x84C0) {
        p_active_texture((unsigned)old_active);
        p_bind_texture(0x0DE1u, old_active_binding >= 0
                       ? (unsigned)old_active_binding : 0u);
    }
    p_bind_framebuffer(0x8D40u, old_fbo >= 0 ? (unsigned)old_fbo : 0u);
    p_viewport(old_viewport[0], old_viewport[1], old_viewport[2],
               old_viewport[3]);
    p_color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    for (unsigned int i = 0; i < sizeof caps / sizeof caps[0]; ++i)
        if (cap_enabled[i]) p_enable(caps[i]); else p_disable(caps[i]);
}
