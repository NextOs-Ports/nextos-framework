/*
 * egl.c -- EGL bridge for a Unity build that contains GLES2 and GLES3 paths.
 *
 * This Unity build advertises GLES2 and carries an ordered graphics API list.
 * The bridge first submits the guest's exact EGL request.  Only an observed
 * failure permits one bounded GLES2 retry; no device/firmware name and no
 * unconditional attribute rewrite chooses the result.
 *
 * Everything else forwards to the system EGL, which on this device is the
 * Mali fbdev driver.  We never set SDL_VIDEODRIVER and never pick a display
 * ourselves; EGL_DEFAULT_DISPLAY is what the driver wants on fbdev.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_elf.h"
#include "gb.h"
#include "egl_sdl.h"
#include "etc2_decode.h"
#include "framework_bridge.h"

/* Attribute names that only exist for ES3 contexts. */
#define EGL_CONTEXT_MINOR_VERSION_KHR      0x30FB
#define EGL_OPENGL_ES3_BIT_KHR             0x00000040

static void *libegl;

static void *sys(const char *n)
{
    if (!libegl) {
        libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!libegl)
            libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libegl)
            nx_die("cannot open the system libEGL: %s", dlerror());
    }
    void *f = dlsym(libegl, n);
    if (!f)
        nx_log("system EGL has no %s", n);
    return f;
}

/* GL entry points come from the driver blob, which on this image is what every
 * libGLES* name links to.  Unity does not import them through the PLT: it builds
 * its own function table at runtime, and an entry it cannot resolve stays NULL
 * and is called anyway -- glGetString(GL_EXTENSIONS) jumping to 0 is what a
 * missing lookup looks like from the crash.  Note the driver's
 * eglGetProcAddress answers for extensions only, so the core names have to come
 * from here. */
static void *libgl;

static void *gl_raw(const char *name)
{
    if (!name || name[0] != 'g' || name[1] != 'l')
        return NULL;
    if (gb_sdl_video_active())
        return gb_sdl_gl_proc(name);
    if (!libgl) {
        static const char *const cands[] = {
            "libGLESv2.so.2", "libGLESv2.so", "libGLESv3.so", "libmali.so",
        };
        for (size_t i = 0; i < sizeof cands / sizeof *cands && !libgl; i++)
            libgl = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        if (!libgl) {
            nx_log("cannot open a system GLES library: %s", dlerror());
            return NULL;
        }
    }
    return dlsym(libgl, name);
}

static GLenum shader_types[256];
static unsigned long shader_sources;
static unsigned long shader_compiles;
static unsigned long program_links;
static unsigned long draw_calls;
static unsigned long texture_images;
static unsigned long texture_sub_images;
static unsigned long texture_binds;
static unsigned long texture_parameters;
static unsigned long buffer_uploads;
static unsigned long framebuffer_calls;
static GLenum active_texture_unit = GL_TEXTURE0;
static GLuint bound_texture_2d;
static GLuint bound_array_buffer;
static GLuint bound_element_array_buffer;
static GLuint current_program;

typedef struct {
    GLuint program;
    GLint location;
    char name[64];
} gb_uniform_name;

static gb_uniform_name uniform_names[128];
static size_t uniform_name_count;

static const char *uniform_label(GLint location)
{
    for (size_t i = uniform_name_count; i > 0; i--)
        if (uniform_names[i - 1].location == location)
            return uniform_names[i - 1].name;
    return "?";
}

static void remember_uniform(GLuint program, GLint location, const char *name)
{
    if (location < 0 || !name || uniform_name_count >=
                                  sizeof uniform_names / sizeof *uniform_names)
        return;
    gb_uniform_name *entry = &uniform_names[uniform_name_count++];
    entry->program = program;
    entry->location = location;
    snprintf(entry->name, sizeof entry->name, "%s", name);
}

static int trace_texture_call(unsigned long call, GLsizei width,
                              GLsizei height)
{
    return gb_trace_gl && (call <= 160 || width >= 512 || height >= 512);
}

static void trace_buffer_payload(const void *data, GLsizeiptr size)
{
    uint32_t words[4] = { 0, 0, 0, 0 };
    if (!data || size <= 0) {
        fprintf(stderr, " data=null");
        return;
    }
    size_t take = (size_t)size < sizeof words ? (size_t)size : sizeof words;
    memcpy(words, data, take);
    fprintf(stderr, " data=%08x,%08x,%08x,%08x",
            words[0], words[1], words[2], words[3]);
}

typedef struct {
    GLuint framebuffer;
} gb_gl_state;

static gb_gl_state gl_state;

static void my_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    static void (*real)(GLenum, GLuint);
    if (!real) {
        real = gl_raw("glBindFramebuffer");
        if (!real)
            real = gl_raw("glBindFramebufferOES");
    }
    if (real)
        real(target, framebuffer);
    if (target == GL_FRAMEBUFFER || target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */)
        gl_state.framebuffer = framebuffer;
    framebuffer_calls++;
    if (gb_trace_gl && framebuffer_calls <= 160)
        fprintf(stderr, "[gunbrick/gl] fbo-bind #%lu target=%#x framebuffer=%u\n",
                framebuffer_calls, target, framebuffer);
}

static void my_glDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{
    static void (*real)(GLsizei, const GLuint *);
    GLuint current = gl_state.framebuffer;
    if (!real) {
        real = gl_raw("glDeleteFramebuffers");
        if (!real)
            real = gl_raw("glDeleteFramebuffersOES");
    }
    if (real)
        real(count, framebuffers);
    if (current && count > 0 && framebuffers) {
        for (GLsizei i = 0; i < count; i++) {
            if (framebuffers[i] == current) {
                gl_state.framebuffer = 0;
                break;
            }
        }
    }
}

