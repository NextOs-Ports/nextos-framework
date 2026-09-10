/*
 * egl.c -- EGL/GLES2 bridge for Blasphemous (Unity 2022.3).
 *
 * The original Android data exposes only GLES3 (BuildSettings
 * m_GraphicsAPIs=[11] and GLES3 shader variants).  On Mali-450 the renderer
 * must be selected before EGL exists: the asset patch relabels the compatible
 * shader payloads and sets m_GraphicsAPIs=[8], while bionic.c supplies Unity's
 * public -force-gles20 switch and reports the real GLES2 system property.
 *
 * The rewrites below are a final negotiation guard, not the backend selector:
 *   - eglChooseConfig removes an accidental ES3 renderable bit,
 *   - eglCreateContext downgrades an accidental ES3 request and drops its
 *     ES3-only context attributes.
 *
 * Everything else forwards to the system EGL, which on the target device is
 * the Mali fbdev driver.  We never set SDL_VIDEODRIVER and never pick a display
 * ourselves; EGL_DEFAULT_DISPLAY is what the driver wants on fbdev.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "nx_elf.h"
#include "sf.h"
#include "egl_sdl.h"
#include "essl1.h"
#include "etc1.h"
#include "etc2_decode.h"
#include "media.h"
#include "nxgl_frame_proof_adapter.h"
#include "present_fill.h"

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
    if (sf_sdl_video_active())
        return sf_sdl_gl_proc(name);
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
static unsigned long shader_translated;
static unsigned long shader_compiles;
static unsigned long program_links;
static unsigned long draw_calls;
static unsigned long texture_images;
static unsigned long texture_sub_images;
static unsigned long texture_binds;
static unsigned long texture_parameters;
static unsigned long buffer_uploads;
static unsigned long framebuffer_calls;

#define SF_TEX_TABLE 8192
#define SF_TEXTURE_UNITS 16
#define SF_PROGRAM_TABLE 8192
#define SF_FBO_TABLE 256
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

/* Unity may load/compile on a shared secondary context.  These bindings are
 * context/thread state, never process globals; a global shadow pairs the
 * wrong texture with the wrong draw as soon as async upload is active. */
static __thread GLenum active_texture_unit;
static __thread GLuint bound_texture_2d;
static __thread GLuint bound_texture_units[SF_TEXTURE_UNITS];
static __thread GLuint bound_array_buffer;
static __thread GLuint bound_element_array_buffer;
static __thread GLuint current_program;
static __thread GLint unpack_alignment;

static GLenum tracked_active_texture_unit(void)
{
    /* GL starts on unit zero.  Keeping the shadow zero-initialized puts this
     * mutable TLS in .tbss instead of ahead of the Bionic guard in .tdata. */
    return active_texture_unit ? active_texture_unit : GL_TEXTURE0;
}

typedef struct {
    GLuint program;
    GLint loc_main;
    GLint loc_alpha;
    GLint loc_enable;
    GLint unit_main;
    GLint unit_alpha;
    GLfloat enable_value;
    uint8_t main_seen;
    uint8_t alpha_seen;
    uint8_t enable_seen;
    uint8_t missing_alpha_logged;
    uint8_t locations_probed;
} sf_program_state;

static sf_program_state program_states[SF_PROGRAM_TABLE];

static sf_program_state *reset_program_state(GLuint program)
{
    if (!program || program >= SF_PROGRAM_TABLE)
        return NULL;
    sf_program_state *state = &program_states[program];
    memset(state, 0, sizeof *state);
    state->program = program;
    state->loc_main = -1;
    state->loc_alpha = -1;
    state->loc_enable = -1;
    state->unit_main = 0;
    state->unit_alpha = 0;
    return state;
}

static sf_program_state *program_state(GLuint program, int create)
{
    if (!program || program >= SF_PROGRAM_TABLE)
        return NULL;
    sf_program_state *state = &program_states[program];
    if (state->program != program) {
        if (!create)
            return NULL;
        state = reset_program_state(program);
    }
    return state;
}

static void probe_program_texture_uniforms(GLuint program, const char *origin)
{
    sf_program_state *state = program_state(program, 1);
    if (!state || state->locations_probed)
        return;
    static GLint (*getuniform)(GLuint, const GLchar *);
    if (!getuniform)
        getuniform = gl_raw("glGetUniformLocation");
    if (!getuniform)
        return;
    state->loc_main = getuniform(program, "_MainTex");
    state->loc_alpha = getuniform(program, "_AlphaTex");
    state->loc_enable = getuniform(program, "_EnableExternalAlpha");
    state->locations_probed = 1;
    if (state->loc_alpha >= 0 || state->loc_enable >= 0)
        fprintf(stderr,
                "[sf/etc1] program=%u external-alpha main=%d alpha=%d "
                "enable=%d origin=%s\n",
                program, state->loc_main, state->loc_alpha,
                state->loc_enable, origin ? origin : "unknown");
}

enum {
    SF_TEX_NORMAL = 0,
    SF_TEX_ETC1_OPAQUE = 1,
    SF_TEX_ETC1_DUAL = 2,
    SF_TEX_RGBA_DEMOTED = 3,
    SF_TEX_PALETTE_LA = 4,
};

static GLuint texture_twins[SF_TEX_TABLE];
static uint8_t texture_kinds[SF_TEX_TABLE];
static GLsizei texture_widths[SF_TEX_TABLE];
static GLsizei texture_heights[SF_TEX_TABLE];
static uint64_t texture_hashes[SF_TEX_TABLE];
static GLuint framebuffer_color_textures[SF_FBO_TABLE];
static __thread unsigned int framebuffer_frame_draws[SF_FBO_TABLE];
static unsigned long etc1_opaque_uploads;
static unsigned long etc1_dual_uploads;
static unsigned long etc1_cache_hits;
static unsigned long etc1_encodes;
static unsigned long etc1_demotions;

static void etc1_reset_texture(GLuint texture, int delete_twin);
static int etc1_demote_texture(GLuint texture, const char *reason);

typedef struct {
    GLuint program;
    GLint location;
    char name[64];
} sf_uniform_name;

static sf_uniform_name uniform_names[128];
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
    sf_uniform_name *entry = &uniform_names[uniform_name_count++];
    entry->program = program;
    entry->location = location;
    snprintf(entry->name, sizeof entry->name, "%s", name);
}

static int trace_texture_call(unsigned long call, GLsizei width,
                              GLsizei height)
{
    return sf_trace_gl && (call <= 160 || width >= 512 || height >= 512);
}

unsigned long sf_swap_count;


/* Sondas de ambiente do caminho quente lidas UMA vez: getenv e' varredura
 * linear do environ e estas ficam DENTRO do quadro (glBindFramebuffer sozinho
 * roda dezenas de vezes por frame). */
static int sf_env_flag(const char *name)
{
    const char *v = getenv(name);
    return v != NULL;
}

static int shader_log_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = sf_env_flag("SF_SHADERLOG");
    return enabled;
}

/* Print sob demanda: o input pede um shot (L3 com SF_SHOT ligado) e o proximo
   frame grava.  Existe porque ler /dev/fb0 de fora devolve preto enquanto o
   Mali renderiza — a unica testemunha honesta e o glReadPixels de dentro. */
int sf_shot_request;
static char sf_shot_path[256];

static void capture_current_fbo_if_requested(void)
{
    static int finished;
    static const char *path;
    static int path_known;
    static unsigned shot_seq;
    if (!path_known) {
        path = getenv("SF_FBO_CAPTURE");
        path_known = 1;
    }
    int on_demand = 0;
    if (sf_shot_request) {
        sf_shot_request = 0;
        /* Sem caminho cravado no binario de release: o padrao e' a pasta do
           proprio jogo, seja qual for onde ele foi instalado. */
        const char *dir = getenv("SF_SHOT_DIR");
        if (!dir || !*dir) dir = sf_gamedir;
        snprintf(sf_shot_path, sizeof sf_shot_path, "%.200s/shot_%03u.ppm",
                 dir, ++shot_seq);
        on_demand = 1;
    }
    if (on_demand) {
        path = sf_shot_path;
        finished = 0;
    }
    if (finished || !path || !*path || draw_calls < 2)
        return;
    if (!on_demand) {
        const char *want = getenv("SF_FBO_CAPTURE_FRAME");
        if (want && *want && sf_swap_count < strtoul(want, NULL, 10))
            return;
    }
    finished = 1;
    if (on_demand)
        fprintf(stderr, "[bc/shot] gravando %s\n", path);

    void (*get_int)(GLenum, GLint *) = gl_raw("glGetIntegerv");
    void (*pixel_store)(GLenum, GLint) = gl_raw("glPixelStorei");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) = gl_raw("glReadPixels");
    if (!get_int || !pixel_store || !read_pixels)
        return;
    GLint viewport[4] = { 0, 0, 0, 0 };
    get_int(GL_VIEWPORT, viewport);
    int width = viewport[2], height = viewport[3];
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return;
    size_t pixels = (size_t)width * (size_t)height;
    unsigned char *rgba = malloc(pixels * 4);
    unsigned char *rgb = malloc(pixels * 3);
    if (!rgba || !rgb) {
        free(rgb);
        free(rgba);
        return;
    }
    GLint old_pack = 4;
    get_int(GL_PACK_ALIGNMENT, &old_pack);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    pixel_store(GL_PACK_ALIGNMENT, old_pack);
    for (int y = 0; y < height; y++) {
        const unsigned char *source =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
        unsigned char *dest = rgb + (size_t)y * (size_t)width * 3;
        for (int x = 0; x < width; x++) {
            dest[x * 3 + 0] = source[x * 4 + 0];
            dest[x * 3 + 1] = source[x * 4 + 1];
            dest[x * 3 + 2] = source[x * 4 + 2];
        }
    }
    FILE *output = fopen(path, "wb");
    if (output) {
        fprintf(output, "P6\n%d %d\n255\n", width, height);
        size_t written = fwrite(rgb, 3, pixels, output);
        if (fclose(output) == 0 && written == pixels)
            nx_log("FBO capture: %dx%d -> %s", width, height, path);
    }
    free(rgb);
    free(rgba);
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

static void dump_buffer_upload(GLenum target, GLuint buffer, GLintptr offset,
                               const void *data, GLsizeiptr size,
                               unsigned long upload)
{
    const char *prefix = getenv("SF_GL_DUMP_PREFIX");
    if (!prefix || !*prefix || !data || size <= 0)
        return;
    char path[1024];
    int length = snprintf(path, sizeof path,
                          "%s-buffer-%u-target-%x-offset-%ld-upload-%lu.bin",
                          prefix, buffer, target, (long)offset, upload);
    if (length < 0 || (size_t)length >= sizeof path)
        return;
    FILE *output = fopen(path, "wb");
    if (!output) {
        nx_log("GL dump: cannot open %s", path);
        return;
    }
    size_t written = fwrite(data, 1, (size_t)size, output);
    if (fclose(output) != 0 || written != (size_t)size)
        nx_log("GL dump: short write for %s", path);
    else
        nx_log("GL dump: %ld bytes -> %s", (long)size, path);
}

/*
 * Amlogic's fbdev OSD compositor uses the alpha channel of fb0.  Unity can
 * leave valid RGB with alpha zero, which makes a correctly rendered frame scan
 * out as black.  Mirror the small piece of GL state needed by the proven
 * Horizon Chase A-only clear so the regular swap path does not introduce
 * glGet* stalls on every frame.
 */
typedef struct {
    int initialized;
    GLuint framebuffer;
    GLboolean color_mask[4];
    GLfloat clear_color[4];
    GLboolean scissor;
} sf_gl_state;

static __thread sf_gl_state gl_state;

static void my_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    static void (*real)(GLenum, GLuint);
    static void (*finish)(void);
    if (!real) {
        real = gl_raw("glBindFramebuffer");
        if (!real)
            real = gl_raw("glBindFramebufferOES");
    }
    GLuint previous = gl_state.framebuffer;
    if (previous != 0 && framebuffer == 0 &&
        (target == GL_FRAMEBUFFER || target == 0x8CA9))
        capture_current_fbo_if_requested();
    static int fbo_finish = -1;
    if (fbo_finish < 0)
        fbo_finish = sf_env_flag("SF_FBO_FINISH");
    if (fbo_finish && previous != 0 && framebuffer == 0 &&
        (target == GL_FRAMEBUFFER || target == 0x8CA9)) {
        if (!finish)
            finish = gl_raw("glFinish");
        if (finish) {
            finish();
            if (sf_trace_gl)
                fprintf(stderr,
                        "[bc/gl] forced finish before fbo %u -> 0\n",
                        previous);
        }
    }
    if (real)
        real(target, framebuffer);
    if (target == GL_FRAMEBUFFER || target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */)
        gl_state.framebuffer = framebuffer;
    framebuffer_calls++;
    if (sf_trace_gl && framebuffer_calls <= 160)
        fprintf(stderr, "[bc/gl] fbo-bind #%lu target=%#x framebuffer=%u\n",
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
    if (count > 0 && framebuffers) {
        for (GLsizei i = 0; i < count; i++) {
            if (framebuffers[i] < SF_FBO_TABLE) {
                framebuffer_color_textures[framebuffers[i]] = 0;
                framebuffer_frame_draws[framebuffers[i]] = 0;
            }
            if (framebuffers[i] == current) {
                gl_state.framebuffer = 0;
            }
        }
    }
}

static void my_glColorMask(GLboolean red, GLboolean green, GLboolean blue,
                           GLboolean alpha)
{
    static void (*real)(GLboolean, GLboolean, GLboolean, GLboolean);
    gl_state.color_mask[0] = !!red;
    gl_state.color_mask[1] = !!green;
    gl_state.color_mask[2] = !!blue;
    gl_state.color_mask[3] = !!alpha;
    if (!real)
        real = gl_raw("glColorMask");
    if (real)
        real(red, green, blue, alpha);
}

static void my_glClearColor(GLfloat red, GLfloat green, GLfloat blue,
                            GLfloat alpha)
{
    static void (*real)(GLfloat, GLfloat, GLfloat, GLfloat);
    gl_state.clear_color[0] = red;
    gl_state.clear_color[1] = green;
    gl_state.clear_color[2] = blue;
    gl_state.clear_color[3] = alpha;
    if (!real)
        real = gl_raw("glClearColor");
    if (real)
        real(red, green, blue, alpha);
}

static void my_glEnable(GLenum capability)
{
    static void (*real)(GLenum);
    if (capability == GL_SCISSOR_TEST)
        gl_state.scissor = GL_TRUE;
    if (!real)
        real = gl_raw("glEnable");
    if (real)
        real(capability);
}

static void my_glDisable(GLenum capability)
{
    static void (*real)(GLenum);
    if (capability == GL_SCISSOR_TEST)
        gl_state.scissor = GL_FALSE;
    if (!real)
        real = gl_raw("glDisable");
    if (real)
        real(capability);
}

