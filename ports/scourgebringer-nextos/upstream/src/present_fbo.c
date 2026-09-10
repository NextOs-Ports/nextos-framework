/*
 * A build Netflix do TMNT renderiza o quadro completo em um RenderTarget,
 * mas o display shader seguinte produz RGB preto no GLES2 do Mali-450.
 * Este passe opt-in apresenta o último FBO full-size com um shader ES2 mínimo.
 */
#include "present_fbo.h"
#include "sdv_egl_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdv_gl_copy_known_framebuffers(unsigned int *out, int capacity);
extern int sdv_gl_texture_dimensions(unsigned int id, int *w, int *h);

typedef void (*GetIntegervFn)(unsigned int, int *);
typedef void (*GetBooleanvFn)(unsigned int, unsigned char *);
typedef unsigned char (*IsEnabledFn)(unsigned int);
typedef void (*EnableDisableFn)(unsigned int);
typedef unsigned char (*IsFramebufferFn)(unsigned int);
typedef void (*BindFramebufferFn)(unsigned int, unsigned int);
typedef unsigned int (*CheckFramebufferStatusFn)(unsigned int);
typedef void (*GetAttachmentFn)(unsigned int, unsigned int, unsigned int,
                                int *);
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
typedef void (*ActiveTextureFn)(unsigned int);
typedef void (*BindTextureFn)(unsigned int, unsigned int);
typedef void (*GetVertexAttribivFn)(unsigned int, unsigned int, int *);
typedef void (*GetVertexAttribPointervFn)(unsigned int, unsigned int, void **);
typedef void (*VertexAttribPointerFn)(unsigned int, int, unsigned int,
                                      unsigned char, int, const void *);
typedef void (*VertexAttribArrayFn)(unsigned int);
typedef void (*ViewportFn)(int, int, int, int);
typedef void (*ColorMaskFn)(unsigned char, unsigned char,
                            unsigned char, unsigned char);
typedef void (*DrawArraysFn)(unsigned int, int, int);
typedef void (*ReadPixelsFn)(int, int, int, int, unsigned int, unsigned int,
                             void *);
typedef unsigned int (*GetErrorFn)(void);

static GetIntegervFn p_get_integerv;
static GetBooleanvFn p_get_booleanv;
static IsEnabledFn p_is_enabled;
static EnableDisableFn p_enable, p_disable;
static IsFramebufferFn p_is_framebuffer;
static BindFramebufferFn p_bind_framebuffer;
static CheckFramebufferStatusFn p_check_fbo;
static GetAttachmentFn p_get_attachment;
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
static ActiveTextureFn p_active_texture;
static BindTextureFn p_bind_texture;
static GetVertexAttribivFn p_get_vertex_attrib_iv;
static GetVertexAttribPointervFn p_get_vertex_attrib_pointer;
static VertexAttribPointerFn p_vertex_attrib_pointer;
static VertexAttribArrayFn p_enable_attrib, p_disable_attrib;
static ViewportFn p_viewport;
static ColorMaskFn p_color_mask;
static DrawArraysFn p_draw_arrays;
static ReadPixelsFn p_read_pixels;
static GetErrorFn p_get_error;

static unsigned int present_program, present_vbo;
static int present_sampler = -1;
static int symbols_resolved, init_failed, last_known_count = -1;
static unsigned int source_fbo, source_texture;
static int bypass_logged;
static unsigned int dump_round;