/* Gunbrick publishes no cursor or synthetic keyboard overlay.  The native
 * Unity input lifecycle remains the sole owner of game UI rendering. */

static void remember_shader(GLuint shader, GLenum type)
{
    if (shader < sizeof shader_types / sizeof *shader_types)
        shader_types[shader] = type;
}

static const char *shader_stage(GLuint shader)
{
    GLenum type = shader < sizeof shader_types / sizeof *shader_types
                    ? shader_types[shader] : 0;
    return type == GL_VERTEX_SHADER ? "vertex"
         : type == GL_FRAGMENT_SHADER ? "fragment" : "unknown";
}

static GLuint my_glCreateShader(GLenum type)
{
    static GLuint (*real)(GLenum);
    if (!real)
        real = gl_raw("glCreateShader");
    GLuint shader = real(type);
    remember_shader(shader, type);
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] create %s shader=%u\n",
                type == GL_VERTEX_SHADER ? "vertex"
                : type == GL_FRAGMENT_SHADER ? "fragment" : "unknown",
                shader);
    return shader;
}

static void my_glShaderSource(GLuint shader, GLsizei count,
                              const GLchar *const *strings,
                              const GLint *lengths)
{
    static void (*real)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    if (!real)
        real = gl_raw("glShaderSource");
    shader_sources++;
    if (gb_trace_gl) {
        size_t total = 0;
        char preview[161];
        size_t used = 0;
        for (GLsizei i = 0; i < count; i++) {
            size_t n = lengths && lengths[i] >= 0
                         ? (size_t)lengths[i] : strlen(strings[i]);
            total += n;
            if (used < sizeof preview - 1) {
                size_t take = n;
                if (take > sizeof preview - 1 - used)
                    take = sizeof preview - 1 - used;
                memcpy(preview + used, strings[i], take);
                used += take;
            }
        }
        preview[used] = '\0';
        for (size_t i = 0; i < used; i++)
            if (preview[i] == '\n' || preview[i] == '\r')
                preview[i] = ' ';
        fprintf(stderr,
                "[gunbrick/gl] source #%lu shader=%u stage=%s bytes=%zu: %.160s\n",
                shader_sources, shader, shader_stage(shader), total, preview);
        if (getenv("GB_GLSOURCE")) {
            fprintf(stderr, "[gunbrick/gl/source-begin] shader=%u stage=%s\n",
                    shader, shader_stage(shader));
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                fwrite(strings[i], 1, n, stderr);
            }
            fprintf(stderr, "\n[gunbrick/gl/source-end] shader=%u\n", shader);
        }
    }
    real(shader, count, strings, lengths);
}

static void my_glCompileShader(GLuint shader)
{
    static void (*compile)(GLuint);
    static void (*getiv)(GLuint, GLenum, GLint *);
    static void (*getlog)(GLuint, GLsizei, GLsizei *, GLchar *);
    if (!compile)
        compile = gl_raw("glCompileShader");
    if (!getiv)
        getiv = gl_raw("glGetShaderiv");
    if (!getlog)
        getlog = gl_raw("glGetShaderInfoLog");
    compile(shader);
    shader_compiles++;
    GLint ok = GL_FALSE, loglen = 0;
    getiv(shader, GL_COMPILE_STATUS, &ok);
    getiv(shader, GL_INFO_LOG_LENGTH, &loglen);
    if (gb_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(shader, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr,
                "[gunbrick/gl] compile #%lu shader=%u stage=%s ok=%d log=%s\n",
                shader_compiles, shader, shader_stage(shader), ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
}

static void my_glLinkProgram(GLuint program)
{
    static void (*link)(GLuint);
    static void (*getiv)(GLuint, GLenum, GLint *);
    static void (*getlog)(GLuint, GLsizei, GLsizei *, GLchar *);
    if (!link)
        link = gl_raw("glLinkProgram");
    if (!getiv)
        getiv = gl_raw("glGetProgramiv");
    if (!getlog)
        getlog = gl_raw("glGetProgramInfoLog");
    link(program);
    program_links++;
    GLint ok = GL_FALSE, loglen = 0;
    getiv(program, GL_LINK_STATUS, &ok);
    getiv(program, GL_INFO_LOG_LENGTH, &loglen);
    if (gb_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(program, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr, "[gunbrick/gl] link #%lu program=%u ok=%d log=%s\n",
                program_links, program, ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
}

static void my_glUseProgram(GLuint program)
{
    static void (*real)(GLuint);
    current_program = program;
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] use-program %u\n", program);
    if (!real)
        real = gl_raw("glUseProgram");
    if (real)
        real(program);
}

static GLint my_glGetAttribLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetAttribLocation");
    GLint location = real ? real(program, name) : -1;
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] attrib program=%u location=%d name=%s\n",
                program, location, name ? name : "(null)");
    return location;
}

static void my_glBindBuffer(GLenum target, GLuint buffer)
{
    static void (*real)(GLenum, GLuint);
    if (target == GL_ARRAY_BUFFER)
        bound_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        bound_element_array_buffer = buffer;
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] buffer-bind target=%#x buffer=%u\n",
                target, buffer);
    if (!real)
        real = gl_raw("glBindBuffer");
    if (real)
        real(target, buffer);
}