static void force_opaque_backbuffer(void)
{
    static int no_opaque = -1;
    if (no_opaque < 0)
        no_opaque = sf_env_flag("SF_NO_OPAQUE_BACKBUFFER");
    if (no_opaque)
        return;

    static void (*get_int)(GLenum, GLint *);
    static void (*get_float)(GLenum, GLfloat *);
    static void (*get_bool)(GLenum, GLboolean *);
    static GLboolean (*is_enabled)(GLenum);
    static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    static void (*clear)(GLbitfield);
    static void (*enable)(GLenum);
    static void (*disable)(GLenum);

    if (!gl_state.initialized) {
        get_int = gl_raw("glGetIntegerv");
        get_float = gl_raw("glGetFloatv");
        get_bool = gl_raw("glGetBooleanv");
        is_enabled = gl_raw("glIsEnabled");
        if (!get_int || !get_float || !get_bool || !is_enabled)
            return;
        get_int(GL_FRAMEBUFFER_BINDING, (GLint *)&gl_state.framebuffer);
        get_float(GL_COLOR_CLEAR_VALUE, gl_state.clear_color);
        get_bool(GL_COLOR_WRITEMASK, gl_state.color_mask);
        gl_state.scissor = !!is_enabled(GL_SCISSOR_TEST);
        gl_state.initialized = 1;
        static int glstate_trace = -1;
        if (glstate_trace < 0)
            glstate_trace = sf_env_flag("SF_GLSTATE_TRACE");
        if (glstate_trace) {
            fprintf(stderr,
                    "[bc/gl] state fbo=%u mask=%u%u%u%u "
                    "clear=%.2f,%.2f,%.2f,%.2f scissor=%u\n",
                    gl_state.framebuffer, gl_state.color_mask[0],
                    gl_state.color_mask[1], gl_state.color_mask[2],
                    gl_state.color_mask[3], gl_state.clear_color[0],
                    gl_state.clear_color[1], gl_state.clear_color[2],
                    gl_state.clear_color[3], gl_state.scissor);
        }
    }

    if (gl_state.framebuffer != 0)
        return;
    if (!color_mask) {
        color_mask = gl_raw("glColorMask");
        clear_color = gl_raw("glClearColor");
        clear = gl_raw("glClear");
        enable = gl_raw("glEnable");
        disable = gl_raw("glDisable");
    }
    if (!color_mask || !clear_color || !clear || !enable || !disable)
        return;

    GLboolean old_mask[4];
    GLfloat old_clear[4];
    memcpy(old_mask, gl_state.color_mask, sizeof old_mask);
    memcpy(old_clear, gl_state.clear_color, sizeof old_clear);
    GLboolean had_scissor = gl_state.scissor;

    if (had_scissor)
        disable(GL_SCISSOR_TEST);
    color_mask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    if (had_scissor)
        enable(GL_SCISSOR_TEST);
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

typedef struct {
    char character;
    uint8_t row[7];
} pixel_glyph;

static const pixel_glyph keyboard_font[] = {
    { 'A', {14,17,17,31,17,17,17} },
    { 'B', {30,17,17,30,17,17,30} },
    { 'C', {14,17,16,16,16,17,14} },
    { 'D', {30,17,17,17,17,17,30} },
    { 'E', {31,16,16,30,16,16,31} },
    { 'F', {31,16,16,30,16,16,16} },
    { 'G', {14,17,16,23,17,17,14} },
    { 'H', {17,17,17,31,17,17,17} },
    { 'I', {31,4,4,4,4,4,31} },
    { 'J', {7,2,2,2,18,18,12} },
    { 'K', {17,18,20,24,20,18,17} },
    { 'L', {16,16,16,16,16,16,31} },
    { 'M', {17,27,21,21,17,17,17} },
    { 'N', {17,25,21,19,17,17,17} },
    { 'O', {14,17,17,17,17,17,14} },
    { 'P', {30,17,17,30,16,16,16} },
    { 'Q', {14,17,17,17,21,18,13} },
    { 'R', {30,17,17,30,20,18,17} },
    { 'S', {15,16,16,14,1,1,30} },
    { 'T', {31,4,4,4,4,4,4} },
    { 'U', {17,17,17,17,17,17,14} },
    { 'V', {17,17,17,17,17,10,4} },
    { 'W', {17,17,17,21,21,21,10} },
    { 'X', {17,17,10,4,10,17,17} },
    { 'Y', {17,17,10,4,4,4,4} },
    { 'Z', {31,1,2,4,8,16,31} },
    { 'a', {0,0,14,1,15,17,15} },
    { 'b', {16,16,30,17,17,17,30} },
    { 'c', {0,0,14,17,16,17,14} },
    { 'd', {1,1,15,17,17,17,15} },
    { 'e', {0,0,14,17,31,16,14} },
    { 'f', {6,9,8,28,8,8,8} },
    { 'g', {0,0,15,17,15,1,14} },
    { 'h', {16,16,30,17,17,17,17} },
    { 'i', {4,0,12,4,4,4,14} },
    { 'j', {2,0,6,2,2,18,12} },
    { 'k', {16,16,18,20,24,20,18} },
    { 'l', {12,4,4,4,4,4,14} },
    { 'm', {0,0,26,21,21,17,17} },
    { 'n', {0,0,30,17,17,17,17} },
    { 'o', {0,0,14,17,17,17,14} },
    { 'p', {0,0,30,17,30,16,16} },
    { 'q', {0,0,15,17,15,1,1} },
    { 'r', {0,0,22,25,16,16,16} },
    { 's', {0,0,15,16,14,1,30} },
    { 't', {8,8,28,8,8,9,6} },
    { 'u', {0,0,17,17,17,19,13} },
    { 'v', {0,0,17,17,17,10,4} },
    { 'w', {0,0,17,17,21,21,10} },
    { 'x', {0,0,17,10,4,10,17} },
    { 'y', {0,0,17,17,15,1,14} },
    { 'z', {0,0,31,2,4,8,31} },
    { '0', {14,17,19,21,25,17,14} },
    { '1', {4,12,4,4,4,4,14} },
    { '2', {14,17,1,2,4,8,31} },
    { '3', {30,1,1,14,1,1,30} },
    { '4', {2,6,10,18,31,2,2} },
    { '5', {31,16,16,30,1,1,30} },
    { '6', {14,16,16,30,17,17,14} },
    { '7', {31,1,2,4,8,8,8} },
    { '8', {14,17,17,14,17,17,14} },
    { '9', {14,17,17,15,1,1,14} },
    { '-', {0,0,0,31,0,0,0} },
    { '/', {1,2,2,4,8,8,16} },
    { '_', {0,0,0,0,0,0,31} },
    { '\'',{4,4,2,0,0,0,0} },
};

static const uint8_t *keyboard_glyph(char character)
{
    for (size_t i = 0; i < sizeof keyboard_font / sizeof *keyboard_font; i++)
        if (keyboard_font[i].character == character)
            return keyboard_font[i].row;
    return NULL;
}

static void keyboard_rect(int x, int y, int w, int h,
                          float r, float g, float b, float a,
                          const GLint *viewport,
                          void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                          void (*clear_color)(GLfloat, GLfloat, GLfloat,
                                              GLfloat),
                          void (*clear)(GLbitfield))
{
    int left = viewport[0] + x * viewport[2] / 1280;
    int right = viewport[0] + (x + w) * viewport[2] / 1280;
    int top = y * viewport[3] / 720;
    int bottom = (y + h) * viewport[3] / 720;
    if (left < viewport[0])
        left = viewport[0];
    if (right > viewport[0] + viewport[2])
        right = viewport[0] + viewport[2];
    if (top < 0)
        top = 0;
    if (bottom > viewport[3])
        bottom = viewport[3];
    if (right <= left || bottom <= top)
        return;
    scissor(left, viewport[1] + viewport[3] - bottom,
            right - left, bottom - top);
    clear_color(r, g, b, a);
    clear(GL_COLOR_BUFFER_BIT);
}

static int keyboard_text_width(const char *text, int scale)
{
    return text ? (int)strlen(text) * 6 * scale : 0;
}

static void keyboard_text(int x, int y, int scale, const char *text,
                          float r, float g, float b, float a,
                          const GLint *viewport,
                          void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                          void (*clear_color)(GLfloat, GLfloat, GLfloat,
                                              GLfloat),
                          void (*clear)(GLbitfield))
{
    if (!text)
        return;
    for (; *text; text++, x += 6 * scale) {
        if (*text == ' ')
            continue;
        const uint8_t *rows = keyboard_glyph(*text);
        if (!rows)
            continue;
        for (int row = 0; row < 7; row++) {
            uint8_t bits = rows[row];
            for (int col = 0; col < 5;) {
                while (col < 5 && !(bits & (1u << (4 - col))))
                    col++;
                int start = col;
                while (col < 5 && (bits & (1u << (4 - col))))
                    col++;
                if (start < col)
                    keyboard_rect(x + start * scale, y + row * scale,
                                  (col - start) * scale, scale,
                                  r, g, b, a, viewport,
                                  scissor, clear_color, clear);
            }
        }
    }
}

/* Controller keyboard modelled after the proven FF4 naming keyboard.  HGO is
 * GLES2, so this version uses only scissored clears: no shader, texture or
 * vertex state from Unity is replaced. */
static void draw_gamepad_keyboard(void)
{
    char typed[64];
    int uppercase = 0;
    int selected = 0;
    const sf_keyboard_key *keys = NULL;
    size_t key_count = 0;
    if (!sf_input_keyboard_snapshot(typed, sizeof typed, &uppercase,
                                     &selected, &keys, &key_count))
        return;

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
    enable(GL_SCISSOR_TEST);
    color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

#define KRECT(x,y,w,h,r,g,b,a) \
    keyboard_rect((x),(y),(w),(h),(r),(g),(b),(a), viewport, \
                  scissor, clear_color, clear)
#define KTEXT(x,y,s,t,r,g,b,a) \
    keyboard_text((x),(y),(s),(t),(r),(g),(b),(a), viewport, \
                  scissor, clear_color, clear)

    /* Near-black frame, burgundy cabinet and cream highlights match HGO. */
    KRECT(132, 268, 1016, 452, 0.02f, 0.01f, 0.02f, 1.0f);
    KRECT(140, 276, 1000, 444, 0.50f, 0.08f, 0.14f, 1.0f);
    KRECT(148, 284, 984, 436, 0.035f, 0.025f, 0.035f, 1.0f);
    KTEXT(171, 290, 2, "ENTER NAME", 0.98f, 0.92f, 0.80f, 1.0f);

    KRECT(167, 306, 947, 62, 0.50f, 0.08f, 0.14f, 1.0f);
    KRECT(173, 312, 935, 50, 0.01f, 0.01f, 0.015f, 1.0f);
    char shown[66];
    snprintf(shown, sizeof shown, "%s_", typed);
    KTEXT(190, 323, 4, shown, 0.98f, 0.92f, 0.80f, 1.0f);

    for (size_t i = 0; i < key_count; i++) {
        const sf_keyboard_key *key = &keys[i];
        char dynamic_label[8];
        const char *label = key->label;
        if (key->action == SF_KEY_CHARACTER) {
            dynamic_label[0] = uppercase ? key->upper : key->lower;
            dynamic_label[1] = '\0';
            label = dynamic_label;
        } else if (key->action == SF_KEY_SHIFT) {
            snprintf(dynamic_label, sizeof dynamic_label, "%s",
                     uppercase ? "UPPER" : "LOWER");
            label = dynamic_label;
        }
        int highlighted = (int)i == selected ||
            (key->action == SF_KEY_SHIFT && uppercase);
        KRECT(key->x - 3, key->y - 3, key->w + 6, key->h + 6,
              highlighted ? 0.98f : 0.02f,
              highlighted ? 0.82f : 0.01f,
              highlighted ? 0.48f : 0.02f, 1.0f);
        KRECT(key->x, key->y, key->w, key->h,
              highlighted ? 0.65f : 0.30f,
              highlighted ? 0.10f : 0.055f,
              highlighted ? 0.16f : 0.095f, 1.0f);
        int scale = strlen(label) > 3 ? 2 : 3;
        int text_x =
            key->x + (key->w - keyboard_text_width(label, scale)) / 2;
        int text_y = key->y + (key->h - 7 * scale) / 2;
        KTEXT(text_x, text_y, scale, label,
              0.98f, 0.92f, 0.80f, 1.0f);
    }
    KTEXT(171, 650, 2, "R3/A SELECT  B DELETE  X SHIFT  START DONE",
          0.82f, 0.78f, 0.70f, 1.0f);

#undef KTEXT
#undef KRECT
    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor[0], old_scissor[1],
            old_scissor[2], old_scissor[3]);
    if (!had_scissor)
        disable(GL_SCISSOR_TEST);
}

/* A small Hitman-style pixel cursor: cream face, near-black outline and
 * wine-red drop shadow.  It is drawn with scissored color clears immediately
 * before swap, avoiding shaders/textures and preserving Unity's GL program. */
