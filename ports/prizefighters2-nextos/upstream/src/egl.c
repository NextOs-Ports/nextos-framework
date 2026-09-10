/*
 * egl.c -- EGL for a GLES3-only Unity build on a GLES2-only GPU.
 *
 * The APK asks for GLES 3.0 (manifest glEsVersion=0x30000, m_GraphicsAPIs=[11])
 * and the Mali-450 stops at GLES 2.0, so the negotiation has to happen here
 * rather than by patching the player:
 *
 *   - eglChooseConfig strips EGL_OPENGL_ES3_BIT_KHR from the renderable-type
 *     mask so the driver actually returns configs,
 *   - eglCreateContext rewrites EGL_CONTEXT_CLIENT_VERSION 3 to 2 and drops the
 *     ES3-only context attributes,
 *   - eglQueryString(EGL_VERSION) is left alone: Unity reads the *GL* version
 *     string through glGetString, which the shader layer answers.
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
#include <SDL2/SDL.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_elf.h"
#include "pf2.h"
#include "egl_sdl.h"

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
    if (pf2_sdl_video_active())
        return pf2_sdl_gl_proc(name);
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

/* Controller keyboard modelled after the proven FF4 naming keyboard.  PF2 is
 * GLES2, so this version uses only scissored clears: no shader, texture or
 * vertex state from Unity is replaced. */