static void my_glBufferData(GLenum target, GLsizeiptr size, const void *data,
                            GLenum usage)
{
    static void (*real)(GLenum, GLsizeiptr, const void *, GLenum);
    buffer_uploads++;
    if (gb_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[gunbrick/gl] buffer-data #%lu target=%#x buffer=%u bytes=%ld "
                "usage=%#x",
                buffer_uploads, target, buffer, (long)size, usage);
        trace_buffer_payload(data, size);
        fputc('\n', stderr);
    }
    if (!real)
        real = gl_raw("glBufferData");
    if (!real)
        return;
    real(target, size, data, usage);
}

static void my_glBufferSubData(GLenum target, GLintptr offset,
                               GLsizeiptr size, const void *data)
{
    static void (*real)(GLenum, GLintptr, GLsizeiptr, const void *);
    buffer_uploads++;
    if (gb_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[gunbrick/gl] buffer-sub #%lu target=%#x buffer=%u offset=%ld "
                "bytes=%ld",
                buffer_uploads, target, buffer, (long)offset, (long)size);
        trace_buffer_payload(data, size);
        fputc('\n', stderr);
    }
    if (!real)
        real = gl_raw("glBufferSubData");
    if (real)
        real(target, offset, size, data);
}

static void my_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                     GLboolean normalized, GLsizei stride,
                                     const void *pointer)
{
    static void (*real)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                        const void *);
    if (gb_trace_gl)
        fprintf(stderr,
                "[gunbrick/gl] attrib-pointer index=%u size=%d type=%#x norm=%u "
                "stride=%d pointer=%p array-buffer=%u\n",
                index, size, type, !!normalized, stride, pointer,
                bound_array_buffer);
    if (!real)
        real = gl_raw("glVertexAttribPointer");
    if (real)
        real(index, size, type, normalized, stride, pointer);
}

static void my_glEnableVertexAttribArray(GLuint index)
{
    static void (*real)(GLuint);
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] attrib-enable index=%u\n", index);
    if (!real)
        real = gl_raw("glEnableVertexAttribArray");
    if (real)
        real(index);
}

static void my_glDisableVertexAttribArray(GLuint index)
{
    static void (*real)(GLuint);
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] attrib-disable index=%u\n", index);
    if (!real)
        real = gl_raw("glDisableVertexAttribArray");
    if (real)
        real(index);
}

static void my_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                      GLenum textarget, GLuint texture,
                                      GLint level)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint, GLint);
    if (gb_trace_gl)
        fprintf(stderr,
                "[gunbrick/gl] fbo-texture fbo=%u target=%#x attachment=%#x "
                "textarget=%#x texture=%u level=%d\n",
                gl_state.framebuffer, target, attachment, textarget, texture,
                level);
    if (!real)
        real = gl_raw("glFramebufferTexture2D");
    if (real)
        real(target, attachment, textarget, texture, level);
}

static void my_glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                         GLenum renderbuffer_target,
                                         GLuint renderbuffer)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint);
    if (gb_trace_gl)
        fprintf(stderr,
                "[gunbrick/gl] fbo-renderbuffer fbo=%u target=%#x attachment=%#x "
                "renderbuffer-target=%#x renderbuffer=%u\n",
                gl_state.framebuffer, target, attachment,
                renderbuffer_target, renderbuffer);
    if (!real)
        real = gl_raw("glFramebufferRenderbuffer");
    if (real)
        real(target, attachment, renderbuffer_target, renderbuffer);
}

static void my_glRenderbufferStorage(GLenum target, GLenum internal_format,
                                     GLsizei width, GLsizei height)
{
    static void (*real)(GLenum, GLenum, GLsizei, GLsizei);
    if (gb_trace_gl)
        fprintf(stderr,
                "[gunbrick/gl] renderbuffer-storage target=%#x internal=%#x "
                "size=%dx%d\n",
                target, internal_format, width, height);
    if (!real)
        real = gl_raw("glRenderbufferStorage");
    if (real)
        real(target, internal_format, width, height);
}

static GLenum my_glCheckFramebufferStatus(GLenum target)
{
    static GLenum (*real)(GLenum);
    if (!real)
        real = gl_raw("glCheckFramebufferStatus");
    GLenum status = real ? real(target) : 0;
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] fbo-status fbo=%u target=%#x status=%#x\n",
                gl_state.framebuffer, target, status);
    return status;
}

static void trace_current_fbo_status(void)
{
    if (!gb_trace_gl || gl_state.framebuffer == 0 || draw_calls > 20)
        return;
    static GLenum (*check)(GLenum);
    if (!check)
        check = gl_raw("glCheckFramebufferStatus");
    if (check)
        fprintf(stderr, "[gunbrick/gl] pre-draw fbo=%u status=%#x\n",
                gl_state.framebuffer, check(GL_FRAMEBUFFER));
}

static GLint my_glGetUniformLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetUniformLocation");
    GLint location = real ? real(program, name) : -1;
    remember_uniform(program, location, name);
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] uniform program=%u location=%d name=%s\n",
                program, location, name ? name : "(null)");
    return location;
}

static void my_glUniform1i(GLint location, GLint value)
{
    static void (*real)(GLint, GLint);
    if (!real)
        real = gl_raw("glUniform1i");
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] uniform1i location=%d name=%s value=%d\n",
                location, uniform_label(location), value);
    if (real)
        real(location, value);
}

static void my_glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z,
                           GLfloat w)
{
    static void (*real)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    if (!real)
        real = gl_raw("glUniform4f");
    if (gb_trace_gl)
        fprintf(stderr,
                "[gunbrick/gl] uniform4f location=%d name=%s "
                "value=%.9g,%.9g,%.9g,%.9g\n",
                location, uniform_label(location), x, y, z, w);
    if (real)
        real(location, x, y, z, w);
}