static void draw_gamepad_cursor(void)
{
    float cursor_x, cursor_y;
    if (!sf_input_cursor(&cursor_x, &cursor_y))
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
    sf_input_set_screen_size(width, height);
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

/* Unity's Blasphemous shaders were built without the Android ETC1 external-
 * alpha variant.  Keeping an RGBA atlas as ETC1 therefore needs a small,
 * mechanical fragment-stage adaptation: RGB remains in _MainTex and alpha is
 * read from the red channel of the ETC1 twin only while the draw bridge enables
 * it.  The explicit branch is intentional on Utgard; a mix() here has produced
 * unstable external-alpha results on this GPU family. */
static char *patch_fragment_external_alpha(const char *source,
                                           size_t source_len,
                                           size_t *patched_len,
                                           size_t *sample_count)
{
    static const char needle[] = "texture2D(_MainTex,";
    static const char replacement[] = "sf_sample_main(";
    static const char helper[] =
        "\nuniform mediump sampler2D _AlphaTex;\n"
        "uniform mediump float _EnableExternalAlpha;\n"
        "mediump vec4 sf_sample_main(highp vec2 uv)\n"
        "{\n"
        "    mediump vec4 color = texture2D(_MainTex, uv);\n"
        "    if (_EnableExternalAlpha > 0.5)\n"
        "        color.a = texture2D(_AlphaTex, uv).r;\n"
        "    return color;\n"
        "}\n\n";

    if (patched_len)
        *patched_len = 0;
    if (sample_count)
        *sample_count = 0;
    if (!source || !source_len || !strstr(source, "_MainTex") ||
        strstr(source, "_AlphaTex") || strstr(source, "sf_sample_main"))
        return NULL;

    size_t samples = 0;
    const char *scan = source;
    const char *end = source + source_len;
    while (scan < end) {
        const char *hit = strstr(scan, needle);
        if (!hit || hit >= end)
            break;
        samples++;
        scan = hit + sizeof needle - 1;
    }
    if (!samples)
        return NULL;

    const size_t needle_len = sizeof needle - 1;
    const size_t replacement_len = sizeof replacement - 1;
    size_t replaced_len;
    if (replacement_len >= needle_len) {
        const size_t growth = replacement_len - needle_len;
        if (growth && samples > (SIZE_MAX - source_len) / growth)
            return NULL;
        replaced_len = source_len + samples * growth;
    } else {
        const size_t shrink = needle_len - replacement_len;
        if (samples > source_len / shrink)
            return NULL;
        replaced_len = source_len - samples * shrink;
    }
    char *replaced = malloc(replaced_len + 1);
    if (!replaced)
        return NULL;

    const char *from = source;
    char *to = replaced;
    for (;;) {
        const char *hit = strstr(from, needle);
        if (!hit || hit >= end)
            break;
        size_t prefix = (size_t)(hit - from);
        memcpy(to, from, prefix);
        to += prefix;
        memcpy(to, replacement, replacement_len);
        to += replacement_len;
        from = hit + needle_len;
    }
    size_t tail = (size_t)(end - from);
    memcpy(to, from, tail);
    to += tail;
    *to = '\0';

    char *main_decl = strstr(replaced, "void main");
    if (!main_decl) {
        free(replaced);
        return NULL;
    }
    const size_t helper_len = sizeof helper - 1;
    if (helper_len > SIZE_MAX - replaced_len) {
        free(replaced);
        return NULL;
    }
    size_t result_len = replaced_len + helper_len;
    char *result = malloc(result_len + 1);
    if (!result) {
        free(replaced);
        return NULL;
    }
    size_t before_main = (size_t)(main_decl - replaced);
    memcpy(result, replaced, before_main);
    memcpy(result + before_main, helper, helper_len);
    memcpy(result + before_main + helper_len, main_decl,
           replaced_len - before_main);
    result[result_len] = '\0';
    free(replaced);

    if (patched_len)
        *patched_len = result_len;
    if (sample_count)
        *sample_count = samples;
    return result;
}

static char *patch_external_video_sampler(const char *source, size_t length,
                                          size_t *patched_length)
{
    static const char external_sampler[] = "samplerExternalOES";
    static const char regular_sampler[] = "sampler2D";
    if (!source || !memmem(source, length, external_sampler,
                           sizeof external_sampler - 1))
        return NULL;
    char *patched = malloc(length + 1);
    if (!patched)
        return NULL;
    size_t from = 0;
    size_t to = 0;
    while (from < length) {
        size_t line_end = from;
        while (line_end < length && source[line_end] != '\n')
            line_end++;
        size_t line_length = line_end - from;
        int extension_line =
            memmem(source + from, line_length, "#extension", 10) &&
            memmem(source + from, line_length,
                   "GL_OES_EGL_image_external", 25);
        if (!extension_line) {
            size_t at = from;
            while (at < line_end) {
                if (line_end - at >= sizeof external_sampler - 1 &&
                    memcmp(source + at, external_sampler,
                           sizeof external_sampler - 1) == 0) {
                    memcpy(patched + to, regular_sampler,
                           sizeof regular_sampler - 1);
                    to += sizeof regular_sampler - 1;
                    at += sizeof external_sampler - 1;
                } else {
                    patched[to++] = source[at++];
                }
            }
        }
        if (line_end < length)
            patched[to++] = '\n';
        from = line_end < length ? line_end + 1 : line_end;
    }
    patched[to] = '\0';
    if (patched_length)
        *patched_length = to;
    return patched;
}

static GLuint my_glCreateShader(GLenum type)
{
    static GLuint (*real)(GLenum);
    if (!real)
        real = gl_raw("glCreateShader");
    GLuint shader = real(type);
    remember_shader(shader, type);
    if (sf_trace_gl || shader_log_enabled())
        fprintf(stderr, "[bc/gl] create %s shader=%u\n",
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
    if (sf_trace_gl || shader_log_enabled()) {
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
                "[bc/gl] source #%lu shader=%u stage=%s bytes=%zu: %.160s\n",
                shader_sources, shader, shader_stage(shader), total, preview);
        if (getenv("SF_GLSOURCE")) {
            fprintf(stderr, "[bc/gl/source-begin] shader=%u stage=%s\n",
                    shader, shader_stage(shader));
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                fwrite(strings[i], 1, n, stderr);
            }
            fprintf(stderr, "\n[bc/gl/source-end] shader=%u\n", shader);
        }
    }

    /* Os shaders do jogo so' existem em ESSL 300 (variante GLES3); o Utgard so'
     * compila ESSL 100.  Traduz antes de entregar ao driver -- sem isso a Unity
     * cai no error shader e a tela fica rosa. */
    if (!sf_essl1_disabled()) {
        size_t total = 0;
        for (GLsizei i = 0; i < count; i++)
            total += lengths && lengths[i] >= 0
                       ? (size_t)lengths[i] : strlen(strings[i]);
        char *joined = malloc(total + 1);
        if (joined) {
            size_t at = 0;
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                memcpy(joined + at, strings[i], n);
                at += n;
            }
            joined[at] = '\0';

            size_t video_len = 0;
            char *video_source = patch_external_video_sampler(
                joined, at, &video_len);
            const char *translation_source = video_source
                                           ? video_source : joined;
            size_t translation_length = video_source ? video_len : at;
            unsigned essl1_reasons = sf_essl1_classify(
                translation_source, translation_length);

            GLenum type = shader < sizeof shader_types / sizeof *shader_types
                            ? shader_types[shader] : 0;
            size_t xlen = 0;
            char *x = sf_essl1_translate(translation_source,
                                         translation_length,
                                         type == GL_FRAGMENT_SHADER, &xlen);
            unsigned essl1_leftovers = x ? sf_essl1_classify(x, xlen)
                                         : essl1_reasons;
            if (essl1_reasons && (sf_trace_gl || shader_log_enabled()))
                fprintf(stderr,
                        "[bc/gl] essl1-decision shader=%u stage=%s "
                        "reasons=0x%x translated=%d leftovers=0x%x\n",
                        shader, shader_stage(shader), essl1_reasons,
                        x != NULL, essl1_leftovers);
            if ((essl1_reasons & SF_ESSL1_REASON_UNITY_ALIASES) && x) {
                static unsigned alias_receipt_stages;
                unsigned stage_bit = type == GL_VERTEX_SHADER ? 1u
                                     : type == GL_FRAGMENT_SHADER ? 2u : 4u;
                if (!(alias_receipt_stages & stage_bit)) {
                    alias_receipt_stages |= stage_bit;
                    fprintf(stderr,
                            "[sf/shader] Unity ESSL aliases -> GLES2 "
                            "shader=%u stage=%s leftovers=0x%x\n",
                            shader, shader_stage(shader), essl1_leftovers);
                }
            }
            if (x) {
                if (type == GL_FRAGMENT_SHADER) {
                    size_t external_len = 0;
                    size_t external_samples = 0;
                    char *external = patch_fragment_external_alpha(
                        x, xlen, &external_len, &external_samples);
                    if (external) {
                        free(x);
                        x = external;
                        xlen = external_len;
                        fprintf(stderr,
                                "[sf/etc1] fragment shader=%u patched "
                                "external alpha (%zu samples)\n",
                                shader, external_samples);
                    }
                }
                const char *one = x;
                GLint onelen = (GLint)xlen;
                shader_translated++;
                if (sf_trace_gl || shader_log_enabled())
                    fprintf(stderr,
                            "[bc/gl] essl1 shader=%u stage=%s %zu -> %zu bytes\n",
                            shader, shader_stage(shader), at, xlen);
                if (getenv("SF_GLSOURCE_OUT")) {
                    fprintf(stderr, "[bc/gl/essl1-begin] shader=%u stage=%s\n",
                            shader, shader_stage(shader));
                    fwrite(x, 1, xlen, stderr);
                    fprintf(stderr, "\n[bc/gl/essl1-end] shader=%u\n", shader);
                }
                real(shader, 1, &one, &onelen);
                free(x);
                free(video_source);
                free(joined);
                return;
            }
            if (video_source) {
                const char *one = video_source;
                GLint onelen = (GLint)video_len;
                if (sf_trace_gl || shader_log_enabled())
                    fprintf(stderr,
                            "[sf/media] shader=%u external sampler -> "
                            "sampler2D (%zu -> %zu bytes)\n",
                            shader, at, video_len);
                real(shader, 1, &one, &onelen);
                free(video_source);
                free(joined);
                return;
            }
            free(joined);
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
    if (sf_trace_gl || shader_log_enabled() || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(shader, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr,
                "[bc/gl] compile #%lu shader=%u stage=%s ok=%d log=%s\n",
                shader_compiles, shader, shader_stage(shader), ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
}

static GLuint my_glCreateProgram(void)
{
    static GLuint (*real)(void);
    if (!real)
        real = gl_raw("glCreateProgram");
    GLuint program = real ? real() : 0;
    (void)reset_program_state(program);
    return program;
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
    if (sf_trace_gl || shader_log_enabled() || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(program, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr, "[bc/gl] link #%lu program=%u ok=%d log=%s\n",
                program_links, program, ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
    if (ok == GL_TRUE) {
        (void)reset_program_state(program);
        probe_program_texture_uniforms(program, "link");
    }
}

static void program_binary_result(GLuint program, GLenum format,
                                  GLsizei length, const char *entry)
{
    static void (*getiv)(GLuint, GLenum, GLint *);
    if (!getiv)
        getiv = gl_raw("glGetProgramiv");
    GLint linked = GL_FALSE;
    if (getiv)
        getiv(program, GL_LINK_STATUS, &linked);
    fprintf(stderr,
            "[sf/shader] %s program=%u format=%#x bytes=%d linked=%d\n",
            entry, program, format, length, linked == GL_TRUE);
    if (linked == GL_TRUE) {
        (void)reset_program_state(program);
        probe_program_texture_uniforms(program, entry);
    }
}

static void my_glProgramBinary(GLuint program, GLenum format,
                               const void *binary, GLsizei length)
{
    static void (*real)(GLuint, GLenum, const void *, GLsizei);
    if (!real)
        real = gl_raw("glProgramBinary");
    if (real)
        real(program, format, binary, length);
    program_binary_result(program, format, length, "glProgramBinary");
}

static void my_glProgramBinaryOES(GLuint program, GLenum format,
                                  const void *binary, GLsizei length)
{
    static void (*real)(GLuint, GLenum, const void *, GLsizei);
    if (!real)
        real = gl_raw("glProgramBinaryOES");
    if (real)
        real(program, format, binary, length);
    program_binary_result(program, format, length, "glProgramBinaryOES");
}

static void my_glUseProgram(GLuint program)
{
    static void (*real)(GLuint);
    current_program = program;
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] use-program %u\n", program);
    if (!real)
        real = gl_raw("glUseProgram");
    if (real)
        real(program);
    probe_program_texture_uniforms(program, "use/cache");
}

static GLint my_glGetAttribLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetAttribLocation");
    GLint location = real ? real(program, name) : -1;
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] attrib program=%u location=%d name=%s\n",
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
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] buffer-bind target=%#x buffer=%u\n",
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
    static void (*sub)(GLenum, GLintptr, GLsizeiptr, const void *);
    buffer_uploads++;
    if (sf_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[bc/gl] buffer-data #%lu target=%#x buffer=%u bytes=%ld "
                "usage=%#x",
                buffer_uploads, target, buffer, (long)size, usage);
        trace_buffer_payload(data, size);
        fputc('\n', stderr);
    }
    GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                    : target == GL_ELEMENT_ARRAY_BUFFER
                    ? bound_element_array_buffer : 0;
    dump_buffer_upload(target, buffer, 0, data, size, buffer_uploads);
    if (!real)
        real = gl_raw("glBufferData");
    if (!real)
        return;
    static int split_upload = -1;
    if (split_upload < 0)
        split_upload = sf_env_flag("SF_BUFFER_SPLIT_UPLOAD");
    if (split_upload && data && size > 0 &&
        usage == GL_DYNAMIC_DRAW) {
        if (!sub)
            sub = gl_raw("glBufferSubData");
        if (sub) {
            real(target, size, NULL, usage);
            sub(target, 0, size, data);
            if (sf_trace_gl)
                fprintf(stderr,
                        "[bc/gl] split buffer upload target=%#x "
                        "buffer=%u bytes=%ld\n",
                        target, buffer, (long)size);
            return;
        }
    }
    real(target, size, data, usage);
}

static void my_glBufferSubData(GLenum target, GLintptr offset,
                               GLsizeiptr size, const void *data)
{
    static void (*real)(GLenum, GLintptr, GLsizeiptr, const void *);
    buffer_uploads++;
    if (sf_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[bc/gl] buffer-sub #%lu target=%#x buffer=%u offset=%ld "
                "bytes=%ld",
                buffer_uploads, target, buffer, (long)offset, (long)size);
        trace_buffer_payload(data, size);
        fputc('\n', stderr);
    }
    GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                    : target == GL_ELEMENT_ARRAY_BUFFER
                    ? bound_element_array_buffer : 0;
    dump_buffer_upload(target, buffer, offset, data, size, buffer_uploads);
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
    if (sf_trace_gl)
        fprintf(stderr,
                "[bc/gl] attrib-pointer index=%u size=%d type=%#x norm=%u "
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
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] attrib-enable index=%u\n", index);
    if (!real)
        real = gl_raw("glEnableVertexAttribArray");
    if (real)
        real(index);
}

static void my_glDisableVertexAttribArray(GLuint index)
{
    static void (*real)(GLuint);
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] attrib-disable index=%u\n", index);
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
    if (texture < SF_TEX_TABLE &&
        (texture_kinds[texture] == SF_TEX_ETC1_OPAQUE ||
         texture_kinds[texture] == SF_TEX_ETC1_DUAL))
        (void)etc1_demote_texture(texture, "anexo de framebuffer");
    if (sf_trace_gl)
        fprintf(stderr,
                "[bc/gl] fbo-texture fbo=%u target=%#x attachment=%#x "
                "textarget=%#x texture=%u level=%d\n",
                gl_state.framebuffer, target, attachment, textarget, texture,
                level);
    if (!real)
        real = gl_raw("glFramebufferTexture2D");
    if (real)
        real(target, attachment, textarget, texture, level);
    if (attachment == GL_COLOR_ATTACHMENT0 &&
        gl_state.framebuffer < SF_FBO_TABLE)
        framebuffer_color_textures[gl_state.framebuffer] = texture;
}

static void my_glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                         GLenum renderbuffer_target,
                                         GLuint renderbuffer)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint);
    if (sf_trace_gl)
        fprintf(stderr,
                "[bc/gl] fbo-renderbuffer fbo=%u target=%#x attachment=%#x "
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
    if (sf_trace_gl)
        fprintf(stderr,
                "[bc/gl] renderbuffer-storage target=%#x internal=%#x "
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
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] fbo-status fbo=%u target=%#x status=%#x\n",
                gl_state.framebuffer, target, status);
    return status;
}

static void trace_current_fbo_status(void)
{
    if (!sf_trace_gl || gl_state.framebuffer == 0 || draw_calls > 20)
        return;
    static GLenum (*check)(GLenum);
    if (!check)
        check = gl_raw("glCheckFramebufferStatus");
    if (check)
        fprintf(stderr, "[bc/gl] pre-draw fbo=%u status=%#x\n",
                gl_state.framebuffer, check(GL_FRAMEBUFFER));
}

static GLint my_glGetUniformLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetUniformLocation");
    GLint location = real ? real(program, name) : -1;
    remember_uniform(program, location, name);
    sf_program_state *state = program_state(program, 1);
    if (state && name) {
        if (strcmp(name, "_MainTex") == 0)
            state->loc_main = location;
        else if (strcmp(name, "_AlphaTex") == 0)
            state->loc_alpha = location;
        else if (strcmp(name, "_EnableExternalAlpha") == 0)
            state->loc_enable = location;
    }
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] uniform program=%u location=%d name=%s\n",
                program, location, name ? name : "(null)");
    return location;
}

static void my_glUniform1i(GLint location, GLint value)
{
    static void (*real)(GLint, GLint);
    if (!real)
        real = gl_raw("glUniform1i");
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] uniform1i location=%d name=%s value=%d\n",
                location, uniform_label(location), value);
    sf_program_state *state = program_state(current_program, 0);
    if (state) {
        if (state->loc_main >= 0 && location == state->loc_main) {
            state->unit_main = value;
            state->main_seen = 1;
        }
        if (state->loc_alpha >= 0 && location == state->loc_alpha) {
            state->unit_alpha = value;
            state->alpha_seen = 1;
        }
    }
    if (real)
        real(location, value);
}

static void my_glUniform1iv(GLint location, GLsizei count, const GLint *value)
{
    static void (*real)(GLint, GLsizei, const GLint *);
    if (!real)
        real = gl_raw("glUniform1iv");
    if (value && count > 0) {
        sf_program_state *state = program_state(current_program, 0);
        if (state) {
            if (state->loc_main >= 0 && location == state->loc_main) {
                state->unit_main = value[0];
                state->main_seen = 1;
            }
            if (state->loc_alpha >= 0 && location == state->loc_alpha) {
                state->unit_alpha = value[0];
                state->alpha_seen = 1;
            }
        }
    }
    if (real)
        real(location, count, value);
}

static void my_glUniform1f(GLint location, GLfloat value)
{
    static void (*real)(GLint, GLfloat);
    if (!real)
        real = gl_raw("glUniform1f");
    sf_program_state *state = program_state(current_program, 0);
    if (state && state->loc_enable >= 0 && location == state->loc_enable) {
        state->enable_value = value;
        state->enable_seen = 1;
    }
    if (real)
        real(location, value);
}