#define RESOLVE(dst, name) do {                                              \
    void *symbol_ = sdv_egl_get_proc_address(name);                          \
    memcpy(&(dst), &symbol_, sizeof(dst));                                   \
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
    RESOLVE(p_is_framebuffer, "glIsFramebuffer");
    RESOLVE(p_bind_framebuffer, "glBindFramebuffer");
    RESOLVE(p_check_fbo, "glCheckFramebufferStatus");
    RESOLVE(p_get_attachment, "glGetFramebufferAttachmentParameteriv");
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
    RESOLVE(p_active_texture, "glActiveTexture");
    RESOLVE(p_bind_texture, "glBindTexture");
    RESOLVE(p_get_vertex_attrib_iv, "glGetVertexAttribiv");
    RESOLVE(p_get_vertex_attrib_pointer, "glGetVertexAttribPointerv");
    RESOLVE(p_vertex_attrib_pointer, "glVertexAttribPointer");
    RESOLVE(p_enable_attrib, "glEnableVertexAttribArray");
    RESOLVE(p_disable_attrib, "glDisableVertexAttribArray");
    RESOLVE(p_viewport, "glViewport");
    RESOLVE(p_color_mask, "glColorMask");
    RESOLVE(p_draw_arrays, "glDrawArrays");
    RESOLVE(p_read_pixels, "glReadPixels");
    RESOLVE(p_get_error, "glGetError");

    if (!p_get_integerv || !p_get_booleanv || !p_is_enabled || !p_enable ||
        !p_disable || !p_is_framebuffer || !p_bind_framebuffer ||
        !p_check_fbo || !p_get_attachment || !p_create_shader ||
        !p_shader_source || !p_compile_shader || !p_get_shader_iv ||
        !p_create_program || !p_attach_shader || !p_bind_attrib_location ||
        !p_link_program || !p_get_program_iv || !p_get_uniform_location ||
        !p_uniform1i || !p_use_program || !p_gen_buffers || !p_bind_buffer ||
        !p_buffer_data || !p_active_texture || !p_bind_texture ||
        !p_get_vertex_attrib_iv || !p_get_vertex_attrib_pointer ||
        !p_vertex_attrib_pointer || !p_enable_attrib || !p_disable_attrib ||
        !p_viewport || !p_color_mask || !p_draw_arrays) {
        fprintf(stderr, "[sdv-egl] present-fbo: simbolo GLES2 ausente\n");
        init_failed = 1;
    }
    return !init_failed;
}

struct present_candidate {
    unsigned int fbo;
    unsigned int texture;
    int width;
    int height;
};

static unsigned int forced_source_fbo(void)
{
    const char *value = getenv("SB_PRESENT_SOURCE_FBO");
    char *end = NULL;
    unsigned long parsed;
    if (!value || !value[0]) return 0;
    parsed = strtoul(value, &end, 0);
    return end && end != value && parsed <= 0xfffffffful
        ? (unsigned int)parsed : 0;
}

static void dump_candidate(const char *prefix,
                           const struct present_candidate *candidate)
{
    char path[1024];
    FILE *file;
    unsigned char *pixels;
    size_t pixel_count, byte_count;
    unsigned int error = 0;
    unsigned long long rgb_sum = 0, alpha_sum = 0;
    unsigned int nonblack = 0, samples = 0;

    if (!prefix || !prefix[0] || !p_read_pixels ||
        candidate->width <= 0 || candidate->height <= 0)
        return;
    pixel_count = (size_t)candidate->width * (size_t)candidate->height;
    if (pixel_count > 4096u * 4096u) return;
    byte_count = pixel_count * 4u;
    pixels = malloc(byte_count);
    if (!pixels) return;

    if (p_get_error) while (p_get_error() != 0) {}
    p_bind_framebuffer(0x8D40u /* FRAMEBUFFER */, candidate->fbo);
    p_read_pixels(0, 0, candidate->width, candidate->height,
                  0x1908u /* RGBA */, 0x1401u /* UNSIGNED_BYTE */, pixels);
    if (p_get_error) error = p_get_error();
    if (error) {
        fprintf(stderr,
                "[sdv-egl] present-dump fbo=%u tex=%u falhou GL=0x%x\n",
                candidate->fbo, candidate->texture, error);
        free(pixels);
        return;
    }

    /* Amostragem esparsa para distinguir alvo realmente preto de cena. */
    for (size_t i = 0; i < pixel_count; i += 257u) {
        const unsigned char *p = pixels + i * 4u;
        unsigned int rgb = (unsigned int)p[0] + p[1] + p[2];
        rgb_sum += rgb;
        alpha_sum += p[3];
        if (rgb > 12u) ++nonblack;
        ++samples;
    }
    snprintf(path, sizeof path, "%s-r%u-fbo%u-tex%u-%dx%d.rgba",
             prefix, dump_round, candidate->fbo, candidate->texture,
             candidate->width, candidate->height);
    file = fopen(path, "wb");
    if (file) {
        if (fwrite(pixels, 1, byte_count, file) != byte_count)
            fprintf(stderr, "[sdv-egl] present-dump escrita curta: %s\n", path);
        fclose(file);
        fprintf(stderr,
                "[sdv-egl] present-dump %s samples=%u nonblack=%u "
                "rgb=%llu alpha=%llu\n",
                path, samples, nonblack, rgb_sum, alpha_sum);
    }
    free(pixels);
}