static void my_glUniform4fv(GLint location, GLsizei count,
                            const GLfloat *value)
{
    static void (*real)(GLint, GLsizei, const GLfloat *);
    if (!real)
        real = gl_raw("glUniform4fv");
    if (gb_trace_gl && value && count > 0) {
        fprintf(stderr,
                "[gunbrick/gl] uniform4fv location=%d name=%s count=%d "
                "first=%.9g,%.9g,%.9g,%.9g\n",
                location, uniform_label(location), count,
                value[0], value[1], value[2], value[3]);
        if (count == 4)
            fprintf(stderr,
                    "[gunbrick/gl] uniform4fv-matrix location=%d name=%s "
                    "rows=[%.9g %.9g %.9g %.9g] [%.9g %.9g %.9g %.9g] "
                    "[%.9g %.9g %.9g %.9g] [%.9g %.9g %.9g %.9g]\n",
                    location, uniform_label(location),
                    value[0], value[1], value[2], value[3],
                    value[4], value[5], value[6], value[7],
                    value[8], value[9], value[10], value[11],
                    value[12], value[13], value[14], value[15]);
    }
    if (real)
        real(location, count, value);
}

static void my_glActiveTexture(GLenum texture)
{
    static void (*real)(GLenum);
    active_texture_unit = texture;
    if (!real)
        real = gl_raw("glActiveTexture");
    if (real)
        real(texture);
}

static void my_glBindTexture(GLenum target, GLuint texture)
{
    static void (*real)(GLenum, GLuint);
    texture_binds++;
    if (target == GL_TEXTURE_2D)
        bound_texture_2d = texture;
    if (gb_trace_gl && texture_binds <= 160)
        fprintf(stderr,
                "[gunbrick/gl] tex-bind #%lu unit=%#x target=%#x texture=%u\n",
                texture_binds, active_texture_unit, target, texture);
    if (!real)
        real = gl_raw("glBindTexture");
    if (real)
        real(target, texture);
}

static void my_glPixelStorei(GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLint);
    if (gb_trace_gl)
        fprintf(stderr, "[gunbrick/gl] pixel-store pname=%#x value=%d\n",
                pname, value);
    if (!real)
        real = gl_raw("glPixelStorei");
    if (real)
        real(pname, value);
}

static void my_glTexParameteri(GLenum target, GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLenum, GLint);
    texture_parameters++;
    if (gb_trace_gl && texture_parameters <= 240)
        fprintf(stderr,
                "[gunbrick/gl] tex-param #%lu unit=%#x texture=%u target=%#x "
                "pname=%#x value=%#x\n",
                texture_parameters, active_texture_unit, bound_texture_2d,
                target, pname, value);
    if (!real)
        real = gl_raw("glTexParameteri");
    if (real)
        real(target, pname, value);
}

static void my_glTexImage2D(GLenum target, GLint level, GLint internal_format,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                        GLenum, const void *);
    texture_images++;
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[gunbrick/gl] tex-image #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d border=%d format=%#x "
                "type=%#x data=%s\n",
                texture_images, active_texture_unit, bound_texture_2d, target,
                level, internal_format, width, height, border, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexImage2D");
    if (real)
        real(target, level, internal_format, width, height, border, format,
             type, data);
}

static void my_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                               GLsizei width, GLsizei height, GLenum format,
                               GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                        GLenum, const void *);
    texture_sub_images++;
    if (trace_texture_call(texture_sub_images, width, height))
        fprintf(stderr,
                "[gunbrick/gl] tex-sub #%lu unit=%#x texture=%u target=%#x "
                "level=%d xy=%d,%d size=%dx%d format=%#x type=%#x data=%s\n",
                texture_sub_images, active_texture_unit, bound_texture_2d,
                target, level, x, y, width, height, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexSubImage2D");
    if (real)
        real(target, level, x, y, width, height, format, type, data);
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real)
        real = gl_raw("glDrawArrays");
    draw_calls++;
    if (gb_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr,
                "[gunbrick/gl] draw #%lu arrays mode=%#x first=%d count=%d "
                "program=%u fbo=%u vbo=%u\n",
                draw_calls, mode, first, count, current_program,
                gl_state.framebuffer, bound_array_buffer);
    trace_current_fbo_status();
    real(mode, first, count);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices)
{
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real)
        real = gl_raw("glDrawElements");
    draw_calls++;
    if (gb_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr,
                "[gunbrick/gl] draw #%lu elements mode=%#x count=%d type=%#x "
                "indices=%p program=%u fbo=%u vbo=%u ebo=%u\n",
                draw_calls, mode, count, type, indices, current_program,
                gl_state.framebuffer, bound_array_buffer,
                bound_element_array_buffer);
    trace_current_fbo_status();
    real(mode, count, type, indices);
}

static int native_etc2_support(void)
{
    static int known = -1;
    if (known >= 0)
        return known;
    const GLubyte *(*get_string)(GLenum) = gl_raw("glGetString");
    const char *version = get_string ? (const char *)get_string(GL_VERSION) : NULL;
    const char *extensions = get_string
                           ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    known = (version && strstr(version, "OpenGL ES 3")) ||
            (extensions && (strstr(extensions, "GL_OES_compressed_ETC2") ||
                            strstr(extensions, "GL_ARB_ES3_compatibility")));
    nx_log("ETC2 uploads: %s", known ? "native" : "software RGBA fallback");
    return known;
}

static int is_etc2(GLenum format)
{
    return format >= 0x9274 && format <= 0x9279;
}