static void my_glUniform1fv(GLint location, GLsizei count,
                            const GLfloat *value)
{
    static void (*real)(GLint, GLsizei, const GLfloat *);
    if (!real)
        real = gl_raw("glUniform1fv");
    if (value && count > 0) {
        sf_program_state *state = program_state(current_program, 0);
        if (state && state->loc_enable >= 0 &&
            location == state->loc_enable) {
            state->enable_value = value[0];
            state->enable_seen = 1;
        }
    }
    if (real)
        real(location, count, value);
}

static void my_glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z,
                           GLfloat w)
{
    static void (*real)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    if (!real)
        real = gl_raw("glUniform4f");
    if (sf_trace_gl)
        fprintf(stderr,
                "[bc/gl] uniform4f location=%d name=%s "
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
    if (sf_trace_gl && value && count > 0) {
        fprintf(stderr,
                "[bc/gl] uniform4fv location=%d name=%s count=%d "
                "first=%.9g,%.9g,%.9g,%.9g\n",
                location, uniform_label(location), count,
                value[0], value[1], value[2], value[3]);
        if (count == 4)
            fprintf(stderr,
                    "[bc/gl] uniform4fv-matrix location=%d name=%s "
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

static GLenum video_texture_target(GLenum target)
{
    /* SurfaceTexture normally binds an EGLImage to
     * GL_TEXTURE_EXTERNAL_OES.  Our decoded RGBA frame is a regular GLES2
     * texture, so both the binding target and Hidden/VideoDecodeAndroid's
     * sampler are lowered to texture2D. */
    return target == GL_TEXTURE_EXTERNAL_OES ? GL_TEXTURE_2D : target;
}

static void my_glBindTexture(GLenum target, GLuint texture)
{
    static void (*real)(GLenum, GLuint);
    GLenum native_target = video_texture_target(target);
    texture_binds++;
    if (native_target == GL_TEXTURE_2D) {
        bound_texture_2d = texture;
        int unit = (int)tracked_active_texture_unit() - (int)GL_TEXTURE0;
        if (unit >= 0 && unit < SF_TEXTURE_UNITS)
            bound_texture_units[unit] = texture;
    }
    if (sf_trace_gl && texture_binds <= 160)
        fprintf(stderr,
                "[bc/gl] tex-bind #%lu unit=%#x target=%#x texture=%u\n",
                texture_binds, tracked_active_texture_unit(), target, texture);
    if (!real)
        real = gl_raw("glBindTexture");
    if (real)
        real(native_target, texture);
}

static void my_glDeleteTextures(GLsizei count, const GLuint *textures)
{
    static void (*real)(GLsizei, const GLuint *);
    if (!real)
        real = gl_raw("glDeleteTextures");
    if (textures && count > 0) {
        for (GLsizei index = 0; index < count; index++) {
            GLuint texture = textures[index];
            if (texture < SF_TEX_TABLE)
                etc1_reset_texture(texture, 1);
            for (int unit = 0; unit < SF_TEXTURE_UNITS; unit++)
                if (bound_texture_units[unit] == texture)
                    bound_texture_units[unit] = 0;
            if (bound_texture_2d == texture)
                bound_texture_2d = 0;
        }
    }
    if (real)
        real(count, textures);
}

static void my_glGenerateMipmap(GLenum target)
{
    static void (*real)(GLenum);
    GLenum native_target = video_texture_target(target);
    if (native_target == GL_TEXTURE_2D &&
        bound_texture_2d < SF_TEX_TABLE &&
        (texture_kinds[bound_texture_2d] == SF_TEX_ETC1_OPAQUE ||
         texture_kinds[bound_texture_2d] == SF_TEX_ETC1_DUAL))
        (void)etc1_demote_texture(bound_texture_2d, "GenerateMipmap");
    if (!real)
        real = gl_raw("glGenerateMipmap");
    if (real)
        real(native_target);
}

static void my_glPixelStorei(GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLint);
    if (pname == GL_UNPACK_ALIGNMENT &&
        (value == 1 || value == 2 || value == 4 || value == 8))
        unpack_alignment = value;
    if (sf_trace_gl)
        fprintf(stderr, "[bc/gl] pixel-store pname=%#x value=%d\n",
                pname, value);
    if (!real)
        real = gl_raw("glPixelStorei");
    if (real)
        real(pname, value);
}

static void my_glTexParameteri(GLenum target, GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLenum, GLint);
    static void (*bind_texture)(GLenum, GLuint);
    GLenum native_target = video_texture_target(target);
    texture_parameters++;
    if (sf_trace_gl && texture_parameters <= 240)
        fprintf(stderr,
                "[bc/gl] tex-param #%lu unit=%#x texture=%u target=%#x "
                "pname=%#x value=%#x\n",
                texture_parameters, tracked_active_texture_unit(),
                bound_texture_2d,
                target, pname, value);
    if (!real)
        real = gl_raw("glTexParameteri");
    if (real) {
        real(native_target, pname, value);
        if (native_target == GL_TEXTURE_2D &&
            bound_texture_2d < SF_TEX_TABLE &&
            texture_twins[bound_texture_2d]) {
            if (!bind_texture)
                bind_texture = gl_raw("glBindTexture");
            if (bind_texture) {
                GLuint texture = bound_texture_2d;
                bind_texture(native_target, texture_twins[texture]);
                real(native_target, pname, value);
                bind_texture(native_target, texture);
            }
        }
    }
}

static void my_glTexParameterf(GLenum target, GLenum pname, GLfloat value)
{
    static void (*real)(GLenum, GLenum, GLfloat);
    static void (*bind_texture)(GLenum, GLuint);
    GLenum native_target = video_texture_target(target);
    if (!real)
        real = gl_raw("glTexParameterf");
    if (!real)
        return;
    real(native_target, pname, value);
    if (native_target == GL_TEXTURE_2D &&
        bound_texture_2d < SF_TEX_TABLE &&
        texture_twins[bound_texture_2d]) {
        if (!bind_texture)
            bind_texture = gl_raw("glBindTexture");
        if (bind_texture) {
            GLuint texture = bound_texture_2d;
            bind_texture(native_target, texture_twins[texture]);
            real(native_target, pname, value);
            bind_texture(native_target, texture);
        }
    }
}

static void my_glTexParameteriv(GLenum target, GLenum pname, const GLint *value)
{
    static void (*real)(GLenum, GLenum, const GLint *);
    static void (*bind_texture)(GLenum, GLuint);
    GLenum native_target = video_texture_target(target);
    if (!real)
        real = gl_raw("glTexParameteriv");
    if (!real)
        return;
    real(native_target, pname, value);
    if (native_target == GL_TEXTURE_2D &&
        bound_texture_2d < SF_TEX_TABLE &&
        texture_twins[bound_texture_2d]) {
        if (!bind_texture)
            bind_texture = gl_raw("glBindTexture");
        if (bind_texture) {
            GLuint texture = bound_texture_2d;
            bind_texture(native_target, texture_twins[texture]);
            real(native_target, pname, value);
            bind_texture(native_target, texture);
        }
    }
}

static void my_glTexParameterfv(GLenum target, GLenum pname,
                                const GLfloat *value)
{
    static void (*real)(GLenum, GLenum, const GLfloat *);
    static void (*bind_texture)(GLenum, GLuint);
    GLenum native_target = video_texture_target(target);
    if (!real)
        real = gl_raw("glTexParameterfv");
    if (!real)
        return;
    real(native_target, pname, value);
    if (native_target == GL_TEXTURE_2D &&
        bound_texture_2d < SF_TEX_TABLE &&
        texture_twins[bound_texture_2d]) {
        if (!bind_texture)
            bind_texture = gl_raw("glBindTexture");
        if (bind_texture) {
            GLuint texture = bound_texture_2d;
            bind_texture(native_target, texture_twins[texture]);
            real(native_target, pname, value);
            bind_texture(native_target, texture);
        }
    }
}

int sf_gl_video_upload(unsigned texture, const unsigned char *rgba,
                       int width, int height, int stride)
{
    if (!texture || !rgba || width <= 0 || height <= 0 ||
        stride != width * 4)
        return 0;
    static void (*get_int)(GLenum, GLint *);
    static void (*active)(GLenum);
    static void (*bind)(GLenum, GLuint);
    static void (*pixel_store)(GLenum, GLint);
    static void (*image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                         GLenum, GLenum, const void *);
    static void (*sub_image)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
                             GLenum, GLenum, const void *);
    static void (*parameter)(GLenum, GLenum, GLint);
    if (!get_int) get_int = gl_raw("glGetIntegerv");
    if (!active) active = gl_raw("glActiveTexture");
    if (!bind) bind = gl_raw("glBindTexture");
    if (!pixel_store) pixel_store = gl_raw("glPixelStorei");
    if (!image) image = gl_raw("glTexImage2D");
    if (!sub_image) sub_image = gl_raw("glTexSubImage2D");
    if (!parameter) parameter = gl_raw("glTexParameteri");
    if (!get_int || !active || !bind || !pixel_store || !image)
        return 0;

    GLint old_active = GL_TEXTURE0;
    GLint old_binding = 0;
    GLint old_unpack = 4;
    get_int(GL_ACTIVE_TEXTURE, &old_active);
    active(GL_TEXTURE0);
    get_int(GL_TEXTURE_BINDING_2D, &old_binding);
    get_int(GL_UNPACK_ALIGNMENT, &old_unpack);
    bind(GL_TEXTURE_2D, texture);
    pixel_store(GL_UNPACK_ALIGNMENT, 4);

    int known = texture < SF_TEX_TABLE &&
                texture_widths[texture] == width &&
                texture_heights[texture] == height;
    if (known && sub_image) {
        sub_image(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
    } else {
        if (texture < SF_TEX_TABLE)
            etc1_reset_texture(texture, 1);
        image(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
              GL_UNSIGNED_BYTE, rgba);
        if (parameter) {
            parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            parameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            parameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (texture < SF_TEX_TABLE) {
            texture_widths[texture] = width;
            texture_heights[texture] = height;
            texture_kinds[texture] = SF_TEX_NORMAL;
        }
    }

    pixel_store(GL_UNPACK_ALIGNMENT, old_unpack);
    bind(GL_TEXTURE_2D, (GLuint)old_binding);
    active((GLenum)old_active);
    static unsigned long uploads;
    uploads++;
    if (getenv("SF_MEDIALOG") &&
        (uploads == 1 || uploads % 300 == 0))
        fprintf(stderr, "[sf/media] GL video texture=%u frame=%lu %dx%d\n",
                texture, uploads, width, height);
    return 1;
}

/* AImageReader on Android exposes each decoded frame as an AHardwareBuffer.
 * Unity imports that buffer through EGLImage and binds it to a texture.  Our
 * FFmpeg-backed media bridge stores ordinary RGBA, so perform the equivalent
 * upload synchronously when Unity targets the fake image. */
static void my_glEGLImageTargetTexture2DOES(GLenum target,
                                            GLeglImageOES image_handle)
{
    const unsigned char *pixels = NULL;
    int width = 0;
    int height = 0;
    int stride = 0;
    if (sf_media_egl_image_rgba(image_handle, &pixels, &width, &height,
                                &stride)) {
        if (video_texture_target(target) != GL_TEXTURE_2D ||
            !bound_texture_2d ||
            !sf_gl_video_upload(bound_texture_2d, pixels, width, height,
                                stride))
            fprintf(stderr,
                    "[sf/media] falha ao importar EGLImage na textura=%u "
                    "target=%#x %dx%d\n",
                    bound_texture_2d, target, width, height);
        return;
    }

    static void (*real)(GLenum, GLeglImageOES);
    if (!real)
        real = gl_raw("glEGLImageTargetTexture2DOES");
    if (!real) {
        static void *(*get_proc)(const char *);
        if (!get_proc)
            get_proc = sys("eglGetProcAddress");
        if (get_proc)
            real = get_proc("glEGLImageTargetTexture2DOES");
    }
    if (real)
        real(target, image_handle);
}

/* ---------------------------------------------------------------------------
 * Redutor seletivo dos perfis oficiais.
 *
 * low reduz arte RGBA/ETC2 estatica acima de 1024 px; medium usa apenas o
 * mip limit oficial da Unity; high preserva o tamanho original. Atlas dinamico, render target e
 * glifo de um/dois canais passam intactos; o cursor do port e code-native. O
 * clamp medido pelo limite fisico do GPU continua obrigatorio em todos.
 * ------------------------------------------------------------------------- */
static int tex_half_min = -1;        /* lido do ambiente na 1a textura; 0 desliga */

static void tex_half_init(void)
{
    if (tex_half_min >= 0)
        return;
    const char *v = getenv("SF_TEX_HALF_MIN");
    tex_half_min = v && *v ? atoi(v) : 0;
    if (tex_half_min < 0)
        tex_half_min = 0;
    fprintf(stderr, "[sf/tex] meia-res acima de %dpx%s\n", tex_half_min,
            tex_half_min ? "" : " (DESLIGADA)");
}

/* O container BYO-data chega com as duas texturas de fundo do episodio 3 no
 * tamanho original (os3_BG 6252x1607, os3_BG2 6018x833) -- acima do limite
 * fisico de 4096 px do Mali-450.  Subir isso direto e' GL_INVALID_VALUE e
 * textura branca.  O clamp so' age quando a dimensao EXCEDE o
 * GL_MAX_TEXTURE_SIZE medido no contexto real (na Mali-G31, 8192, nada e'
 * tocado); o redutor de perfil continua seletivo e nunca altera dados offline. */
static GLint sf_max_texture_size = 0;
static GLint max_texture_size(void)
{
    if (sf_max_texture_size > 0)
        return sf_max_texture_size;
    void (*getiv)(GLenum, GLint *) =
        (void (*)(GLenum, GLint *))gl_raw("glGetIntegerv");
    GLint v = 0;
    if (getiv)
        getiv(GL_MAX_TEXTURE_SIZE, &v);
    if (v <= 0)
        v = 4096;             /* piso conservador se a consulta falhar */
    sf_max_texture_size = v;
    fprintf(stderr, "[sf/tex] GL_MAX_TEXTURE_SIZE=%d\n", v);
    return v;
}

static int tex_exceeds_hw(GLsizei width, GLsizei height)
{
    GLint max = max_texture_size();
    return width > max || height > max;
}

static int tex_should_half(GLsizei width, GLsizei height)
{
    tex_half_init();
    int profile = tex_half_min > 0 &&
                  (width >= tex_half_min || height >= tex_half_min);
    if (profile) {
        int drawable_width = 0;
        int drawable_height = 0;
        sf_window_get_size(&drawable_width, &drawable_height);
        /* A textura intermediaria da apresentacao pode chegar com pixels
         * iniciais e nao e' um asset. Nunca reduza algo que caiba inteiro no
         * drawable; isso preserva backbuffer/FBO em 640x480 e 1280x720. */
        if (drawable_width > 0 && drawable_height > 0 &&
            width <= drawable_width && height <= drawable_height)
            profile = 0;
    }
    return profile || tex_exceeds_hw(width, height);
}
static unsigned long tex_halved, tex_halved_saved_kb;

static uint8_t tex_is_halved[SF_TEX_TABLE];

static int etc1_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("SF_ETC1");
        enabled = !value || strcmp(value, "0") != 0;
    }
    return enabled;
}

static int etc1_min_dimension(void)
{
    static int minimum = -1;
    if (minimum < 0) {
        const char *value = getenv("SF_ETC1_MIN");
        minimum = value && *value ? atoi(value) : 64;
        if (minimum < 4)
            minimum = 4;
        fprintf(stderr, "[sf/etc1] split RGB+alpha ativo acima de %dpx\n",
                minimum);
    }
    return minimum;
}

static int native_etc1_support(void)
{
    static int supported = -1;
    if (supported >= 0)
        return supported;
    const GLubyte *(*get_string)(GLenum) = gl_raw("glGetString");
    const char *extensions = get_string
                           ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    supported = extensions &&
                strstr(extensions, "GL_OES_compressed_ETC1_RGB8_texture");
    fprintf(stderr, "[sf/etc1] driver ETC1: %s\n",
            supported ? "sim" : "nao");
    return supported;
}

static int etc1_cache_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("SF_ETC1_CACHE");
        enabled = !value || strcmp(value, "0") != 0;
    }
    return enabled;
}

