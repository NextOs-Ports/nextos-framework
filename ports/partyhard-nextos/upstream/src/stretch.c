/* SPDX-License-Identifier: GPL-3.0-only */
/* Ver stretch.h.  Toda funcao GL e' resolvida pelo provider real do port
 * (st_gl_raw_sym), nunca pelas fachadas GLES3 do executavel. */
#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "stretch.h"
#include "settings.h"
#include "nxcompat_video.h"
#include "nx_elf.h"

extern void *st_gl_raw_sym(const char *name);

#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

static int g_active;
static int g_panel_w, g_panel_h, g_internal_w, g_internal_h;
static nxcompat_video_owner_decision g_video;
static int g_video_ready;
static GLuint g_fbo, g_color, g_depth;
static GLuint g_program, g_vbo;
static int g_gl_dead;

static struct {
    void (*gen_framebuffers)(GLsizei, GLuint *);
    void (*bind_framebuffer)(GLenum, GLuint);
    void (*gen_textures)(GLsizei, GLuint *);
    void (*bind_texture)(GLenum, GLuint);
    void (*tex_image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*tex_parameteri)(GLenum, GLenum, GLint);
    void (*framebuffer_texture)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*gen_renderbuffers)(GLsizei, GLuint *);
    void (*bind_renderbuffer)(GLenum, GLuint);
    void (*renderbuffer_storage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*framebuffer_renderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum (*check_status)(GLenum);
    void (*viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*clear)(GLbitfield);
    void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*get_int)(GLenum, GLint *);
    void (*get_float)(GLenum, GLfloat *);
    void (*get_bool)(GLenum, GLboolean *);
    GLboolean (*is_enabled)(GLenum);
    void (*enable)(GLenum);
    void (*disable)(GLenum);
    void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (*use_program)(GLuint);
    void (*active_texture)(GLenum);
    void (*bind_buffer)(GLenum, GLuint);
    void (*buffer_data)(GLenum, GLsizeiptr, const void *, GLenum);
    void (*vertex_attrib_pointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*enable_vertex_attrib)(GLuint);
    void (*disable_vertex_attrib)(GLuint);
    void (*get_vertex_attribiv)(GLuint, GLenum, GLint *);
    void (*get_vertex_attrib_ptr)(GLuint, GLenum, void **);
    void (*draw_arrays)(GLenum, GLint, GLsizei);
    GLuint (*create_shader)(GLenum);
    void (*shader_source)(GLuint, GLsizei, const char *const *, const GLint *);
    void (*compile_shader)(GLuint);
    void (*get_shaderiv)(GLuint, GLenum, GLint *);
    GLuint (*create_program)(void);
    void (*attach_shader)(GLuint, GLuint);
    void (*bind_attrib)(GLuint, GLuint, const char *);
    void (*link_program)(GLuint);
    void (*get_programiv)(GLuint, GLenum, GLint *);
    GLint (*get_uniform)(GLuint, const char *);
    void (*uniform1i)(GLint, GLint);
    void (*gen_buffers)(GLsizei, GLuint *);
    GLenum (*get_error)(void);
} gl;

static const char VS[] =
    "attribute vec2 p;\nvarying vec2 t;\n"
    "void main(){t=p*0.5+0.5;gl_Position=vec4(p,0.0,1.0);}\n";
static const char FS[] =
    "precision mediump float;\nuniform sampler2D s;\nvarying vec2 t;\n"
    "void main(){gl_FragColor=texture2D(s,t);}\n";

#define R(field, name) do { *(void **)&gl.field = st_gl_raw_sym(name); \
    if (!gl.field) { nx_log("stretch: falta %s", name); return 0; } } while (0)