static void update_source(void)
{
    unsigned int ids[32];
    struct present_candidate candidates[32];
    int candidate_count = 0;
    int count = sdv_gl_copy_known_framebuffers(ids, 32);
    if (count < 4) return;
    if (count == last_known_count && source_fbo &&
        p_is_framebuffer(source_fbo)) return;

    int old_fbo = 0;
    unsigned int previous_fbo = source_fbo;
    unsigned int forced_fbo = forced_source_fbo();
    unsigned int fallback_fbo = 0, fallback_texture = 0;
    unsigned long long fallback_area = 0;
    p_get_integerv(0x8CA6u /* FRAMEBUFFER_BINDING */, &old_fbo);
    last_known_count = count;
    source_fbo = source_texture = 0;
    for (int i = 0; i < count; ++i) {
        int type = 0, texture = 0, width = 0, height = 0;
        if (!p_is_framebuffer(ids[i])) continue;
        p_bind_framebuffer(0x8D40u /* FRAMEBUFFER */, ids[i]);
        if (p_check_fbo(0x8D40u) != 0x8CD5u /* COMPLETE */) continue;
        p_get_attachment(0x8D40u, 0x8CE0u /* COLOR_ATTACHMENT0 */,
                         0x8CD0u /* OBJECT_TYPE */, &type);
        p_get_attachment(0x8D40u, 0x8CE0u, 0x8CD1u /* OBJECT_NAME */,
                         &texture);
        if (type == 0x1702u /* TEXTURE */ &&
            sdv_gl_texture_dimensions((unsigned)texture, &width, &height) &&
            width >= 1280 && height >= 720) {
            struct present_candidate *candidate =
                &candidates[candidate_count++];
            candidate->fbo = ids[i];
            candidate->texture = (unsigned int)texture;
            candidate->width = width;
            candidate->height = height;
            /* O gameplay cria depois um FBO auxiliar 1280x720 totalmente
             * preto. O alvo composto real continua 1920x1080 (FBO 4). Entre
             * alvos do mesmo tamanho, o mais novo vence; um auxiliar menor
             * nunca deve substituir o compositor principal. */
            unsigned long long area =
                (unsigned long long)width * (unsigned long long)height;
            if (area >= fallback_area) {
                fallback_area = area;
                fallback_fbo = candidate->fbo;
                fallback_texture = candidate->texture;
            }
            if (forced_fbo == candidate->fbo) {
                source_fbo = candidate->fbo;
                source_texture = candidate->texture;
            }
        }
    }
    if (!source_texture) {
        source_fbo = fallback_fbo;
        source_texture = fallback_texture;
    }
    if (getenv("SB_PRESENT_TRACE")) {
        fprintf(stderr,
                "[sdv-egl] present-fbo candidatos=%d conhecidos=%d force=%u",
                candidate_count, count, forced_fbo);
        for (int i = 0; i < candidate_count; ++i)
            fprintf(stderr, " fbo%u/tex%u/%dx%d", candidates[i].fbo,
                    candidates[i].texture, candidates[i].width,
                    candidates[i].height);
        fputc('\n', stderr);
    }
    if (source_fbo && source_fbo != previous_fbo &&
        getenv("SB_PRESENT_DUMP")) {
        const char *prefix = getenv("SB_PRESENT_DUMP");
        ++dump_round;
        for (int i = 0; i < candidate_count; ++i)
            dump_candidate(prefix, &candidates[i]);
    }
    p_bind_framebuffer(0x8D40u, old_fbo >= 0 ? (unsigned)old_fbo : 0u);
    if (source_texture)
        fprintf(stderr, "[sdv-egl] present-fbo source fbo=%u texture=%u\n",
                source_fbo, source_texture);
}