static int etc1_cache_abi_valid(const char *abi)
{
    if (!abi || strlen(abi) != 64)
        return 0;
    for (size_t index = 0; index < 64; index++)
        if (!((abi[index] >= '0' && abi[index] <= '9') ||
              (abi[index] >= 'a' && abi[index] <= 'f')))
            return 0;
    return 1;
}

static const char *etc1_cache_directory(void)
{
    static char directory[1200];
    static int initialized;
    if (!initialized) {
        const char *abi = getenv("SF_ETC1_CACHE_ABI");
        char root[1100];
        int root_length = snprintf(root, sizeof root, "%s/etc1-cache", sf_home);
        if (!etc1_cache_abi_valid(abi)) {
            directory[0] = '\0';
            fprintf(stderr,
                    "[sf/etc1] cache desligado: SF_ETC1_CACHE_ABI invalido\n");
        } else if (root_length < 0 || (size_t)root_length >= sizeof root ||
                   (mkdir(root, 0755) != 0 && errno != EEXIST)) {
            directory[0] = '\0';
        } else {
            int length = snprintf(directory, sizeof directory, "%s/%s", root, abi);
            if (length < 0 || (size_t)length >= sizeof directory ||
                (mkdir(directory, 0755) != 0 && errno != EEXIST))
                directory[0] = '\0';
        }
        initialized = 1;
    }
    return directory[0] ? directory : NULL;
}

static int etc1_cache_path(char *path, size_t capacity, char plane,
                           uint64_t hash, int width, int height, int level)
{
    const char *directory = etc1_cache_directory();
    if (!directory)
        return 0;
    int length = snprintf(path, capacity,
                          "%s/%016llx-%dx%d-l%d-%c.etc1", directory,
                          (unsigned long long)hash, width, height, level, plane);
    return length >= 0 && (size_t)length < capacity;
}

static int etc1_cache_read(const char *path, void *data, size_t size)
{
    if (!path || !data)
        return 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    int exact = fseek(file, 0, SEEK_END) == 0 &&
                ftell(file) == (long)size &&
                fseek(file, 0, SEEK_SET) == 0 &&
                fread(data, 1, size, file) == size;
    if (fclose(file) != 0)
        exact = 0;
    return exact;
}

static void etc1_cache_write(const char *path, const void *data, size_t size)
{
    if (!path || !data)
        return;
    static unsigned sequence;
    unsigned ticket = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
    char temporary[1280];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp.%ld.%u", path,
                          (long)getpid(), ticket);
    if (length < 0 || (size_t)length >= sizeof temporary)
        return;
    FILE *file = fopen(temporary, "wb");
    if (!file)
        return;
    int complete = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0)
        complete = 0;
    if (complete) {
        if (rename(temporary, path) != 0)
            unlink(temporary);
    } else {
        unlink(temporary);
    }
}

static uint64_t etc1_hash_rgba(const uint8_t *rgba, int width, int height,
                               size_t stride, int *has_alpha,
                               int *has_soft_alpha, int *is_grayscale)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    int alpha = 0;
    int soft_alpha = 0;
    int grayscale = 1;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = rgba + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            const uint8_t *pixel = row + (size_t)x * 4u;
            for (int channel = 0; channel < 4; channel++) {
                hash ^= pixel[channel];
                hash *= UINT64_C(1099511628211);
            }
            alpha |= pixel[3] != 255;
            soft_alpha |= pixel[3] != 0 && pixel[3] != 255;
            /* RGB below a fully transparent texel is semantically invisible
             * and Unity importers need not initialise it consistently. */
            if (pixel[3] != 0)
                grayscale &= pixel[0] == pixel[1] && pixel[1] == pixel[2];
        }
    }
    if (has_alpha)
        *has_alpha = alpha;
    if (has_soft_alpha)
        *has_soft_alpha = soft_alpha;
    if (is_grayscale)
        *is_grayscale = grayscale;
    return hash;
}

/* Blasphemous' palette-swapped character atlases are RGBA images containing
 * a small set of exact RGB keys (normally about 32) and a binary cutout
 * alpha.  Their fragment shader uses those RGB values as coordinates into a
 * 256x1 palette.  ETC1 is visually suitable for ordinary art, but its lossy
 * colour endpoints change the keys and turn the Penitent into magenta/black
 * blocks.  Recognise that source representation before compression and keep
 * only those indexed atlases lossless; ordinary and dual-plane ETC1 remain
 * the default for the rest of the game.
 */
static int etc1_is_palette_indexed_rgba(const uint8_t *rgba, int width,
                                        int height, size_t stride,
                                        unsigned *color_count)
{
    enum { MIN_COLORS = 16, MAX_COLORS = 64, TABLE_SIZE = 128 };
    uint32_t colors[TABLE_SIZE] = { 0 };
    unsigned unique = 0;
    int transparent = 0;
    int opaque = 0;

    if (!rgba || width < 128 || height < 128)
        return 0;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = rgba + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            const uint8_t *pixel = row + (size_t)x * 4u;
            if (pixel[3] == 0) {
                transparent = 1;
                continue;
            }
            if (pixel[3] != 255)
                return 0;
            opaque = 1;
            uint32_t key = ((uint32_t)pixel[0] << 16) |
                           ((uint32_t)pixel[1] << 8) | pixel[2];
            uint32_t stored = key + 1u;
            unsigned slot = (key * UINT32_C(2654435761)) &
                            (TABLE_SIZE - 1u);
            while (colors[slot] && colors[slot] != stored)
                slot = (slot + 1u) & (TABLE_SIZE - 1u);
            if (!colors[slot]) {
                colors[slot] = stored;
                if (++unique > MAX_COLORS)
                    return 0;
            }
        }
    }
    if (color_count)
        *color_count = unique;
    return transparent && opaque && unique >= MIN_COLORS;
}

/* The palette shader reads only _MainTex.z as its 0..255 palette coordinate
 * and _MainTex.w as cutout alpha.  GLES2 LUMINANCE_ALPHA therefore stores the
 * two exact source bytes the shader actually consumes, at half the memory of
 * RGBA and without ETC1's colour loss.  Sampling LA yields (L,L,L,A), so the
 * original shader and native material flow remain untouched.
 */
static uint8_t *palette_luminance_alpha_pixels(const uint8_t *rgba,
                                                GLsizei width,
                                                GLsizei height,
                                                size_t stride)
{
    if (!rgba || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 2u / (size_t)height)
        return NULL;
    size_t pixels = (size_t)width * (size_t)height;
    uint8_t *la = malloc(pixels * 2u);
    if (!la)
        return NULL;
    for (int y = 0; y < height; y++) {
        const uint8_t *source = rgba + (size_t)y * stride;
        uint8_t *dest = la + (size_t)y * (size_t)width * 2u;
        for (int x = 0; x < width; x++) {
            dest[x * 2 + 0] = source[x * 4 + 2];
            dest[x * 2 + 1] = source[x * 4 + 3];
        }
    }
    return la;
}

static int palette_upload_luminance_alpha(GLenum target, GLint level,
                                          GLsizei width, GLsizei height,
                                          GLint border,
                                          const uint8_t *rgba, size_t stride,
                                          uint64_t hash)
{
    if (target != GL_TEXTURE_2D || border != 0)
        return 0;
    uint8_t *la = palette_luminance_alpha_pixels(rgba, width, height, stride);
    if (!la)
        return 0;
    static void (*plain)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                         GLenum, GLenum, const void *);
    static void (*pixel_store)(GLenum, GLint);
    if (!plain)
        plain = gl_raw("glTexImage2D");
    if (!pixel_store)
        pixel_store = gl_raw("glPixelStorei");
    if (!plain) {
        free(la);
        return 0;
    }
    if (pixel_store && unpack_alignment != 1)
        pixel_store(GL_UNPACK_ALIGNMENT, 1);
    plain(target, level, GL_LUMINANCE_ALPHA, width, height, border,
          GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
    if (pixel_store && unpack_alignment != 1)
        pixel_store(GL_UNPACK_ALIGNMENT, unpack_alignment);
    free(la);

    if (level == 0 && bound_texture_2d < SF_TEX_TABLE) {
        texture_kinds[bound_texture_2d] = SF_TEX_PALETTE_LA;
        texture_widths[bound_texture_2d] = width;
        texture_heights[bound_texture_2d] = height;
        texture_hashes[bound_texture_2d] = hash;
    }
    return 1;
}

static const uint8_t *etc1_source_rgba(GLsizei width, GLsizei height,
                                       GLenum format, GLenum type,
                                       const void *data, size_t *stride,
                                       uint8_t **owned)
{
    *owned = NULL;
    if (!data || width <= 0 || height <= 0 || type != GL_UNSIGNED_BYTE)
        return NULL;
    if (format == GL_RGBA) {
        size_t row = (size_t)width * 4u;
        size_t alignment = unpack_alignment > 0
                         ? (size_t)unpack_alignment : 4u;
        *stride = (row + alignment - 1u) & ~(alignment - 1u);
        return data;
    }
    if (format != GL_RGB)
        return NULL;

    size_t source_row = (size_t)width * 3u;
    size_t alignment = unpack_alignment > 0 ? (size_t)unpack_alignment : 4u;
    size_t source_stride = (source_row + alignment - 1u) & ~(alignment - 1u);
    if ((size_t)width > SIZE_MAX / 4u / (size_t)height)
        return NULL;
    uint8_t *rgba = malloc((size_t)width * (size_t)height * 4u);
    if (!rgba)
        return NULL;
    for (int y = 0; y < height; y++) {
        const uint8_t *source = (const uint8_t *)data +
                                (size_t)y * source_stride;
        uint8_t *target = rgba + (size_t)y * (size_t)width * 4u;
        for (int x = 0; x < width; x++) {
            target[x * 4 + 0] = source[x * 3 + 0];
            target[x * 4 + 1] = source[x * 3 + 1];
            target[x * 4 + 2] = source[x * 3 + 2];
            target[x * 4 + 3] = 255;
        }
    }
    *stride = (size_t)width * 4u;
    *owned = rgba;
    return rgba;
}

static void etc1_reset_texture(GLuint texture, int delete_twin)
{
    if (!texture || texture >= SF_TEX_TABLE)
        return;
    GLuint twin = texture_twins[texture];
    if (delete_twin && twin) {
        static void (*delete_textures)(GLsizei, const GLuint *);
        if (!delete_textures)
            delete_textures = gl_raw("glDeleteTextures");
        if (delete_textures)
            delete_textures(1, &twin);
        for (int unit = 0; unit < SF_TEXTURE_UNITS; unit++)
            if (bound_texture_units[unit] == twin)
                bound_texture_units[unit] = 0;
    }
    texture_twins[texture] = 0;
    texture_kinds[texture] = SF_TEX_NORMAL;
    texture_widths[texture] = 0;
    texture_heights[texture] = 0;
    texture_hashes[texture] = 0;
}

static int etc1_upload_rgba(GLenum target, GLint level, GLsizei width,
                            GLsizei height, GLint border, const uint8_t *rgba,
                            size_t stride)
{
    GLuint texture = bound_texture_2d;
    if (!etc1_enabled() || !native_etc1_support() || !rgba ||
        target != GL_TEXTURE_2D || border != 0 || !texture ||
        texture >= SF_TEX_TABLE || width <= 0 || height <= 0)
        return 0;
    if (level == 0) {
        int minimum = etc1_min_dimension();
        /* ETC1 is block based.  A 256x1 palette lookup strip technically
         * uploads, but every four exact palette entries become one lossy
         * colour block and all indexed sprites are then recoloured.  These
         * strips cost only about 1 KiB in RGBA and must stay byte-exact. */
        if (width < 4 || height < 4) {
            if ((width >= 16 || height >= 16) &&
                (width <= 2 || height <= 2))
                fprintf(stderr,
                        "[sf/etc1] tex=%u %dx%d faixa de paleta preservada "
                        "em RGBA exato\n",
                        texture, width, height);
            return 0;
        }
        if (width < minimum && height < minimum)
            return 0;
        etc1_reset_texture(texture, 1);
    } else if (texture_kinds[texture] == SF_TEX_PALETTE_LA) {
        return palette_upload_luminance_alpha(target, level, width, height,
                                              border, rgba, stride,
                                              texture_hashes[texture]);
    } else if (texture_kinds[texture] != SF_TEX_ETC1_OPAQUE &&
               texture_kinds[texture] != SF_TEX_ETC1_DUAL) {
        return 0;
    }

    int has_alpha = 0;
    int has_soft_alpha = 0;
    int is_grayscale = 0;
    uint64_t hash = etc1_hash_rgba(rgba, width, height, stride, &has_alpha,
                                   &has_soft_alpha, &is_grayscale);
    /* Soft monochrome masks are the game's shadows, fog and light falloff.
     * Encoding their alpha as ETC1 quantises it in visible 4x4 squares.
     * GLES2 LUMINANCE_ALPHA is an exact representation of grayscale RGBA at
     * half the original memory, and the existing LA mip/subimage path keeps
     * every later update consistent.  Ordinary artwork still uses essential
     * ETC1 colour + ETC1 external-alpha dual planes. */
    if (level == 0 && has_alpha && has_soft_alpha && is_grayscale) {
        static unsigned long soft_mask_uploads;
        if (palette_upload_luminance_alpha(target, level, width, height,
                                           border, rgba, stride, hash)) {
            soft_mask_uploads++;
            if (soft_mask_uploads <= 20 || soft_mask_uploads % 25 == 0 ||
                getenv("SF_ETC1_LOG"))
                fprintf(stderr,
                        "[sf/etc1] tex=%u %dx%d mascara alpha suave "
                        "preservada em LA8, total=%lu\n",
                        texture, width, height, soft_mask_uploads);
            return 1;
        }
    }
    unsigned palette_colors = 0;
    if (level == 0 && has_alpha &&
        etc1_is_palette_indexed_rgba(rgba, width, height, stride,
                                     &palette_colors)) {
        static unsigned long palette_uploads;
        if (palette_upload_luminance_alpha(target, level, width, height,
                                           border, rgba, stride, hash)) {
            palette_uploads++;
            if (palette_uploads <= 20 || palette_uploads % 25 == 0 ||
                getenv("SF_ETC1_LOG"))
                fprintf(stderr,
                        "[sf/etc1] tex=%u %dx%d paleta indexada (%u cores) "
                        "preservada em LA8, total=%lu\n",
                        texture, width, height, palette_colors,
                        palette_uploads);
            return 1;
        }
    }
    if (level > 0 && texture_kinds[texture] == SF_TEX_ETC1_DUAL)
        has_alpha = 1;
    if (level > 0 && has_alpha &&
        texture_kinds[texture] != SF_TEX_ETC1_DUAL)
        return 0;

    size_t encoded_size = sf_etc1_size(width, height);
    uint8_t *encoded = malloc(encoded_size);
    if (!encoded)
        return 0;
    static void (*compressed)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                              GLsizei, const void *);
    static void (*gen_textures)(GLsizei, GLuint *);
    static void (*bind_texture)(GLenum, GLuint);
    static void (*tex_parameter)(GLenum, GLenum, GLint);
    static void (*get_parameter)(GLenum, GLenum, GLint *);
    if (!compressed)
        compressed = gl_raw("glCompressedTexImage2D");
    if (!gen_textures)
        gen_textures = gl_raw("glGenTextures");
    if (!bind_texture)
        bind_texture = gl_raw("glBindTexture");
    if (!tex_parameter)
        tex_parameter = gl_raw("glTexParameteri");
    if (!get_parameter)
        get_parameter = gl_raw("glGetTexParameteriv");
    if (!compressed || !bind_texture) {
        free(encoded);
        return 0;
    }

    char color_path[1280] = "";
    int use_cache = etc1_cache_enabled() &&
                    etc1_cache_path(color_path, sizeof color_path, 'c', hash,
                                    width, height, level);
    int color_hit = use_cache &&
                    etc1_cache_read(color_path, encoded, encoded_size);
    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    if (!color_hit) {
        sf_etc1_encode_rgba(rgba, width, height, stride, encoded);
        etc1_encodes++;
        if (use_cache)
            etc1_cache_write(color_path, encoded, encoded_size);
    } else {
        etc1_cache_hits++;
    }
    compressed(target, level, GL_ETC1_RGB8_OES, width, height, border,
               (GLsizei)encoded_size, encoded);

    GLuint twin = level == 0 ? 0 : texture_twins[texture];
    int alpha_hit = 0;
    if (has_alpha) {
        if (level == 0 && gen_textures)
            gen_textures(1, &twin);
        if (!twin) {
            free(encoded);
            return 0;
        }
        char alpha_path[1280] = "";
        use_cache = etc1_cache_enabled() &&
                    etc1_cache_path(alpha_path, sizeof alpha_path, 'a', hash,
                                    width, height, level);
        alpha_hit = use_cache &&
                    etc1_cache_read(alpha_path, encoded, encoded_size);
        if (!alpha_hit) {
            sf_etc1_encode_alpha(rgba, width, height, stride, encoded);
            etc1_encodes++;
            if (use_cache)
                etc1_cache_write(alpha_path, encoded, encoded_size);
        } else {
            etc1_cache_hits++;
        }

        GLint parameters[4] = { GL_LINEAR, GL_LINEAR,
                                GL_REPEAT, GL_REPEAT };
        const GLenum names[4] = { GL_TEXTURE_MIN_FILTER,
                                  GL_TEXTURE_MAG_FILTER,
                                  GL_TEXTURE_WRAP_S,
                                  GL_TEXTURE_WRAP_T };
        if (level == 0 && get_parameter)
            for (int index = 0; index < 4; index++)
                get_parameter(target, names[index], &parameters[index]);
        bind_texture(target, twin);
        if (level == 0 && tex_parameter)
            for (int index = 0; index < 4; index++)
                tex_parameter(target, names[index], parameters[index]);
        compressed(target, level, GL_ETC1_RGB8_OES, width, height, border,
                   (GLsizei)encoded_size, encoded);
        bind_texture(target, texture);
    }
    free(encoded);

    if (level == 0) {
        texture_twins[texture] = twin;
        texture_kinds[texture] = has_alpha ? SF_TEX_ETC1_DUAL
                                           : SF_TEX_ETC1_OPAQUE;
        texture_widths[texture] = width;
        texture_heights[texture] = height;
        texture_hashes[texture] = hash;
        if (has_alpha)
            etc1_dual_uploads++;
        else
            etc1_opaque_uploads++;
    }
    clock_gettime(CLOCK_MONOTONIC, &finished);
    if (level == 0 &&
        (etc1_dual_uploads + etc1_opaque_uploads <= 100 ||
         getenv("SF_ETC1_LOG"))) {
        long elapsed_ms = (finished.tv_sec - started.tv_sec) * 1000L +
                          (finished.tv_nsec - started.tv_nsec) / 1000000L;
        fprintf(stderr,
                "[sf/etc1] tex=%u %dx%d %s cache=%c%c encode=%ldms "
                "totals opaque=%lu dual=%lu hits=%lu\n",
                texture, width, height, has_alpha ? "dual" : "opaque",
                color_hit ? 'C' : '-', alpha_hit ? 'A' : '-', elapsed_ms,
                etc1_opaque_uploads, etc1_dual_uploads, etc1_cache_hits);
    }
    return 1;
}