static int resolve_gl(void)
{
    if (gl.get_error)
        return 1;
    R(gen_framebuffers, "glGenFramebuffers");
    R(bind_framebuffer, "glBindFramebuffer");
    R(gen_textures, "glGenTextures");
    R(bind_texture, "glBindTexture");
    R(tex_image, "glTexImage2D");
    R(tex_parameteri, "glTexParameteri");
    R(framebuffer_texture, "glFramebufferTexture2D");
    R(gen_renderbuffers, "glGenRenderbuffers");
    R(bind_renderbuffer, "glBindRenderbuffer");
    R(renderbuffer_storage, "glRenderbufferStorage");
    R(framebuffer_renderbuffer, "glFramebufferRenderbuffer");
    R(check_status, "glCheckFramebufferStatus");
    R(viewport, "glViewport");
    R(clear, "glClear");
    R(clear_color, "glClearColor");
    R(get_int, "glGetIntegerv");
    R(get_float, "glGetFloatv");
    R(get_bool, "glGetBooleanv");
    R(is_enabled, "glIsEnabled");
    R(enable, "glEnable");
    R(disable, "glDisable");
    R(color_mask, "glColorMask");
    R(use_program, "glUseProgram");
    R(active_texture, "glActiveTexture");
    R(bind_buffer, "glBindBuffer");
    R(buffer_data, "glBufferData");
    R(vertex_attrib_pointer, "glVertexAttribPointer");
    R(enable_vertex_attrib, "glEnableVertexAttribArray");
    R(disable_vertex_attrib, "glDisableVertexAttribArray");
    R(get_vertex_attribiv, "glGetVertexAttribiv");
    R(get_vertex_attrib_ptr, "glGetVertexAttribPointerv");
    R(draw_arrays, "glDrawArrays");
    R(create_shader, "glCreateShader");
    R(shader_source, "glShaderSource");
    R(compile_shader, "glCompileShader");
    R(get_shaderiv, "glGetShaderiv");
    R(create_program, "glCreateProgram");
    R(attach_shader, "glAttachShader");
    R(bind_attrib, "glBindAttribLocation");
    R(link_program, "glLinkProgram");
    R(get_programiv, "glGetProgramiv");
    R(get_uniform, "glGetUniformLocation");
    R(uniform1i, "glUniform1i");
    R(gen_buffers, "glGenBuffers");
    R(get_error, "glGetError");
    return 1;
}

int st_stretch_setup(int panel_w, int panel_h, int *internal_w, int *internal_h)
{
    nxcompat_video_owner_input request;
    char receipt[512];
    int iw = panel_w, ih = panel_h;

    g_active = 0;
    g_video_ready = 0;
    g_panel_w = panel_w;
    g_panel_h = panel_h;
    if (panel_w <= 0 || panel_h <= 0)
        return 0;

    const nxcompat_settings *settings = st_settings_get();
    int owner_settings = strcmp(st_settings_source(), "settings") == 0;
    memset(&request, 0, sizeof request);
    request.settings_authority = owner_settings ? settings->video_authority : "nextos";
    request.settings_aspect = owner_settings ? settings->video_aspect : "";
    request.port_env_aspect = getenv("NX_VIDEO_ASPECT");
    request.auto_algorithm_declared = 1;
    request.auto_algorithm_result = nxcompat_video_auto_stretch();
    request.package_default = NXCOMPAT_VIDEO_ASPECT_STRETCH;
    request.source_w = 1280;
    request.source_h = 720;
    request.drawable_w = panel_w;
    request.drawable_h = panel_h;
    if (nxcompat_video_resolve_owner(&request, &g_video) != 0) {
        nx_die("video owner decision refused: %s", g_video.reason);
        return 0;
    }
    g_video_ready = 1;
    if (nxcompat_video_owner_receipt(&g_video, receipt, sizeof receipt) > 0)
        fprintf(stderr, "[st/video] %s\n", receipt);

    /* Stretch e preserve usam o mesmo alvo lógico 16:9. Só muda o retângulo
     * de apresentação: painel inteiro ou conteúdo centralizado com barras. */
    if (g_video.geometry.effective == NXCOMPAT_VIDEO_ASPECT_ENGINE) {
        fprintf(stderr, "[st/video] effective=engine; presentation untouched\n");
        return 0;
    }
    {
        int wanted_h = (panel_w * 9 + 8) / 16;
        if (wanted_h > 0 && wanted_h != panel_h && panel_h > 0 &&
            (panel_h - wanted_h > 2 || wanted_h - panel_h > 2)) {
            iw = panel_w;
            ih = wanted_h;
        }
    }
    if (iw == panel_w && ih == panel_h) {
        fprintf(stderr, "[st/video] internal=%dx%d == panel: no-op\n", iw, ih);
        return 0;
    }
    g_internal_w = iw;
    g_internal_h = ih;
    g_active = 1;
    fprintf(stderr, "[st/video] effective=%s internal=%dx%d content=%d,%d,%d,%d panel=%dx%d\n",
            nxcompat_video_aspect_name(g_video.geometry.effective), iw, ih,
            g_video.geometry.content.x, g_video.geometry.content.y,
            g_video.geometry.content.w, g_video.geometry.content.h,
            panel_w, panel_h);
    if (internal_w) *internal_w = iw;
    if (internal_h) *internal_h = ih;
    return 1;
}

int st_stretch_active(void) { return g_active; }
void st_stretch_internal_size(int *w, int *h) { if (w) *w = g_internal_w; if (h) *h = g_internal_h; }
void st_stretch_panel_size(int *w, int *h) { if (w) *w = g_panel_w; if (h) *h = g_panel_h; }