static int init_present_program(void)
{
    static const char *vertex_source =
        "attribute vec4 a; varying vec2 uv;"
        "void main(){gl_Position=vec4(a.xy,0.0,1.0);uv=a.zw;}";
    static const char *fragment_source =
        "precision mediump float; varying vec2 uv; uniform sampler2D s;"
        /* O compositor fbdev/OSD do Amlogic usa o alfa do fb0. No gameplay,
         * o RenderTarget composto pode ter RGB correto com alfa zero: menus e
         * HUD aparecem, mas a fase fica preta/transparente. Este e o ultimo
         * passe para o backbuffer, portanto grave sempre alfa opaco aqui. */
        "void main(){vec4 c=texture2D(s,uv);gl_FragColor=vec4(c.rgb,1.0);}";
    static const float quad[16] = {
        /* RenderTarget do MonoGame tem origem superior; o display shader
         * original faria este flip vertical antes do backbuffer. */
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f
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
    present_program = p_create_program();
    p_attach_shader(present_program, vertex);
    p_attach_shader(present_program, fragment);
    p_bind_attrib_location(present_program, 0, "a");
    p_link_program(present_program);
    p_get_program_iv(present_program, 0x8B82u /* LINK_STATUS */, &link_ok);
    if (!vertex_ok || !fragment_ok || !link_ok) {
        char log[512] = {0};
        int used = 0;
        if (!vertex_ok && p_get_shader_log)
            p_get_shader_log(vertex, sizeof log - 1, &used, log);
        else if (!fragment_ok && p_get_shader_log)
            p_get_shader_log(fragment, sizeof log - 1, &used, log);
        else if (p_get_program_log)
            p_get_program_log(present_program, sizeof log - 1, &used, log);
        fprintf(stderr, "[sdv-egl] present-fbo shader falhou: %s\n", log);
        init_failed = 1;
        return 0;
    }
    present_sampler = p_get_uniform_location(present_program, "s");
    p_gen_buffers(1, &present_vbo);
    p_bind_buffer(0x8892u /* ARRAY_BUFFER */, present_vbo);
    p_buffer_data(0x8892u, (intptr_t)sizeof quad, quad,
                  0x88E4u /* STATIC_DRAW */);
    return present_vbo != 0;
}

void sb_present_fullsize_fbo(int backbuffer_width, int backbuffer_height)
{
    const char *value = getenv("SB_PRESENT_FBO");
    if (!value || !value[0] || value[0] == '0' || !resolve_symbols()) return;
    update_source();
    if (!source_texture) return;

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

    if (!present_program && !init_present_program()) goto restore;
    p_bind_framebuffer(0x8D40u /* FRAMEBUFFER */, 0);
    p_viewport(0, 0, backbuffer_width, backbuffer_height);
    for (unsigned int i = 0; i < sizeof caps / sizeof caps[0]; ++i)
        p_disable(caps[i]);
    p_color_mask(1, 1, 1, 1);
    p_use_program(present_program);
    p_active_texture(0x84C0u /* TEXTURE0 */);
    p_bind_texture(0x0DE1u /* TEXTURE_2D */, source_texture);
    p_uniform1i(present_sampler, 0);
    p_bind_buffer(0x8892u /* ARRAY_BUFFER */, present_vbo);
    p_vertex_attrib_pointer(0, 4, 0x1406u /* FLOAT */, 0,
                            4 * (int)sizeof(float), NULL);
    p_enable_attrib(0);
    p_draw_arrays(0x0005u /* TRIANGLE_STRIP */, 0, 4);
    if (!bypass_logged) {
        fprintf(stderr, "[sdv-egl] present-fbo bypass ativo\n");
        bypass_logged = 1;
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