static int etc1_demote_texture(GLuint texture, const char *reason)
{
    if (!texture || texture >= SF_TEX_TABLE ||
        (texture_kinds[texture] != SF_TEX_ETC1_OPAQUE &&
         texture_kinds[texture] != SF_TEX_ETC1_DUAL))
        return 0;
    int width = texture_widths[texture];
    int height = texture_heights[texture];
    uint64_t hash = texture_hashes[texture];
    size_t encoded_size = sf_etc1_size(width, height);
    uint8_t *color = malloc(encoded_size);
    uint8_t *alpha = texture_kinds[texture] == SF_TEX_ETC1_DUAL
                   ? malloc(encoded_size) : NULL;
    char path[1280];
    int ok = color &&
             etc1_cache_path(path, sizeof path, 'c', hash, width, height, 0) &&
             etc1_cache_read(path, color, encoded_size);
    if (ok && alpha)
        ok = etc1_cache_path(path, sizeof path, 'a', hash, width, height, 0) &&
             etc1_cache_read(path, alpha, encoded_size);
    uint8_t *rgba = ok ? sf_etc1_decode_pair(color, alpha, width, height) : NULL;
    free(alpha);
    free(color);
    if (!rgba)
        return 0;

    static void (*bind_texture)(GLenum, GLuint);
    static void (*plain)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                         GLenum, const void *);
    static void (*tex_parameter)(GLenum, GLenum, GLint);
    static void (*delete_textures)(GLsizei, const GLuint *);
    if (!bind_texture)
        bind_texture = gl_raw("glBindTexture");
    if (!plain)
        plain = gl_raw("glTexImage2D");
    if (!tex_parameter)
        tex_parameter = gl_raw("glTexParameteri");
    if (!delete_textures)
        delete_textures = gl_raw("glDeleteTextures");
    if (!bind_texture || !plain) {
        free(rgba);
        return 0;
    }
    int unit = (int)tracked_active_texture_unit() - (int)GL_TEXTURE0;
    GLuint saved = unit >= 0 && unit < SF_TEXTURE_UNITS
                 ? bound_texture_units[unit] : bound_texture_2d;
    bind_texture(GL_TEXTURE_2D, texture);
    plain(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
          GL_UNSIGNED_BYTE, rgba);
    if (tex_parameter)
        tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    bind_texture(GL_TEXTURE_2D, saved);
    free(rgba);

    GLuint twin = texture_twins[texture];
    if (twin && delete_textures)
        delete_textures(1, &twin);
    texture_twins[texture] = 0;
    texture_kinds[texture] = SF_TEX_RGBA_DEMOTED;
    etc1_demotions++;
    fprintf(stderr,
            "[sf/etc1] tex=%u %dx%d demovida para RGBA (%s), total=%lu\n",
            texture, width, height, reason ? reason : "uso dinamico",
            etc1_demotions);
    return 1;
}

static int texel_bytes(GLenum format, GLenum type)
{
    if (type != GL_UNSIGNED_BYTE)
        return 0;                    /* 565/4444/5551 empacotados: nao mexer */
    switch (format) {
    case GL_RGBA: return 4;
    case GL_RGB:  return 3;
    default:      return 0;          /* ALPHA/LUMINANCE/LUMINANCE_ALPHA */
    }
}

/* Filtro 2x2 para arte comum; nearest para atlas indexado, pois fazer media
 * de indices de paleta inventa cores que nao existem na lookup table. */
static void *halve_image(const void *src, GLsizei w, GLsizei h, int bpp,
                         int nearest, GLsizei *out_w, GLsizei *out_h)
{
    if (!src || w <= 0 || h <= 0 || bpp <= 0)
        return NULL;
    GLsizei nw = w > 1 ? w / 2 : 1;
    GLsizei nh = h > 1 ? h / 2 : 1;
    uint8_t *dst = malloc((size_t)nw * nh * bpp);
    if (!dst)
        return NULL;
    const uint8_t *s = src;
    for (GLsizei y = 0; y < nh; y++) {
        GLsizei sy0 = h > 1 ? y * 2 : 0;
        GLsizei sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
        const uint8_t *r0 = s + (size_t)sy0 * w * bpp;
        const uint8_t *r1 = s + (size_t)sy1 * w * bpp;
        uint8_t *o = dst + (size_t)y * nw * bpp;
        for (GLsizei x = 0; x < nw; x++) {
            GLsizei sx0 = w > 1 ? x * 2 : 0;
            GLsizei sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
            const uint8_t *p0 = r0 + (size_t)sx0 * bpp;
            const uint8_t *p1 = r0 + (size_t)sx1 * bpp;
            const uint8_t *p2 = r1 + (size_t)sx0 * bpp;
            const uint8_t *p3 = r1 + (size_t)sx1 * bpp;
            for (int c = 0; c < bpp; c++) {
                o[c] = nearest ? p0[c]
                     : (uint8_t)(((unsigned)p0[c] + p1[c] + p2[c] +
                                  p3[c] + 2) / 4);
            }
            o += bpp;
        }
    }
    *out_w = nw;
    *out_h = nh;
    return dst;
}

static void my_glTexImage2D(GLenum target, GLint level, GLint internal_format,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                        GLenum, const void *);
    texture_images++;
    tex_half_init();
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[bc/gl] tex-image #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d border=%d format=%#x "
                "type=%#x data=%s\n",
                texture_images, tracked_active_texture_unit(),
                bound_texture_2d, target,
                level, internal_format, width, height, border, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexImage2D");
    if (!real)
        return;

    int bpp = texel_bytes(format, type);
    int tracked = bound_texture_2d < SF_TEX_TABLE;
    int big = tex_should_half(width, height);
    int continue_half = tracked && level > 0 &&
                        tex_is_halved[bound_texture_2d];
    const void *upload_data = data;
    GLsizei upload_width = width;
    GLsizei upload_height = height;
    void *half_data = NULL;
    if (level == 0 && tracked)
        tex_is_halved[bound_texture_2d] = 0;
    /* RGBA rows are always naturally aligned, so the reducer cannot mistake
     * GL_UNPACK_ALIGNMENT padding for pixels.  RGB and channel atlases stay
     * untouched. */
    if (data && format == GL_RGBA && bpp == 4 &&
        ((level == 0 && big) || continue_half)) {
        int indexed = texture_kinds[bound_texture_2d] == SF_TEX_PALETTE_LA;
        if (level == 0)
            indexed = etc1_is_palette_indexed_rgba(
                data, width, height, (size_t)width * 4u, NULL);
        half_data = halve_image(data, width, height, bpp, indexed,
                                &upload_width, &upload_height);
        if (half_data) {
            upload_data = half_data;
            if (level == 0 && tracked) {
                tex_is_halved[bound_texture_2d] = 1;
                size_t saved = ((size_t)width * height -
                                (size_t)upload_width * upload_height) * 4u;
                tex_halved++;
                tex_halved_saved_kb += saved / 1024;
                if (sf_trace_gl || tex_halved <= 40)
                    fprintf(stderr,
                            "[sf/tex] meia-res #%lu texture=%u %dx%d -> "
                            "%dx%d (-%zuKB fonte, total -%luMB)\n",
                            tex_halved, bound_texture_2d, width, height,
                            upload_width, upload_height, saved / 1024,
                            tex_halved_saved_kb / 1024);
            }
        }
    }

    uint8_t *owned_rgba = NULL;
    size_t rgba_stride = 0;
    const uint8_t *rgba = etc1_source_rgba(upload_width, upload_height,
                                            format, type, upload_data,
                                            &rgba_stride, &owned_rgba);
    if (rgba && etc1_upload_rgba(target, level, upload_width, upload_height,
                                 border, rgba, rgba_stride)) {
        free(owned_rgba);
        free(half_data);
        return;
    }
    free(owned_rgba);
    if (tracked) {
        if (level == 0) {
            etc1_reset_texture(bound_texture_2d, 1);
            texture_widths[bound_texture_2d] = upload_width;
            texture_heights[bound_texture_2d] = upload_height;
        } else if (texture_kinds[bound_texture_2d] == SF_TEX_ETC1_OPAQUE ||
                 texture_kinds[bound_texture_2d] == SF_TEX_ETC1_DUAL)
            (void)etc1_demote_texture(bound_texture_2d,
                                      "TexImage incompativel");
    }
    real(target, level, internal_format, upload_width, upload_height, border,
         format, type, upload_data);
    free(half_data);
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
                "[bc/gl] tex-sub #%lu unit=%#x texture=%u target=%#x "
                "level=%d xy=%d,%d size=%dx%d format=%#x type=%#x data=%s\n",
                texture_sub_images, tracked_active_texture_unit(),
                bound_texture_2d,
                target, level, x, y, width, height, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexSubImage2D");
    if (!real)
        return;

    const void *upload_data = data;
    GLint upload_x = x;
    GLint upload_y = y;
    GLsizei upload_width = width;
    GLsizei upload_height = height;
    void *half_data = NULL;
    int tracked = bound_texture_2d < SF_TEX_TABLE;
    if (level == 0 && data && format == GL_RGBA &&
        type == GL_UNSIGNED_BYTE && tracked &&
        tex_is_halved[bound_texture_2d]) {
        int indexed = texture_kinds[bound_texture_2d] == SF_TEX_PALETTE_LA;
        half_data = halve_image(data, width, height, 4, indexed,
                                &upload_width, &upload_height);
        if (half_data) {
            upload_data = half_data;
            upload_x = x / 2;
            upload_y = y / 2;
        }
    }

    if (data && bound_texture_2d < SF_TEX_TABLE &&
        texture_kinds[bound_texture_2d] == SF_TEX_PALETTE_LA) {
        uint8_t *owned_rgba = NULL;
        size_t rgba_stride = 0;
        const uint8_t *rgba = etc1_source_rgba(
            upload_width, upload_height, format, type, upload_data,
            &rgba_stride, &owned_rgba);
        uint8_t *la = palette_luminance_alpha_pixels(
            rgba, upload_width, upload_height, rgba_stride);
        if (la) {
            static void (*pixel_store)(GLenum, GLint);
            if (!pixel_store)
                pixel_store = gl_raw("glPixelStorei");
            if (pixel_store && unpack_alignment != 1)
                pixel_store(GL_UNPACK_ALIGNMENT, 1);
            real(target, level, upload_x, upload_y, upload_width,
                 upload_height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
            if (pixel_store && unpack_alignment != 1)
                pixel_store(GL_UNPACK_ALIGNMENT, unpack_alignment);
            free(la);
            free(owned_rgba);
            free(half_data);
            return;
        }
        free(owned_rgba);
    }

    if (bound_texture_2d < SF_TEX_TABLE &&
        (texture_kinds[bound_texture_2d] == SF_TEX_ETC1_OPAQUE ||
         texture_kinds[bound_texture_2d] == SF_TEX_ETC1_DUAL))
        (void)etc1_demote_texture(bound_texture_2d, "TexSubImage dinamica");

    real(target, level, upload_x, upload_y, upload_width, upload_height,
         format, type, upload_data);
    free(half_data);
}

typedef struct {
    sf_program_state *program;
    int armed;
    int alpha_unit;
    int saved_active_unit;
    GLuint saved_alpha_texture;
    GLint saved_alpha_sampler;
    GLfloat saved_enable;
    uint8_t restore_sampler;
    uint8_t restore_enable;
} sf_etc1_draw_state;