void st_stretch_logical_to_panel(float sx, float sy, float *dx, float *dy)
{
    nxcompat_video_rect r = { 0, 0, g_panel_w, g_panel_h };
    if (g_video_ready)
        r = g_video.geometry.content;
    if (dx) *dx = (float)r.x + sx * (float)r.w / 1280.0f;
    if (dy) *dy = (float)r.y + sy * (float)r.h / 720.0f;
}

static int create_target(void)
{
    if (g_fbo)
        return 1;
    if (g_gl_dead || !resolve_gl()) {
        g_gl_dead = 1;
        return 0;
    }
    while (gl.get_error() != GL_NO_ERROR) ;
    GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0;
    gl.get_int(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    gl.get_int(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl.get_int(GL_RENDERBUFFER_BINDING, &prev_rb);

    gl.gen_textures(1, &g_color);
    gl.bind_texture(GL_TEXTURE_2D, g_color);
    gl.tex_image(GL_TEXTURE_2D, 0, GL_RGBA, g_internal_w, g_internal_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.gen_renderbuffers(1, &g_depth);
    gl.bind_renderbuffer(GL_RENDERBUFFER, g_depth);
    gl.renderbuffer_storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES,
                            g_internal_w, g_internal_h);
    gl.gen_framebuffers(1, &g_fbo);
    gl.bind_framebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.framebuffer_texture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_color, 0);
    gl.framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, g_depth);
    gl.framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, g_depth);
    GLenum status = gl.check_status(GL_FRAMEBUFFER);
    GLenum err = gl.get_error();
    gl.bind_texture(GL_TEXTURE_2D, (GLuint)prev_tex);
    gl.bind_renderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        nx_log("stretch: FBO %dx%d incompleto (status=%#x err=%#x); politica game",
               g_internal_w, g_internal_h, status, err);
        gl.bind_framebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        g_fbo = 0;
        g_gl_dead = 1;
        g_active = 0;
        return 0;
    }
    /* programa do present */
    GLuint vs = gl.create_shader(GL_VERTEX_SHADER);
    GLuint fs = gl.create_shader(GL_FRAGMENT_SHADER);
    const char *vsrc = VS, *fsrc = FS;
    GLint ok = 0;
    gl.shader_source(vs, 1, &vsrc, NULL); gl.compile_shader(vs);
    gl.get_shaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (ok) { gl.shader_source(fs, 1, &fsrc, NULL); gl.compile_shader(fs);
              gl.get_shaderiv(fs, GL_COMPILE_STATUS, &ok); }
    if (ok) {
        g_program = gl.create_program();
        gl.attach_shader(g_program, vs);
        gl.attach_shader(g_program, fs);
        gl.bind_attrib(g_program, 0, "p");
        gl.link_program(g_program);
        gl.get_programiv(g_program, GL_LINK_STATUS, &ok);
    }
    if (!ok) {
        nx_log("stretch: shader do present nao compilou/linkou; politica game");
        gl.bind_framebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        g_fbo = 0;
        g_gl_dead = 1;
        g_active = 0;
        return 0;
    }
    static const float quad[] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
    GLint prev_buf = 0;
    gl.get_int(GL_ARRAY_BUFFER_BINDING, &prev_buf);
    gl.gen_buffers(1, &g_vbo);
    gl.bind_buffer(GL_ARRAY_BUFFER, g_vbo);
    gl.buffer_data(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    gl.bind_buffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
    nx_log("stretch: alvo interno %dx%d pronto (fbo=%u)", g_internal_w,
           g_internal_h, g_fbo);
    /* O chamador (glBindFramebuffer(0) interceptado) liga o FBO em seguida. */
    return 1;
}

unsigned st_stretch_default_fbo(void)
{
    if (!g_active)
        return 0;
    if (!g_fbo && !create_target())
        return 0;
    return g_fbo;
}