static void my_glCompressedTexImage2D(GLenum target, GLint level,
                                      GLenum internal_format, GLsizei width,
                                      GLsizei height, GLint border,
                                      GLsizei image_size, const void *data)
{
    static void (*compressed)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                              GLsizei, const void *);
    static void (*plain)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                         GLenum, const void *);
    texture_images++;
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[gunbrick/gl] tex-compressed #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d bytes=%d data=%s\n",
                texture_images, active_texture_unit, bound_texture_2d, target,
                level, internal_format, width, height, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexImage2D");
    if (!plain) plain = gl_raw("glTexImage2D");
    if (data && is_etc2(internal_format) && !native_etc2_support()) {
        unsigned char *rgba = etc2_decode_rgba(internal_format, width, height,
                                                data, image_size);
        if (rgba && plain) {
            plain(target, level, GL_RGBA, width, height, border, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
    }
    if (compressed)
        compressed(target, level, internal_format, width, height, border,
                   image_size, data);
}

static void my_glCompressedTexSubImage2D(GLenum target, GLint level,
                                         GLint x, GLint y, GLsizei width,
                                         GLsizei height, GLenum format,
                                         GLsizei image_size, const void *data)
{
    static void (*compressed)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
                              GLenum, GLsizei, const void *);
    static void (*plain)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                         GLenum, const void *);
    texture_sub_images++;
    if (trace_texture_call(texture_sub_images, width, height))
        fprintf(stderr,
                "[gunbrick/gl] tex-compressed-sub #%lu unit=%#x texture=%u "
                "target=%#x level=%d xy=%d,%d size=%dx%d format=%#x "
                "bytes=%d data=%s\n",
                texture_sub_images, active_texture_unit, bound_texture_2d,
                target, level, x, y, width, height, format, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexSubImage2D");
    if (!plain) plain = gl_raw("glTexSubImage2D");
    if (data && is_etc2(format) && !native_etc2_support()) {
        unsigned char *rgba = etc2_decode_rgba(format, width, height, data,
                                                image_size);
        if (rgba && plain) {
            plain(target, level, x, y, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
    }
    if (compressed)
        compressed(target, level, x, y, width, height, format, image_size,
                   data);
}

void *gb_gl_sym(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "glBindFramebuffer") == 0 ||
        strcmp(name, "glBindFramebufferOES") == 0)
        return my_glBindFramebuffer;
    if (strcmp(name, "glDeleteFramebuffers") == 0 ||
        strcmp(name, "glDeleteFramebuffersOES") == 0)
        return my_glDeleteFramebuffers;
    if (strcmp(name, "glCreateShader") == 0)
        return my_glCreateShader;
    if (strcmp(name, "glShaderSource") == 0)
        return my_glShaderSource;
    if (strcmp(name, "glCompileShader") == 0)
        return my_glCompileShader;
    if (strcmp(name, "glLinkProgram") == 0)
        return my_glLinkProgram;
    if (strcmp(name, "glUseProgram") == 0)
        return my_glUseProgram;
    if (strcmp(name, "glGetAttribLocation") == 0)
        return my_glGetAttribLocation;
    if (strcmp(name, "glBindBuffer") == 0)
        return my_glBindBuffer;
    if (strcmp(name, "glBufferData") == 0)
        return my_glBufferData;
    if (strcmp(name, "glBufferSubData") == 0)
        return my_glBufferSubData;
    if (strcmp(name, "glVertexAttribPointer") == 0)
        return my_glVertexAttribPointer;
    if (strcmp(name, "glEnableVertexAttribArray") == 0)
        return my_glEnableVertexAttribArray;
    if (strcmp(name, "glDisableVertexAttribArray") == 0)
        return my_glDisableVertexAttribArray;
    if (strcmp(name, "glFramebufferTexture2D") == 0)
        return my_glFramebufferTexture2D;
    if (strcmp(name, "glFramebufferRenderbuffer") == 0)
        return my_glFramebufferRenderbuffer;
    if (strcmp(name, "glRenderbufferStorage") == 0)
        return my_glRenderbufferStorage;
    if (strcmp(name, "glCheckFramebufferStatus") == 0)
        return my_glCheckFramebufferStatus;
    if (strcmp(name, "glGetUniformLocation") == 0)
        return my_glGetUniformLocation;
    if (strcmp(name, "glUniform1i") == 0)
        return my_glUniform1i;
    if (strcmp(name, "glUniform4f") == 0)
        return my_glUniform4f;
    if (strcmp(name, "glUniform4fv") == 0)
        return my_glUniform4fv;
    if (strcmp(name, "glActiveTexture") == 0)
        return my_glActiveTexture;
    if (strcmp(name, "glBindTexture") == 0)
        return my_glBindTexture;
    if (strcmp(name, "glPixelStorei") == 0)
        return my_glPixelStorei;
    if (strcmp(name, "glTexParameteri") == 0)
        return my_glTexParameteri;
    if (strcmp(name, "glTexImage2D") == 0)
        return my_glTexImage2D;
    if (strcmp(name, "glTexSubImage2D") == 0)
        return my_glTexSubImage2D;
    if (strcmp(name, "glDrawArrays") == 0)
        return my_glDrawArrays;
    if (strcmp(name, "glDrawElements") == 0)
        return my_glDrawElements;
    if (strcmp(name, "glCompressedTexImage2D") == 0)
        return my_glCompressedTexImage2D;
    if (strcmp(name, "glCompressedTexSubImage2D") == 0)
        return my_glCompressedTexSubImage2D;
    return gl_raw(name);
}

#define SYS(name, ret, ...) \
    static ret (*p_##name)(__VA_ARGS__); \
    static ret r_##name(__VA_ARGS__)

/* --- the two calls we rewrite ------------------------------------------- */

static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                       EGLint, EGLint *);