static void etc1_before_draw(sf_etc1_draw_state *draw)
{
    memset(draw, 0, sizeof *draw);
    draw->alpha_unit = -1;
    sf_program_state *program = program_state(current_program, 0);
    if (!program)
        return;
    int main_unit = program->main_seen ? program->unit_main : 0;
    if (main_unit < 0 || main_unit >= SF_TEXTURE_UNITS)
        return;
    GLuint texture = bound_texture_units[main_unit];
    if (!texture || texture >= SF_TEX_TABLE ||
        (texture_kinds[texture] != SF_TEX_ETC1_OPAQUE &&
         texture_kinds[texture] != SF_TEX_ETC1_DUAL))
        return;

    static void (*uniform1f)(GLint, GLfloat);
    static void (*uniform1i)(GLint, GLint);
    static void (*active_texture)(GLenum);
    static void (*bind_texture)(GLenum, GLuint);
    if (!uniform1f)
        uniform1f = gl_raw("glUniform1f");
    if (!uniform1i)
        uniform1i = gl_raw("glUniform1i");
    if (!active_texture)
        active_texture = gl_raw("glActiveTexture");
    if (!bind_texture)
        bind_texture = gl_raw("glBindTexture");

    draw->program = program;
    draw->saved_enable = program->enable_seen ? program->enable_value : 0.0f;
    if (texture_kinds[texture] == SF_TEX_ETC1_OPAQUE) {
        if (program->loc_enable >= 0 && uniform1f) {
            uniform1f(program->loc_enable, 0.0f);
            draw->restore_enable = 1;
            draw->armed = 1;
        }
        return;
    }

    /* A shader without Unity's external-alpha variant cannot consume the
     * second ETC1 plane.  Reconstruct only that actually-used texture as RGBA;
     * silently drawing its ETC1 RGB as opaque is the "missing UI" failure. */
    if (program->loc_alpha < 0) {
        if (!program->missing_alpha_logged) {
            program->missing_alpha_logged = 1;
            fprintf(stderr,
                    "[sf/etc1] program=%u sem _AlphaTex; tex=%u volta a RGBA\n",
                    program->program, texture);
        }
        (void)etc1_demote_texture(texture, "shader sem _AlphaTex");
        return;
    }
    if (!active_texture || !bind_texture || !uniform1i)
        return;

    /* Injected samplers are unknown to Unity, so reserve the highest fragment
     * texture unit reported by the driver.  Unit 1 is not safe: GUI shaders use
     * it for clip/mask textures.  Native external-alpha variants keep the unit
     * explicitly assigned by Unity when one was observed. */
    static __thread GLint max_fragment_units;
    if (max_fragment_units <= 0) {
        static void (*get_integer)(GLenum, GLint *);
        if (!get_integer)
            get_integer = gl_raw("glGetIntegerv");
        if (get_integer)
            get_integer(GL_MAX_TEXTURE_IMAGE_UNITS, &max_fragment_units);
        if (max_fragment_units > SF_TEXTURE_UNITS)
            max_fragment_units = SF_TEXTURE_UNITS;
    }
    if (max_fragment_units < 2)
        return;
    int alpha_unit = program->alpha_seen ? program->unit_alpha
                                         : max_fragment_units - 1;
    if (alpha_unit == main_unit)
        alpha_unit--;
    if (alpha_unit < 0 || alpha_unit >= max_fragment_units)
        return;
    int active_unit = (int)tracked_active_texture_unit() - (int)GL_TEXTURE0;
    if (active_unit < 0 || active_unit >= SF_TEXTURE_UNITS)
        active_unit = 0;

    draw->armed = 1;
    draw->alpha_unit = alpha_unit;
    draw->saved_active_unit = active_unit;
    draw->saved_alpha_texture = bound_texture_units[alpha_unit];
    draw->saved_alpha_sampler = program->alpha_seen ? program->unit_alpha : 0;

    active_texture(GL_TEXTURE0 + alpha_unit);
    bind_texture(GL_TEXTURE_2D, texture_twins[texture]);
    active_texture(GL_TEXTURE0 + active_unit);
    uniform1i(program->loc_alpha, alpha_unit);
    draw->restore_sampler = 1;
    if (program->loc_enable >= 0 && uniform1f) {
        uniform1f(program->loc_enable, 1.0f);
        draw->restore_enable = 1;
    }

    static unsigned long bindings;
    bindings++;
    if (bindings <= 100 || getenv("SF_ETC1_BIND_LOG"))
        fprintf(stderr,
                "[sf/etc1] bind #%lu prog=%u main=u%d tex=%u alpha=u%d "
                "twin=%u enable=%d\n",
                bindings, program->program, main_unit, texture, alpha_unit,
                texture_twins[texture], program->loc_enable);
}

static void etc1_after_draw(sf_etc1_draw_state *draw)
{
    if (!draw || !draw->armed || !draw->program)
        return;
    static void (*uniform1f)(GLint, GLfloat);
    static void (*uniform1i)(GLint, GLint);
    static void (*active_texture)(GLenum);
    static void (*bind_texture)(GLenum, GLuint);
    if (!uniform1f)
        uniform1f = gl_raw("glUniform1f");
    if (!uniform1i)
        uniform1i = gl_raw("glUniform1i");
    if (!active_texture)
        active_texture = gl_raw("glActiveTexture");
    if (!bind_texture)
        bind_texture = gl_raw("glBindTexture");
    if (draw->restore_enable && uniform1f)
        uniform1f(draw->program->loc_enable, draw->saved_enable);
    if (draw->restore_sampler && uniform1i)
        uniform1i(draw->program->loc_alpha, draw->saved_alpha_sampler);
    if (draw->alpha_unit >= 0 && active_texture && bind_texture) {
        active_texture(GL_TEXTURE0 + draw->alpha_unit);
        bind_texture(GL_TEXTURE_2D, draw->saved_alpha_texture);
        active_texture(GL_TEXTURE0 + draw->saved_active_unit);
    }
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real)
        real = gl_raw("glDrawArrays");
    draw_calls++;
    if (gl_state.framebuffer < SF_FBO_TABLE)
        framebuffer_frame_draws[gl_state.framebuffer]++;
    if (sf_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr,
                "[bc/gl] draw #%lu arrays mode=%#x first=%d count=%d "
                "program=%u fbo=%u vbo=%u\n",
                draw_calls, mode, first, count, current_program,
                gl_state.framebuffer, bound_array_buffer);
    trace_current_fbo_status();
    sf_etc1_draw_state etc1_draw;
    etc1_before_draw(&etc1_draw);
    real(mode, first, count);
    etc1_after_draw(&etc1_draw);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices)
{
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real)
        real = gl_raw("glDrawElements");
    draw_calls++;
    if (gl_state.framebuffer < SF_FBO_TABLE)
        framebuffer_frame_draws[gl_state.framebuffer]++;
    if (sf_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr,
                "[bc/gl] draw #%lu elements mode=%#x count=%d type=%#x "
                "indices=%p program=%u fbo=%u vbo=%u ebo=%u\n",
                draw_calls, mode, count, type, indices, current_program,
                gl_state.framebuffer, bound_array_buffer,
                bound_element_array_buffer);
    trace_current_fbo_status();
    sf_etc1_draw_state etc1_draw;
    etc1_before_draw(&etc1_draw);
    real(mode, count, type, indices);
    etc1_after_draw(&etc1_draw);
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
                "[bc/gl] tex-compressed #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d bytes=%d data=%s\n",
                texture_images, tracked_active_texture_unit(),
                bound_texture_2d, target,
                level, internal_format, width, height, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexImage2D");
    if (!plain) plain = gl_raw("glTexImage2D");
    tex_half_init();
    int tracked = bound_texture_2d < SF_TEX_TABLE;
    int profile_half = tex_half_min > 0 &&
                       (width >= tex_half_min || height >= tex_half_min);
    int need_half = profile_half || tex_exceeds_hw(width, height);
    int continue_half = tracked && level > 0 &&
                        tex_is_halved[bound_texture_2d];
    int reduce = (level == 0 && need_half) || continue_half;
    int software_required = is_etc2(internal_format) &&
                            !native_etc2_support();
    /* On ES3, only textures selected by the profile cross the software path;
     * smaller ETC2 remains native. On Mali-450 every ETC2 upload still uses
     * the established decoder/ETC1 fallback. */
    if (data && is_etc2(internal_format) &&
        (software_required || reduce)) {
        unsigned char *rgba = etc2_decode_rgba(internal_format, width, height,
                                                data, image_size);
        /* O decode ETC2 ja produziu RGBA cru. Use exatamente o mesmo redutor
         * seguro do upload comum para low, alem do clamp fisico obrigatorio.
         * Mips seguem a textura marcada e nunca sao transformados offline. */
        GLsizei up_w = width;
        GLsizei up_h = height;
        if (rgba && level == 0 && tracked)
            tex_is_halved[bound_texture_2d] = 0;
        if (rgba && reduce) {
            GLsizei nw = 0, nh = 0;
            int indexed = level == 0 &&
                etc1_is_palette_indexed_rgba(
                    rgba, width, height, (size_t)width * 4u, NULL);
            void *half = halve_image(rgba, width, height, 4, indexed,
                                     &nw, &nh);
            if (half) {
                free(rgba);
                rgba = half;
                up_w = nw;
                up_h = nh;
                if (level == 0 && tracked) {
                    size_t saved = ((size_t)width * height -
                                    (size_t)up_w * up_h) * 4u;
                    tex_is_halved[bound_texture_2d] = 1;
                    tex_halved++;
                    tex_halved_saved_kb += saved / 1024;
                    fprintf(stderr,
                            "[sf/tex] %s texture=%u %dx%d -> %dx%d "
                            "(-%zuKB fonte, total -%luMB)\n",
                            profile_half ? "perfil-low" : "clamp-hw",
                            bound_texture_2d, width, height, up_w, up_h,
                            saved / 1024, tex_halved_saved_kb / 1024);
                }
                if (level == 0 && tex_exceeds_hw(up_w, up_h))
                    fprintf(stderr,
                            "[sf/tex] AVISO: %dx%d ainda excede o limite "
                            "apos uma meia-res\n", up_w, up_h);
            }
        }
        if (rgba && etc1_upload_rgba(target, level, up_w, up_h, border,
                                     rgba, (size_t)up_w * 4u)) {
            free(rgba);
            return;
        }
        if (bound_texture_2d < SF_TEX_TABLE) {
            if (level == 0)
                etc1_reset_texture(bound_texture_2d, 1);
            else if (texture_kinds[bound_texture_2d] == SF_TEX_ETC1_OPAQUE ||
                     texture_kinds[bound_texture_2d] == SF_TEX_ETC1_DUAL)
                (void)etc1_demote_texture(bound_texture_2d,
                                          "mip ETC2 incompativel");
        }
        if (rgba && plain) {
            plain(target, level, GL_RGBA, up_w, up_h, border, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
    }
    if (level == 0 && bound_texture_2d < SF_TEX_TABLE)
        etc1_reset_texture(bound_texture_2d, 1);
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
                "[bc/gl] tex-compressed-sub #%lu unit=%#x texture=%u "
                "target=%#x level=%d xy=%d,%d size=%dx%d format=%#x "
                "bytes=%d data=%s\n",
                texture_sub_images, tracked_active_texture_unit(),
                bound_texture_2d,
                target, level, x, y, width, height, format, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexSubImage2D");
    if (!plain) plain = gl_raw("glTexSubImage2D");
    if (data && is_etc2(format) && !native_etc2_support()) {
        unsigned char *rgba = etc2_decode_rgba(format, width, height, data,
                                                image_size);
        if (bound_texture_2d < SF_TEX_TABLE &&
            (texture_kinds[bound_texture_2d] == SF_TEX_ETC1_OPAQUE ||
             texture_kinds[bound_texture_2d] == SF_TEX_ETC1_DUAL))
            (void)etc1_demote_texture(bound_texture_2d,
                                      "CompressedTexSubImage dinamica");
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


void *sf_gl_sym(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "glBindFramebuffer") == 0 ||
        strcmp(name, "glBindFramebufferOES") == 0)
        return my_glBindFramebuffer;
    if (strcmp(name, "glDeleteFramebuffers") == 0 ||
        strcmp(name, "glDeleteFramebuffersOES") == 0)
        return my_glDeleteFramebuffers;
    if (strcmp(name, "glColorMask") == 0)
        return my_glColorMask;
    if (strcmp(name, "glClearColor") == 0)
        return my_glClearColor;
    if (strcmp(name, "glEnable") == 0)
        return my_glEnable;
    if (strcmp(name, "glDisable") == 0)
        return my_glDisable;
    if (strcmp(name, "glCreateShader") == 0)
        return my_glCreateShader;
    if (strcmp(name, "glShaderSource") == 0)
        return my_glShaderSource;
    if (strcmp(name, "glCompileShader") == 0)
        return my_glCompileShader;
    if (strcmp(name, "glCreateProgram") == 0)
        return my_glCreateProgram;
    if (strcmp(name, "glLinkProgram") == 0)
        return my_glLinkProgram;
    if (strcmp(name, "glProgramBinary") == 0)
        return my_glProgramBinary;
    if (strcmp(name, "glProgramBinaryOES") == 0)
        return my_glProgramBinaryOES;
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
    if (strcmp(name, "glUniform1iv") == 0)
        return my_glUniform1iv;
    if (strcmp(name, "glUniform1f") == 0)
        return my_glUniform1f;
    if (strcmp(name, "glUniform1fv") == 0)
        return my_glUniform1fv;
    if (strcmp(name, "glUniform4f") == 0)
        return my_glUniform4f;
    if (strcmp(name, "glUniform4fv") == 0)
        return my_glUniform4fv;
    if (strcmp(name, "glActiveTexture") == 0)
        return my_glActiveTexture;
    if (strcmp(name, "glBindTexture") == 0)
        return my_glBindTexture;
    if (strcmp(name, "glDeleteTextures") == 0)
        return my_glDeleteTextures;
    if (strcmp(name, "glGenerateMipmap") == 0 ||
        strcmp(name, "glGenerateMipmapOES") == 0)
        return my_glGenerateMipmap;
    if (strcmp(name, "glPixelStorei") == 0)
        return my_glPixelStorei;
    if (strcmp(name, "glTexParameteri") == 0)
        return my_glTexParameteri;
    if (strcmp(name, "glTexParameterf") == 0)
        return my_glTexParameterf;
    if (strcmp(name, "glTexParameteriv") == 0)
        return my_glTexParameteriv;
    if (strcmp(name, "glTexParameterfv") == 0)
        return my_glTexParameterfv;
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
    if (strcmp(name, "glEGLImageTargetTexture2DOES") == 0)
        return my_glEGLImageTargetTexture2DOES;
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
    int patched = 0;
    int saw_stencil = 0;
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 58; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_RENDERABLE_TYPE && (val & EGL_OPENGL_ES3_BIT_KHR)) {
                val = (val & ~EGL_OPENGL_ES3_BIT_KHR) | EGL_OPENGL_ES2_BIT;
                patched = 1;
            }
            if (name == EGL_STENCIL_SIZE) {
                saw_stencil = 1;
                if (val < 8) { val = 8; patched = 1; }
            }
            nx_log("eglChooseConfig: pedido 0x%04x = %d", (unsigned)name, (int)val);
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    /* A UI da Unity desenha painel/mascara com STENCIL (o shader de texto tem
     * StencilComp/StencilID).  Sem bits de stencil no config, todo elemento
     * mascarado some — foi o que deixou o menu de pause invisivel neste port
     * (NextOS, 07/08/2026: "nao era pra ter menu no meio da tela?").
     * SF_NO_FORCE_STENCIL=1 desliga o forcamento. */
    if (!saw_stencil && k < 60 &&
        !(getenv("SF_NO_FORCE_STENCIL") &&
          strcmp(getenv("SF_NO_FORCE_STENCIL"), "0") != 0)) {
        copy[k++] = EGL_STENCIL_SIZE;
        copy[k++] = 8;
        patched = 1;
        nx_log("eglChooseConfig: STENCIL 8 forcado (a UI da Unity precisa)");
    }
    copy[k] = EGL_NONE;
    if (patched)
        nx_log("eglChooseConfig: atributos ajustados");
    if (!p_eglChooseConfig)
        p_eglChooseConfig = sys("eglChooseConfig");
    EGLBoolean ok = p_eglChooseConfig(dpy, attrib ? copy : NULL, cfgs, n, num);
    /* prova por medida: quantos bits de stencil o config escolhido tem */
    if (ok && num && *num > 0 && cfgs) {
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
    return ok;
}

static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);