void st_stretch_present(void)
{
    if (!g_active || !g_fbo)
        return;
    GLint old_program = 0, old_buffer = 0, old_tex = 0, old_active = GL_TEXTURE0;
    GLint old_enabled = 0, old_size = 4, old_type = GL_FLOAT, old_norm = 0,
          old_stride = 0, old_binding = 0, old_viewport[4] = { 0, 0, 0, 0 };
    GLfloat old_clear[4] = { 0.f, 0.f, 0.f, 0.f };
    void *old_pointer = NULL;
    GLboolean old_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    gl.get_int(GL_CURRENT_PROGRAM, &old_program);
    gl.get_int(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    gl.get_int(GL_ACTIVE_TEXTURE, &old_active);
    gl.active_texture(GL_TEXTURE0);
    gl.get_int(GL_TEXTURE_BINDING_2D, &old_tex);
    gl.get_int(GL_VIEWPORT, old_viewport);
    gl.get_float(GL_COLOR_CLEAR_VALUE, old_clear);
    gl.get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &old_enabled);
    gl.get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &old_size);
    gl.get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &old_type);
    gl.get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &old_norm);
    gl.get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &old_stride);
    gl.get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &old_binding);
    gl.get_vertex_attrib_ptr(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &old_pointer);
    gl.get_bool(GL_COLOR_WRITEMASK, old_mask);
    GLboolean had_blend = gl.is_enabled(GL_BLEND), had_depth = gl.is_enabled(GL_DEPTH_TEST),
              had_cull = gl.is_enabled(GL_CULL_FACE), had_scissor = gl.is_enabled(GL_SCISSOR_TEST),
              had_stencil = gl.is_enabled(GL_STENCIL_TEST);

    gl.bind_framebuffer(GL_FRAMEBUFFER, 0);
    gl.viewport(0, 0, g_panel_w, g_panel_h);
    gl.color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.disable(GL_BLEND); gl.disable(GL_DEPTH_TEST); gl.disable(GL_CULL_FACE);
    gl.disable(GL_SCISSOR_TEST); gl.disable(GL_STENCIL_TEST);
    if (g_video.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE) {
        gl.clear_color(0.f, 0.f, 0.f, 1.f);
        gl.clear(GL_COLOR_BUFFER_BIT);
    }
    int content_gl_y = g_panel_h - g_video.geometry.content.y -
                       g_video.geometry.content.h;
    gl.viewport(g_video.geometry.content.x, content_gl_y,
                g_video.geometry.content.w, g_video.geometry.content.h);
    gl.use_program(g_program);
    gl.bind_texture(GL_TEXTURE_2D, g_color);
    gl.uniform1i(gl.get_uniform(g_program, "s"), 0);
    gl.bind_buffer(GL_ARRAY_BUFFER, g_vbo);
    gl.enable_vertex_attrib(0);
    gl.vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    gl.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);

    static int readback_done;
    if (!readback_done) {
        GLint measured[4] = { 0, 0, 0, 0 };
        nxcompat_video_readback got;
        char line[256];
        gl.get_int(GL_VIEWPORT, measured);
        memset(&got, 0, sizeof got);
        got.api_version = 1u;
        got.drawable_w = g_panel_w;
        got.drawable_h = g_panel_h;
        got.content.x = measured[0];
        /* GL mede Y a partir de baixo; nxcompat publica coordenadas de tela
         * a partir do topo (a mesma base do touch/cursor). */
        got.content.y = g_panel_h - measured[1] - measured[3];
        got.content.w = measured[2];
        got.content.h = measured[3];
        got.effective = g_video.geometry.effective;
        got.cas_generation = g_video.cas_generation;
        if (nxcompat_video_readback_check(&g_video, &got, line, sizeof line) == 0)
            fprintf(stderr, "[st/video] %s\n", line);
        else
            nx_die("video readback mismatch: %s", line);
        readback_done = 1;
    }

    /* devolve o estado da engine (o binding fica em 0 de proposito) */
    gl.bind_buffer(GL_ARRAY_BUFFER, (GLuint)old_binding);
    if (old_enabled)
        gl.vertex_attrib_pointer(0, old_size, (GLenum)old_type, (GLboolean)old_norm,
                                 old_stride, old_pointer);
    else
        gl.disable_vertex_attrib(0);
    gl.bind_buffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
    gl.bind_texture(GL_TEXTURE_2D, (GLuint)old_tex);
    gl.active_texture((GLenum)old_active);
    gl.use_program((GLuint)old_program);
    gl.clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    gl.color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    if (had_blend) gl.enable(GL_BLEND);
    if (had_depth) gl.enable(GL_DEPTH_TEST);
    if (had_cull) gl.enable(GL_CULL_FACE);
    if (had_scissor) gl.enable(GL_SCISSOR_TEST);
    if (had_stencil) gl.enable(GL_STENCIL_TEST);
    gl.viewport(0, 0, g_panel_w, g_panel_h);
    (void)old_viewport; /* o viewport do painel fica para a seta; a engine
                           reprograma o seu no proximo quadro */
}

void st_stretch_after_present(void)
{
    if (!g_active || !g_fbo)
        return;
    gl.bind_framebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.viewport(0, 0, g_internal_w, g_internal_h);
}