static EGLBoolean my_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib,
                                     EGLConfig *cfgs, EGLint n, EGLint *num)
{
    EGLint copy[64];
    size_t k = 0;
    int can_retry = 0;
    EGLBoolean result;

    if (!p_eglChooseConfig)
        p_eglChooseConfig = sys("eglChooseConfig");
    if (!p_eglChooseConfig)
        return EGL_FALSE;
    result = p_eglChooseConfig(dpy, attrib, cfgs, n, num);
    if (result == EGL_TRUE && (!num || *num > 0))
        return result;

    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 58; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_RENDERABLE_TYPE && (val & EGL_OPENGL_ES3_BIT_KHR)) {
                val = (val & ~EGL_OPENGL_ES3_BIT_KHR) | EGL_OPENGL_ES2_BIT;
                can_retry = 1;
            }
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    copy[k] = EGL_NONE;
    if (!can_retry || !attrib || k >= 58)
        return result;
    if (num)
        *num = 0;
    nx_log("eglChooseConfig: exact request unavailable; bounded GLES2 retry");
    result = p_eglChooseConfig(dpy, copy, cfgs, n, num);
    if (result == EGL_TRUE && num && *num > 0 && cfgs) {
        EGLint st = -1, dp = -1;
        EGLBoolean (*getattr_)(EGLDisplay, EGLConfig, EGLint, EGLint *) =
            sys("eglGetConfigAttrib");
        if (getattr_) {
            getattr_(dpy, cfgs[0], EGL_STENCIL_SIZE, &st);
            getattr_(dpy, cfgs[0], EGL_DEPTH_SIZE, &dp);
            nx_log("eglChooseConfig: config escolhido stencil=%d depth=%d",
                   (int)st, (int)dp);
        }
    }
    return result;
}

static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);
static EGLConfig raw_context_config;

static EGLContext my_eglCreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const EGLint *attrib)
{
    EGLint copy[32];
    size_t k = 0;
    int can_retry = 0;
    EGLContext context;

    if (!p_eglCreateContext)
        p_eglCreateContext = sys("eglCreateContext");
    if (!p_eglCreateContext)
        return EGL_NO_CONTEXT;
    context = p_eglCreateContext(dpy, cfg, share, attrib);
    if (context != EGL_NO_CONTEXT) {
        raw_context_config = cfg;
        return context;
    }

    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 28; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_CONTEXT_CLIENT_VERSION && val > 2) {
                val = 2;
                can_retry = 1;
            }
            if (name == EGL_CONTEXT_MINOR_VERSION_KHR)
                continue;                       /* ES2 has no minor version */
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    copy[k] = EGL_NONE;
    if (!can_retry || !attrib || k >= 28)
        return EGL_NO_CONTEXT;
    nx_log("eglCreateContext: exact request failed; bounded GLES2 retry");
    context = p_eglCreateContext(dpy, cfg, share, copy);
    if (context != EGL_NO_CONTEXT)
        raw_context_config = cfg;
    return context;
}

static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                      EGLContext);

static const char *raw_gl_string(GLenum name)
{
    const GLubyte *(*get_string)(GLenum) = gl_raw("glGetString");
    const GLubyte *value = get_string ? get_string(name) : NULL;
    return value ? (const char *)value : "";
}

static EGLBoolean my_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                    EGLSurface read, EGLContext context)
{
    gb_graphics_evidence evidence;
    EGLBoolean (*get_config)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    EGLBoolean (*query_surface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    const char *(*query_string)(EGLDisplay, EGLint);
    EGLint width = 0, height = 0, render_buffer = 0;
    EGLint config_id = 0, red = 0, green = 0, blue = 0, alpha = 0;
    EGLint depth = 0, stencil = 0, renderable = 0, surface_type = 0;
    EGLBoolean result;

    if (!p_eglMakeCurrent)
        p_eglMakeCurrent = sys("eglMakeCurrent");
    if (!p_eglMakeCurrent)
        return EGL_FALSE;
    result = p_eglMakeCurrent(dpy, draw, read, context);
    if (result != EGL_TRUE || context == EGL_NO_CONTEXT ||
        draw == EGL_NO_SURFACE)
        return result;

    get_config = sys("eglGetConfigAttrib");
    query_surface = sys("eglQuerySurface");
    query_string = sys("eglQueryString");
    if (!raw_context_config || !get_config || !query_surface ||
        !query_string ||
        query_surface(dpy, draw, EGL_WIDTH, &width) != EGL_TRUE ||
        query_surface(dpy, draw, EGL_HEIGHT, &height) != EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_CONFIG_ID, &config_id) !=
            EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_RED_SIZE, &red) != EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_GREEN_SIZE, &green) !=
            EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_BLUE_SIZE, &blue) != EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_ALPHA_SIZE, &alpha) !=
            EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_DEPTH_SIZE, &depth) !=
            EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_STENCIL_SIZE, &stencil) !=
            EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_RENDERABLE_TYPE,
                   &renderable) != EGL_TRUE ||
        get_config(dpy, raw_context_config, EGL_SURFACE_TYPE,
                   &surface_type) != EGL_TRUE) {
        (void)p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
        return EGL_FALSE;
    }
    (void)query_surface(dpy, draw, EGL_RENDER_BUFFER, &render_buffer);
    memset(&evidence, 0, sizeof(evidence));
    evidence.window_width = width;
    evidence.window_height = height;
    evidence.drawable_width = width;
    evidence.drawable_height = height;
    evidence.red_bits = red;
    evidence.green_bits = green;
    evidence.blue_bits = blue;
    evidence.alpha_bits = alpha;
    evidence.depth_bits = depth;
    evidence.stencil_bits = stencil;
    evidence.double_buffer = render_buffer == EGL_BACK_BUFFER;
    evidence.egl_config_id = config_id;
    evidence.egl_renderable_type = renderable;
    evidence.egl_surface_type = surface_type;
    evidence.backend = "raw-egl";
    evidence.gl_vendor = raw_gl_string(GL_VENDOR);
    evidence.gl_renderer = raw_gl_string(GL_RENDERER);
    evidence.gl_version = raw_gl_string(GL_VERSION);
    evidence.glsl_version = raw_gl_string(GL_SHADING_LANGUAGE_VERSION);
    evidence.gl_extensions = raw_gl_string(GL_EXTENSIONS);
    evidence.egl_vendor = query_string(dpy, EGL_VENDOR);
    evidence.egl_version = query_string(dpy, EGL_VERSION);
    evidence.egl_client_apis = query_string(dpy, EGL_CLIENT_APIS);
    if (gb_framework_publish_graphics(&evidence) != 0) {
        (void)p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

/* Unity asks for the surface it should render into.  On fbdev the native
 * window is the framebuffer itself, so hand the driver what it expects rather
 * than our ANativeWindow shim. */
static EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig,
                                              EGLNativeWindowType,
                                              const EGLint *);