static void draw_gamepad_keyboard(void)
{
    char typed[64];
    int uppercase = 0;
    int selected = 0;
    const pf2_keyboard_key *keys = NULL;
    size_t key_count = 0;
    if (!pf2_input_keyboard_snapshot(typed, sizeof typed, &uppercase,
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

    /* Near-black frame, burgundy cabinet and cream highlights match PF2. */
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
        const pf2_keyboard_key *key = &keys[i];
        char dynamic_label[8];
        const char *label = key->label;
        if (key->action == PF2_KEY_CHARACTER) {
            dynamic_label[0] = uppercase ? key->upper : key->lower;
            dynamic_label[1] = '\0';
            label = dynamic_label;
        } else if (key->action == PF2_KEY_SHIFT) {
            snprintf(dynamic_label, sizeof dynamic_label, "%s",
                     uppercase ? "UPPER" : "LOWER");
            label = dynamic_label;
        }
        int highlighted = (int)i == selected ||
            (key->action == PF2_KEY_SHIFT && uppercase);
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

/* A small Prizefighters-style pixel cursor: cream face, near-black outline and
 * wine-red drop shadow.  It is drawn with scissored color clears immediately
 * before swap, avoiding shaders/textures and preserving Unity's GL program. */
static void draw_gamepad_cursor(void)
{
    float cursor_x, cursor_y;
    if (!pf2_input_cursor(&cursor_x, &cursor_y))
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
    pf2_input_set_screen_size(width, height);
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

static GLuint my_glCreateShader(GLenum type)
{
    static GLuint (*real)(GLenum);
    if (!real)
        real = gl_raw("glCreateShader");
    GLuint shader = real(type);
    remember_shader(shader, type);
    if (pf2_trace_gl)
        fprintf(stderr, "[pf2/gl] create %s shader=%u\n",
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
    if (pf2_trace_gl) {
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
                "[pf2/gl] source #%lu shader=%u stage=%s bytes=%zu: %.160s\n",
                shader_sources, shader, shader_stage(shader), total, preview);
        if (getenv("PF2_GLSOURCE")) {
            fprintf(stderr, "[pf2/gl/source-begin] shader=%u stage=%s\n",
                    shader, shader_stage(shader));
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                fwrite(strings[i], 1, n, stderr);
            }
            fprintf(stderr, "\n[pf2/gl/source-end] shader=%u\n", shader);
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
    if (pf2_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(shader, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr,
                "[pf2/gl] compile #%lu shader=%u stage=%s ok=%d log=%s\n",
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
    if (pf2_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(program, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr, "[pf2/gl] link #%lu program=%u ok=%d log=%s\n",
                program_links, program, ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real)
        real = gl_raw("glDrawArrays");
    draw_calls++;
    if (pf2_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr, "[pf2/gl] draw #%lu arrays mode=%#x count=%d\n",
                draw_calls, mode, count);
    real(mode, first, count);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices)
{
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real)
        real = gl_raw("glDrawElements");
    draw_calls++;
    if (pf2_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr, "[pf2/gl] draw #%lu elements mode=%#x count=%d\n",
                draw_calls, mode, count);
    real(mode, count, type, indices);
}

void *pf2_gl_sym(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "glCreateShader") == 0)
        return my_glCreateShader;
    if (strcmp(name, "glShaderSource") == 0)
        return my_glShaderSource;
    if (strcmp(name, "glCompileShader") == 0)
        return my_glCompileShader;
    if (strcmp(name, "glLinkProgram") == 0)
        return my_glLinkProgram;
    if (strcmp(name, "glDrawArrays") == 0)
        return my_glDrawArrays;
    if (strcmp(name, "glDrawElements") == 0)
        return my_glDrawElements;
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
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 60; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_RENDERABLE_TYPE && (val & EGL_OPENGL_ES3_BIT_KHR)) {
                val = (val & ~EGL_OPENGL_ES3_BIT_KHR) | EGL_OPENGL_ES2_BIT;
                patched = 1;
            }
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    copy[k] = EGL_NONE;
    if (patched)
        nx_log("eglChooseConfig: dropped EGL_OPENGL_ES3_BIT");
    if (!p_eglChooseConfig)
        p_eglChooseConfig = sys("eglChooseConfig");
    return p_eglChooseConfig(dpy, attrib ? copy : NULL, cfgs, n, num);
}

static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);

static EGLContext my_eglCreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const EGLint *attrib)
{
    EGLint copy[32];
    size_t k = 0;
    int downgraded = 0;
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 28; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_CONTEXT_CLIENT_VERSION && val > 2) {
                val = 2;
                downgraded = 1;
            }
            if (name == EGL_CONTEXT_MINOR_VERSION_KHR)
                continue;                       /* ES2 has no minor version */
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    copy[k] = EGL_NONE;
    if (downgraded)
        nx_log("eglCreateContext: ES3 context requested, created ES2");
    if (!p_eglCreateContext)
        p_eglCreateContext = sys("eglCreateContext");
    return p_eglCreateContext(dpy, cfg, share, attrib ? copy : NULL);
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
    extern void *pf2_native_window(void);
    if (!p_eglCreateWindowSurface)
        p_eglCreateWindowSurface = sys("eglCreateWindowSurface");
    EGLNativeWindowType real = (EGLNativeWindowType)pf2_native_window();
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

static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);

static EGLBoolean my_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    draw_gamepad_keyboard();
    draw_gamepad_cursor();
    if (pf2_sdl_video_active())
        return pf2_sdl_swap_buffers(display, surface);
    if (!p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");
    return p_eglSwapBuffers(display, surface);
}

static void *my_eglGetProcAddress(const char *name)
{
    if (!name)
        return NULL;
    if (name[0] == 'g' && name[1] == 'l')
        return pf2_gl_sym(name);
    if (pf2_sdl_video_active())
        return pf2_sdl_egl_proc(name);
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
    "eglCreateImageKHR", "eglDestroyImageKHR", "eglCreateSyncKHR",
    "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglGetSyncAttribKHR",
    "eglPresentationTimeANDROID", "eglDupNativeFenceFDANDROID",
};

#define MAXTAB (sizeof passthrough / sizeof *passthrough + 8)
static nx_import tab[MAXTAB];
static size_t tab_n;

void pf2_egl_init(void)
{
    /* The one-shot owner-data capture happens before Unity's graphics
     * lifecycle and must not acquire DRM/KMS or create an SDL context.  It
     * still needs the ordinary EGL import table because the protected DSOs
     * carry those relocations.  Normal gameplay keeps the proven runtime
     * backend selection unchanged. */
    int sdl_owned = pf2_capture_mode ? 0 : pf2_sdl_video_init();
    tab_n = 0;
    tab[tab_n++] = (nx_import){ "eglChooseConfig",
        sdl_owned ? pf2_sdl_egl_proc("eglChooseConfig")
                  : (void *)my_eglChooseConfig };
    tab[tab_n++] = (nx_import){ "eglCreateContext",
        sdl_owned ? pf2_sdl_egl_proc("eglCreateContext")
                  : (void *)my_eglCreateContext };
    tab[tab_n++] = (nx_import){ "eglCreateWindowSurface",
        sdl_owned ? pf2_sdl_egl_proc("eglCreateWindowSurface")
                  : (void *)my_eglCreateWindowSurface };
    tab[tab_n++] = (nx_import){ "eglGetDisplay",
        sdl_owned ? pf2_sdl_egl_proc("eglGetDisplay")
                  : (void *)my_eglGetDisplay };
    tab[tab_n++] = (nx_import){ "eglSwapBuffers",        (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",     (void *)my_eglGetProcAddress };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        void *f = sdl_owned ? pf2_sdl_egl_proc(passthrough[i])
                            : dlsym(RTLD_DEFAULT, passthrough[i]);
        if (!f && !sdl_owned)
            f = sys(passthrough[i]);
        if (f)
            tab[tab_n++] = (nx_import){ passthrough[i], f };
    }
    nx_log("egl table: %zu entries (%s ownership)", tab_n,
           sdl_owned ? "SDL" : "raw");
}

const nx_import *pf2_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *pf2_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return pf2_gl_sym(name);
}