static EGLContext my_eglCreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const EGLint *attrib)
{
    EGLint copy[32];
    size_t k = 0;
    EGLint requested_version = 1;
    int downgraded = 0;
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 28; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_CONTEXT_CLIENT_VERSION) {
                requested_version = val;
                if (val > 2) {
                    val = 2;
                    downgraded = 1;
                }
            }
            if (name == EGL_CONTEXT_MINOR_VERSION_KHR)
                continue;                       /* ES2 has no minor version */
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    copy[k] = EGL_NONE;
    if (!p_eglCreateContext)
        p_eglCreateContext = sys("eglCreateContext");
    EGLContext context = p_eglCreateContext(dpy, cfg, share,
                                             attrib ? copy : NULL);
    nx_log("eglCreateContext: Unity pediu ES%d, driver recebeu ES%d, "
           "resultado=%p%s", (int)requested_version,
           downgraded ? 2 : (int)requested_version, (void *)context,
           downgraded ? " (proteção de downgrade usada)" : "");
    return context;
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
    extern void *sf_native_window(void);
    if (!p_eglCreateWindowSurface)
        p_eglCreateWindowSurface = sys("eglCreateWindowSurface");
    EGLNativeWindowType real = (EGLNativeWindowType)sf_native_window();
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

static EGLClientBuffer my_eglGetNativeClientBufferANDROID(
    const void *hardware_buffer)
{
    void *client = sf_media_egl_get_native_client_buffer(
        (void *)hardware_buffer);
    if (client)
        return (EGLClientBuffer)client;

    static EGLClientBuffer (*real)(const void *);
    if (!real) {
        static void *(*get_proc)(const char *);
        if (!get_proc)
            get_proc = sys("eglGetProcAddress");
        if (get_proc)
            real = get_proc("eglGetNativeClientBufferANDROID");
    }
    return real ? real(hardware_buffer) : NULL;
}

static EGLImageKHR my_eglCreateImageKHR(EGLDisplay display,
                                        EGLContext context,
                                        EGLenum target,
                                        EGLClientBuffer client_buffer,
                                        const EGLint *attributes)
{
    void *image = sf_media_egl_create_image(target, client_buffer);
    if (image)
        return (EGLImageKHR)image;

    static EGLImageKHR (*real)(EGLDisplay, EGLContext, EGLenum,
                               EGLClientBuffer, const EGLint *);
    if (!real)
        real = sf_sdl_video_active()
                   ? sf_sdl_egl_proc("eglCreateImageKHR")
                   : sys("eglCreateImageKHR");
    return real ? real(display, context, target, client_buffer, attributes)
                : EGL_NO_IMAGE_KHR;
}

static EGLBoolean my_eglDestroyImageKHR(EGLDisplay display,
                                        EGLImageKHR image)
{
    if (sf_media_egl_destroy_image(image))
        return EGL_TRUE;

    static EGLBoolean (*real)(EGLDisplay, EGLImageKHR);
    if (!real)
        real = sf_sdl_video_active()
                   ? sf_sdl_egl_proc("eglDestroyImageKHR")
                   : sys("eglDestroyImageKHR");
    return real ? real(display, image) : EGL_FALSE;
}

static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);

/* One-shot diagnostic for the raw Mali/fbdev path.  The SDL-owned backend has
 * the equivalent hook in egl_sdl.c; keeping this immediately before the real
 * swap captures exactly what Unity is about to present. */
extern void *sf_native_window(void);

static int capture_bound_frame(const char *path, GLuint framebuffer,
                               unsigned int frame_draws, unsigned long frame,
                               int width, int height, GLenum status)
{
    if (!path || !*path || width <= 0 || height <= 0 ||
        width > 4096 || height > 4096)
        return 0;
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / 4u || pixels > 16u * 1024u * 1024u)
        return 0;

    void (*get_int)(GLenum, GLint *) = gl_raw("glGetIntegerv");
    void (*pixel_store)(GLenum, GLint) = gl_raw("glPixelStorei");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) = gl_raw("glReadPixels");
    if (!get_int || !pixel_store || !read_pixels)
        return 0;

    unsigned char *rgba = malloc(pixels * 4u);
    unsigned char *rgb = malloc(pixels * 3u);
    if (!rgba || !rgb) {
        free(rgb);
        free(rgba);
        return 0;
    }

    GLint old_pack = 4;
    get_int(GL_PACK_ALIGNMENT, &old_pack);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    pixel_store(GL_PACK_ALIGNMENT, old_pack);

    size_t nonblack = 0;
    size_t alpha_zero = 0;
    for (int y = 0; y < height; y++) {
        const unsigned char *source =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4u;
        unsigned char *dest = rgb + (size_t)y * (size_t)width * 3u;
        for (int x = 0; x < width; x++) {
            unsigned char red = source[x * 4 + 0];
            unsigned char green = source[x * 4 + 1];
            unsigned char blue = source[x * 4 + 2];
            unsigned char alpha = source[x * 4 + 3];
            dest[x * 3 + 0] = red;
            dest[x * 3 + 1] = green;
            dest[x * 3 + 2] = blue;
            if ((unsigned)red + green + blue > 24)
                nonblack++;
            if (alpha == 0)
                alpha_zero++;
        }
    }

    int complete = 0;
    FILE *output = fopen(path, "wb");
    if (output) {
        fprintf(output, "P6\n%d %d\n255\n", width, height);
        size_t written = fwrite(rgb, 3, pixels, output);
        complete = fclose(output) == 0 && written == pixels;
    }
    fprintf(stderr,
            "[sf/capture] frame=%lu fbo=%u draws=%u status=%#x "
            "size=%dx%d nonblack=%zu/%zu alpha-zero=%zu file=%s %s\n",
            frame, framebuffer, frame_draws, status, width, height,
            nonblack, pixels, alpha_zero, path, complete ? "OK" : "FALHOU");
    free(rgb);
    free(rgba);
    return complete;
}

static void capture_raw_frame_if_requested(void)
{
    static int finished;
    unsigned long frame = ++sf_swap_count;

    static const char *path;
    static int path_known;
    static const char *trigger_path;
    if (!path_known) {
        path = getenv("SF_SCREENSHOT");
        trigger_path = getenv("SF_SCREENSHOT_TRIGGER");
        if (!trigger_path || !*trigger_path)
            trigger_path = "/tmp/bcshot";
        path_known = 1;
    }
    if (!path || !*path)
        return;

    int live_mode = getenv("SF_SCREENSHOT_LIVE") &&
                    strcmp(getenv("SF_SCREENSHOT_LIVE"), "0") != 0;
    int live_request = live_mode && access(trigger_path, F_OK) == 0;
    if (live_mode) {
        if (!live_request)
            return;
        unlink(trigger_path);
    } else if (finished) {
        return;
    }

    if (!live_mode) {
        long wanted = 300;
        const char *value = getenv("SF_SCREENSHOT_FRAME");
        if (value && *value) {
            char *end = NULL;
            long parsed = strtol(value, &end, 10);
            if (end && *end == '\0' && parsed > 0)
                wanted = parsed;
        }
        if (frame < (unsigned long)wanted)
            return;
        finished = 1;
    }

    void (*get_int)(GLenum, GLint *) = gl_raw("glGetIntegerv");
    void (*bind_fb)(GLenum, GLuint) = gl_raw("glBindFramebuffer");
    GLenum (*check_fb)(GLenum) = gl_raw("glCheckFramebufferStatus");
    if (!get_int || !bind_fb) {
        fprintf(stderr, "[sf/capture] funcoes GLES obrigatorias ausentes\n");
        return;
    }

    GLint prev_fb = 0;
    get_int(GL_FRAMEBUFFER_BINDING, &prev_fb);

    /* Capture every offscreen target that actually received draws in this
     * frame.  This makes the exact loss boundary visible: scene FBO versus
     * final framebuffer 0. */
    for (GLuint fbo = 1; fbo < SF_FBO_TABLE; fbo++) {
        if (!framebuffer_frame_draws[fbo])
            continue;
        GLuint texture = framebuffer_color_textures[fbo];
        if (!texture || texture >= SF_TEX_TABLE)
            continue;
        int width = texture_widths[texture];
        int height = texture_heights[texture];
        char fbo_path[1400];
        int length = snprintf(fbo_path, sizeof fbo_path,
                              "%s.fbo%u.ppm", path, fbo);
        if (length < 0 || (size_t)length >= sizeof fbo_path)
            continue;
        bind_fb(GL_FRAMEBUFFER, fbo);
        GLenum status = check_fb ? check_fb(GL_FRAMEBUFFER) : 0;
        (void)capture_bound_frame(fbo_path, fbo,
                                  framebuffer_frame_draws[fbo], frame,
                                  width, height, status);
    }

    bind_fb(GL_FRAMEBUFFER, 0);
    unsigned short *window = (unsigned short *)sf_native_window();
    int width = window ? window[0] : 0;
    int height = window ? window[1] : 0;
    if (width <= 0 || height <= 0) {
        GLint viewport[4] = { 0, 0, 0, 0 };
        get_int(GL_VIEWPORT, viewport);
        width = viewport[2];
        height = viewport[3];
    }
    GLenum status = check_fb ? check_fb(GL_FRAMEBUFFER) : 0;
    (void)capture_bound_frame(path, 0, framebuffer_frame_draws[0], frame,
                              width, height, status);
    bind_fb(GL_FRAMEBUFFER, (GLuint)prev_fb);
}

static void finish_frame_diagnostics(void)
{
    static int frame_log = -1;
    if (frame_log < 0)
        frame_log = sf_env_flag("SF_FRAMELOG");
    if (frame_log && (sf_swap_count <= 10 || sf_swap_count % 120 == 0)) {
        fprintf(stderr, "[sf/frame] swap=%lu current-fbo=%u draws",
                sf_swap_count, gl_state.framebuffer);
        for (GLuint fbo = 0; fbo < SF_FBO_TABLE; fbo++)
            if (framebuffer_frame_draws[fbo])
                fprintf(stderr, " fbo%u=%u", fbo,
                        framebuffer_frame_draws[fbo]);
        fputc('\n', stderr);
    }
    memset(framebuffer_frame_draws, 0, sizeof framebuffer_frame_draws);
}

static void frame_proof_before_present(int width, int height)
{
    static int context_registered;
    if (!context_registered) {
        const GLubyte *(*get_string)(GLenum) = gl_raw("glGetString");
        const char *driver = sf_sdl_video_active()
                                 ? SDL_GetCurrentVideoDriver() : "raw-egl";
        const char *renderer = get_string
                                   ? (const char *)get_string(GL_RENDERER) : NULL;
        const char *version = get_string
                                  ? (const char *)get_string(GL_VERSION) : NULL;
        nxgl_frame_proof_set_resolver(gl_raw);
        nxgl_frame_proof_set_video_context(
            width, height, driver ? driver : "?", renderer, version);
        context_registered = 1;
    }
    nxgl_frame_proof_before_present(width, height);
}

static EGLBoolean my_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    /* SurfaceTexture.updateTexImage normally runs from Unity's render thread.
     * Its callback bridge is preserved, and this consumes any still-pending
     * frame on that same render thread before the next Unity draw. */
    sf_media_update_textures();
    {
        int width = 0, height = 0;
        sf_sdl_screen_size(&width, &height);
        sf_present_fill_set_resolver(gl_raw);
        (void)sf_present_fill_apply(width, height, getenv("SF_ASPECT"));
    }
    draw_gamepad_keyboard();
    draw_gamepad_cursor();
    if (sf_sdl_video_active()) {
        int width = 0, height = 0;
        sf_sdl_screen_size(&width, &height);
        frame_proof_before_present(width, height);
        return sf_sdl_swap_buffers(display, surface);
    }
    capture_raw_frame_if_requested();
    finish_frame_diagnostics();
    force_opaque_backbuffer();
    {
        int width = 0, height = 0;
        sf_sdl_screen_size(&width, &height);
        frame_proof_before_present(width, height);
    }
    if (!p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");
    return p_eglSwapBuffers(display, surface);
}

static void *my_eglGetProcAddress(const char *name)
{
    if (!name)
        return NULL;
    if (name[0] == 'g' && name[1] == 'l')
        return sf_gl_sym(name);
    if (strcmp(name, "eglGetNativeClientBufferANDROID") == 0)
        return my_eglGetNativeClientBufferANDROID;
    if (strcmp(name, "eglCreateImageKHR") == 0)
        return my_eglCreateImageKHR;
    if (strcmp(name, "eglDestroyImageKHR") == 0)
        return my_eglDestroyImageKHR;
    if (sf_sdl_video_active())
        return sf_sdl_egl_proc(name);
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
    "eglBindAPI", "eglQueryAPI", "eglDestroyContext", "eglMakeCurrent",
    "eglGetCurrentContext", "eglGetCurrentSurface", "eglGetCurrentDisplay",
    "eglQueryContext", "eglGetError",
    "eglQueryString", "eglSurfaceAttrib", "eglSwapInterval",
    "eglReleaseThread", "eglWaitClient", "eglWaitGL", "eglWaitNative",
    "eglCreateSyncKHR",
    "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglGetSyncAttribKHR",
    "eglPresentationTimeANDROID", "eglDupNativeFenceFDANDROID",
};

#define MAXTAB (sizeof passthrough / sizeof *passthrough + 12)
static nx_import tab[MAXTAB];
static size_t tab_n;

void sf_egl_init(void)
{
    /* SDL owns KMS/Wayland contexts; Mali fbdev stays on the vendor EGL path,
     * matching the validated Horizon Chase backend split. */
    int sdl_owned = sf_capture_mode ? 0 : sf_sdl_video_init();
    nxgl_frame_proof_set_resolver(gl_raw);
    nxgl_frame_proof_launch_receipt();
    tab_n = 0;
    tab[tab_n++] = (nx_import){ "eglChooseConfig",
        sdl_owned ? sf_sdl_egl_proc("eglChooseConfig")
                  : (void *)my_eglChooseConfig };
    tab[tab_n++] = (nx_import){ "eglCreateContext",
        sdl_owned ? sf_sdl_egl_proc("eglCreateContext")
                  : (void *)my_eglCreateContext };
    tab[tab_n++] = (nx_import){ "eglCreateWindowSurface",
        sdl_owned ? sf_sdl_egl_proc("eglCreateWindowSurface")
                  : (void *)my_eglCreateWindowSurface };
    tab[tab_n++] = (nx_import){ "eglGetDisplay",
        sdl_owned ? sf_sdl_egl_proc("eglGetDisplay")
                  : (void *)my_eglGetDisplay };
    tab[tab_n++] = (nx_import){ "eglSwapBuffers",        (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",     (void *)my_eglGetProcAddress };
    tab[tab_n++] = (nx_import){ "eglGetNativeClientBufferANDROID",
                                (void *)my_eglGetNativeClientBufferANDROID };
    tab[tab_n++] = (nx_import){ "eglCreateImageKHR",
                                (void *)my_eglCreateImageKHR };
    tab[tab_n++] = (nx_import){ "eglDestroyImageKHR",
                                (void *)my_eglDestroyImageKHR };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        void *f = sdl_owned ? sf_sdl_egl_proc(passthrough[i])
                            : dlsym(RTLD_DEFAULT, passthrough[i]);
        if (!f && !sdl_owned)
            f = sys(passthrough[i]);
        if (f)
            tab[tab_n++] = (nx_import){ passthrough[i], f };
    }
    nx_log("egl table: %zu entries (%s ownership)", tab_n,
           sdl_owned ? "SDL" : "raw");
}

const nx_import *sf_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *sf_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return sf_gl_sym(name);
}