static EGLSurface my_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg,
                                            EGLNativeWindowType win,
                                            const EGLint *attrib)
{
    extern void *gb_native_window(void);
    if (!p_eglCreateWindowSurface)
        p_eglCreateWindowSurface = sys("eglCreateWindowSurface");
    EGLNativeWindowType real = (EGLNativeWindowType)gb_native_window();
    nx_log("eglCreateWindowSurface: game win=%p -> native %p", (void *)win,
           (void *)real);
    return p_eglCreateWindowSurface(dpy, cfg, real, attrib);
}

static EGLDisplay (*p_eglGetDisplay)(EGLNativeDisplayType);

static EGLDisplay my_eglGetDisplay(EGLNativeDisplayType d)
{
    (void)d;
    if (!p_eglGetDisplay)
        p_eglGetDisplay = sys("eglGetDisplay");
    return p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static void clear_cursor_runs(const uint16_t *mask, int rows, int x, int y,
                              int scale, int width, int height,
                              void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                              void (*clear)(GLbitfield))
{
    for (int row = 0; row < rows; row++) {
        uint16_t bits = mask[row];
        for (int col = 0; col < 16;) {
            while (col < 16 && !(bits & (UINT16_C(1) << col)))
                col++;
            int start = col;
            while (col < 16 && (bits & (UINT16_C(1) << col)))
                col++;
            if (start == col)
                continue;
            int sx = x + start * scale;
            int top = y + row * scale;
            int sw = (col - start) * scale;
            int sh = scale;
            if (sx < 0) {
                sw += sx;
                sx = 0;
            }
            if (top < 0) {
                sh += top;
                top = 0;
            }
            if (sx + sw > width)
                sw = width - sx;
            if (top + sh > height)
                sh = height - top;
            if (sw <= 0 || sh <= 0)
                continue;
            scissor(sx, height - top - sh, sw, sh);
            clear(GL_COLOR_BUFFER_BIT);
        }
    }
}

/* A small polished pixel cursor: cream face, near-black outline and
 * wine-red drop shadow.  It is drawn with scissored color clears immediately
 * before swap, avoiding shaders/textures and preserving Unity's GL program. */
static void draw_gamepad_cursor(void)
{
    float cursor_x, cursor_y;
    if (!gb_input_cursor(&cursor_x, &cursor_y))
        return;

    static const uint16_t outline[] = {
        0b0000000000000001, 0b0000000000000011,
        0b0000000000000111, 0b0000000000001111,
        0b0000000000011111, 0b0000000000111111,
        0b0000000001111111, 0b0000000011111111,
        0b0000000111111111, 0b0000001111111111,
        0b0000011111111111, 0b0000111111111111,
        0b0001111111111111, 0b0000000011111111,
        0b0000000111101111, 0b0000001111000111,
        0b0000001111000011, 0b0000011110000001,
        0b0000011110000000, 0b0000001100000000,
    };
    static const uint16_t fill[] = {
        0, 0, 0b0000000000000010, 0b0000000000000110,
        0b0000000000001110, 0b0000000000011110,
        0b0000000000111110, 0b0000000001111110,
        0b0000000011111110, 0b0000000111111110,
        0b0000001111111110, 0b0000011111111110,
        0b0000000111111110, 0b0000000000111110,
        0b0000000011000110, 0b0000000110000010,
        0b0000000110000000, 0b0000001100000000,
        0b0000001100000000, 0,
    };
    static void (*get_int)(GLenum, GLint *);
    static void (*get_float)(GLenum, GLfloat *);
    static void (*get_bool)(GLenum, GLboolean *);
    static GLboolean (*is_enabled)(GLenum);
    static void (*enable)(GLenum);
    static void (*disable)(GLenum);
    static void (*scissor)(GLint, GLint, GLsizei, GLsizei);
    static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static void (*clear)(GLbitfield);
    if (!get_int) {
        get_int = gl_raw("glGetIntegerv");
        get_float = gl_raw("glGetFloatv");
        get_bool = gl_raw("glGetBooleanv");
        is_enabled = gl_raw("glIsEnabled");
        enable = gl_raw("glEnable");
        disable = gl_raw("glDisable");
        scissor = gl_raw("glScissor");
        clear_color = gl_raw("glClearColor");
        color_mask = gl_raw("glColorMask");
        clear = gl_raw("glClear");
    }
    if (!get_int || !get_float || !get_bool || !is_enabled || !enable ||
        !disable || !scissor || !clear_color || !color_mask || !clear)
        return;

    GLint framebuffer = 0;
    GLint viewport[4] = { 0, 0, 1280, 720 };
    GLint old_scissor[4];
    GLfloat old_clear[4];
    GLboolean old_mask[4];
    GLboolean had_scissor = is_enabled(GL_SCISSOR_TEST);
    get_int(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0)
        return;
    get_int(GL_VIEWPORT, viewport);
    get_int(GL_SCISSOR_BOX, old_scissor);
    get_float(GL_COLOR_CLEAR_VALUE, old_clear);
    get_bool(GL_COLOR_WRITEMASK, old_mask);

    int width = viewport[2];
    int height = viewport[3];
    gb_input_set_screen_size(width, height);
    int x = viewport[0] + (int)(cursor_x * width / 1280.0f);
    int y = (int)(cursor_y * height / 720.0f);
    int scale = width >= 1000 ? 2 : 1;
    enable(GL_SCISSOR_TEST);
    color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    clear_color(0.48f, 0.10f, 0.18f, 1.0f);
    clear_cursor_runs(outline, 20, x + 3, y + 3, scale, width, height,
                      scissor, clear);
    clear_color(0.035f, 0.020f, 0.035f, 1.0f);
    clear_cursor_runs(outline, 20, x, y, scale, width, height,
                      scissor, clear);
    clear_color(0.98f, 0.92f, 0.80f, 1.0f);
    clear_cursor_runs(fill, 20, x, y, scale, width, height,
                      scissor, clear);

    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor[0], old_scissor[1],
            old_scissor[2], old_scissor[3]);
    if (!had_scissor)
        disable(GL_SCISSOR_TEST);
}

static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);

static EGLBoolean my_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    draw_gamepad_cursor();
    if (gb_sdl_video_active())
        return gb_sdl_swap_buffers(display, surface);
    if (!p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");
    return p_eglSwapBuffers(display, surface);
}

static void *my_eglGetProcAddress(const char *name)
{
    if (!name)
        return NULL;
    if (name[0] == 'g' && name[1] == 'l')
        return gb_gl_sym(name);
    if (gb_sdl_video_active())
        return gb_sdl_egl_proc(name);
    static void *(*real)(const char *);
    if (!real)
        real = sys("eglGetProcAddress");
    return real ? real(name) : NULL;
}

/* --- everything else is a straight forward ------------------------------ */

/* A trampoline table beats writing 20 wrappers: the calls we do not rewrite
 * are resolved to the system symbol directly at start-up. */
static const char *const passthrough[] = {
    "eglInitialize", "eglTerminate", "eglGetConfigs", "eglGetConfigAttrib",
    "eglCreatePbufferSurface", "eglDestroySurface", "eglQuerySurface",
    "eglBindAPI", "eglQueryAPI", "eglDestroyContext",
    "eglGetCurrentContext", "eglGetCurrentSurface", "eglGetCurrentDisplay",
    "eglQueryContext", "eglGetError",
    "eglQueryString", "eglSurfaceAttrib", "eglSwapInterval",
    "eglReleaseThread", "eglWaitClient", "eglWaitGL", "eglWaitNative",
    "eglCreateImageKHR", "eglDestroyImageKHR", "eglCreateSyncKHR",
    "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglGetSyncAttribKHR",
    "eglPresentationTimeANDROID", "eglDupNativeFenceFDANDROID",
};

#define MAXTAB (sizeof passthrough / sizeof *passthrough + 8)
static nx_import tab[MAXTAB];
static size_t tab_n;

void gb_egl_init(void)
{
    /* Select ownership by a real SDL GLES capability probe. Raw EGL is the
     * bounded fallback; no CFW, device, display or driver name is consulted. */
    int sdl_owned = gb_sdl_video_init();
    tab_n = 0;
    tab[tab_n++] = (nx_import){ "eglChooseConfig",
        sdl_owned ? gb_sdl_egl_proc("eglChooseConfig")
                  : (void *)my_eglChooseConfig };
    tab[tab_n++] = (nx_import){ "eglCreateContext",
        sdl_owned ? gb_sdl_egl_proc("eglCreateContext")
                  : (void *)my_eglCreateContext };
    tab[tab_n++] = (nx_import){ "eglCreateWindowSurface",
        sdl_owned ? gb_sdl_egl_proc("eglCreateWindowSurface")
                  : (void *)my_eglCreateWindowSurface };
    tab[tab_n++] = (nx_import){ "eglGetDisplay",
        sdl_owned ? gb_sdl_egl_proc("eglGetDisplay")
                  : (void *)my_eglGetDisplay };
    tab[tab_n++] = (nx_import){ "eglMakeCurrent",
        sdl_owned ? gb_sdl_egl_proc("eglMakeCurrent")
                  : (void *)my_eglMakeCurrent };
    tab[tab_n++] = (nx_import){ "eglSwapBuffers",        (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",     (void *)my_eglGetProcAddress };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        void *f = sdl_owned ? gb_sdl_egl_proc(passthrough[i])
                            : dlsym(RTLD_DEFAULT, passthrough[i]);
        if (!f && !sdl_owned)
            f = sys(passthrough[i]);
        if (f)
            tab[tab_n++] = (nx_import){ passthrough[i], f };
    }
    nx_log("egl table: %zu entries (%s ownership)", tab_n,
           sdl_owned ? "SDL" : "raw");
}

const nx_import *gb_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *gb_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return gb_gl_sym(name);
}
