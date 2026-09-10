/*
 * egl.c -- EGL bridge for a Unity build that contains GLES2 and GLES3 paths.
 *
 * Hitman GO advertises GLES2 and carries m_GraphicsAPIs=[8,11].  The Mali-450
 * stops at GLES2, so any optional GLES3 request is negotiated here rather than
 * by patching the original player or data:
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
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <SDL2/SDL.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_elf.h"
#include "gb.h"
#include "egl_sdl.h"
#include "etc2_decode.h"
#include "astc_decode.h"
#include "texture_convert.h"
#include "unity6_shader.h"
#include "nxgl_frame_proof_adapter.h"

#define GL_TEXTURE_2D_ARRAY 0x8C1A

/* GLES2 exposes one framebuffer binding.  Unity 6 uses the split GLES3
 * read/draw bindings even though this bridge creates an ES2 context. */
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif

extern GLenum fp2_gles3_texture_target(GLenum target);
extern GLenum fp2_gles3_texture_format(GLenum format);

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

/* The raw driver entry point, with no port-level override.  gles3.c builds its
 * ES3 emulation out of these. */
static void *gl_raw(const char *name);

void *fp2_gl_raw_sym(const char *name) { return gl_raw(name); }

static void *gl_raw(const char *name)
{
    if (!name || name[0] != 'g' || name[1] != 'l')
        return NULL;
    if (fp2_sdl_video_active())
        return fp2_sdl_gl_proc(name);
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

typedef struct {
    GLuint shader;
    GLenum type;
    char *driver_source;
    size_t driver_source_length;
    fp2_shader_attribs attribs;
} fp2_shader_record;

typedef struct {
    GLuint program;
    GLuint attached_shaders[16];
    size_t attached_count;
} fp2_program_record;

static fp2_program_record program_records[1024];
static size_t program_record_count;
static pthread_mutex_t program_record_lock = PTHREAD_MUTEX_INITIALIZER;

/* GL object names are not bounded by the number of simultaneously-live
 * shaders.  The old direct shader_types[256] index silently stopped
 * translating after handle 255, which a complete game session can exceed. */
static fp2_shader_record shader_records[2048];
static size_t shader_record_count;
static pthread_mutex_t shader_record_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long shader_sources;
static unsigned long shader_translated;
static unsigned long shader_rejected;
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
static GLint unpack_alignment = 4;
static GLint unpack_row_length = 0;
static GLint unpack_skip_rows = 0;
static GLint unpack_skip_pixels = 0;
static GLint unpack_image_height = 0;
static GLint unpack_skip_images = 0;
static GLuint bound_array_buffer;
static GLuint bound_element_array_buffer;
static GLuint current_program;

typedef struct {
    GLuint program;
    GLint location;
    char name[64];
} fp2_uniform_name;

static fp2_uniform_name uniform_names[128];
static size_t uniform_name_count;

typedef struct {
    GLuint texture;
    GLsizei width;
    GLsizei height;
    GLsizei levels;
    GLenum internal_format;
    uint64_t compact_levels;
    int compact_mode;
    int attached;
} fp2_texture_record;

static fp2_texture_record texture_records[4096];
static size_t texture_record_count;

static fp2_texture_record *find_texture_record(GLuint texture, int create)
{
    if (!texture)
        return NULL;
    for (size_t i = 0; i < texture_record_count; i++)
        if (texture_records[i].texture == texture)
            return &texture_records[i];
    if (!create || texture_record_count >=
                   sizeof texture_records / sizeof *texture_records)
        return NULL;
    fp2_texture_record *record = &texture_records[texture_record_count++];
    memset(record, 0, sizeof *record);
    record->texture = texture;
    return record;
}

static void forget_texture_record(GLuint texture)
{
    for (size_t i = 0; i < texture_record_count; i++) {
        if (texture_records[i].texture != texture)
            continue;
        texture_records[i] = texture_records[texture_record_count - 1];
        memset(&texture_records[texture_record_count - 1], 0,
               sizeof texture_records[0]);
        texture_record_count--;
        return;
    }
}

void fp2_gles3_texture_storage_2d(GLenum target, GLsizei levels,
                                 GLenum internal, GLsizei width,
                                 GLsizei height)
{
    if (fp2_gles3_texture_target(target) != GL_TEXTURE_2D ||
        !bound_texture_2d || levels <= 0 || width <= 0 || height <= 0)
        return;
    fp2_texture_record *record = find_texture_record(bound_texture_2d, 1);
    if (!record)
        return;
    int attached = record->attached;
    memset(record, 0, sizeof *record);
    record->texture = bound_texture_2d;
    record->width = width;
    record->height = height;
    record->levels = levels;
    record->internal_format = internal;
    record->attached = attached;
}

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
    fp2_uniform_name *entry = &uniform_names[uniform_name_count++];
    entry->program = program;
    entry->location = location;
    snprintf(entry->name, sizeof entry->name, "%s", name);
}

static int trace_texture_call(unsigned long call, GLsizei width,
                              GLsizei height)
{
    return fp2_trace_gl && (call <= 160 || width >= 512 || height >= 512);
}

unsigned long fp2_swap_count;


/* Sondas de ambiente do caminho quente lidas UMA vez: getenv e' varredura
 * linear do environ e estas ficam DENTRO do quadro (glBindFramebuffer sozinho
 * roda dezenas de vezes por frame). */
static int fp2_env_flag(const char *name)
{
    const char *v = getenv(name);
    return v != NULL;
}

static int texture_16bit_enabled(void)
{
#ifdef FP2_RELEASE_BUILD
    return 0;
#else
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("FP2_TEXTURE_16BIT");
        enabled = value && *value && strcmp(value, "0") != 0;
    }
    return enabled;
#endif
}

static size_t texture_16bit_min_bytes(void)
{
#ifdef FP2_RELEASE_BUILD
    return 4u * 1024u * 1024u;
#else
    static size_t minimum;
    static int known;
    if (known)
        return minimum;
    minimum = 4u * 1024u * 1024u;
    const char *value = getenv("FP2_TEXTURE_16BIT_MIN_BYTES");
    if (value && *value) {
        char *end = NULL;
        unsigned long long parsed = strtoull(value, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= SIZE_MAX)
            minimum = (size_t)parsed;
    }
    known = 1;
    return minimum;
#endif
}

/* Print sob demanda: o input pede um shot (L3 com FP2_SHOT ligado) e o proximo
   frame grava.  Existe porque ler /dev/fb0 de fora devolve preto enquanto o
   Mali renderiza — a unica testemunha honesta e o glReadPixels de dentro. */
int fp2_shot_request;
#ifndef FP2_RELEASE_BUILD
static char fp2_shot_path[256];
#endif

static void capture_current_fbo_if_requested(void)
{
#ifdef FP2_RELEASE_BUILD
    return;
#else
    static int finished;
    static const char *path;
    static int path_known;
    static unsigned shot_seq;
    if (!path_known) {
        path = getenv("FP2_FBO_CAPTURE");
        path_known = 1;
    }
    int on_demand = 0;
#ifdef FP2_BENCH_PROBES
    if (fp2_shot_request) {
        fp2_shot_request = 0;
        /* Sem caminho cravado no binario de release: o padrao e' a pasta do
           proprio jogo, seja qual for onde ele foi instalado. */
        const char *dir = getenv("FP2_SHOT_DIR");
        if (!dir || !*dir) dir = fp2_gamedir;
        snprintf(fp2_shot_path, sizeof fp2_shot_path, "%.200s/shot_%03u.ppm",
                 dir, ++shot_seq);
        on_demand = 1;
    }
#endif
    if (on_demand) {
        path = fp2_shot_path;
        finished = 0;
    }
    if (finished || !path || !*path || draw_calls < 2)
        return;
    if (!on_demand) {
        const char *want = getenv("FP2_FBO_CAPTURE_FRAME");
        if (want && *want && fp2_swap_count < strtoul(want, NULL, 10))
            return;
    }
    finished = 1;
    if (on_demand)
        fprintf(stderr, "[fp2/shot] gravando %s\n", path);

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
#endif
}

static void trace_buffer_payload(const void *data, GLsizeiptr size)
{
#ifdef FP2_RELEASE_BUILD
    (void)data;
    (void)size;
#else
    uint32_t words[4] = { 0, 0, 0, 0 };
    if (!data || size <= 0) {
        fprintf(stderr, " data=null");
        return;
    }
    size_t take = (size_t)size < sizeof words ? (size_t)size : sizeof words;
    memcpy(words, data, take);
    fprintf(stderr, " data=%08x,%08x,%08x,%08x",
            words[0], words[1], words[2], words[3]);
#endif
}

static void dump_buffer_upload(GLenum target, GLuint buffer, GLintptr offset,
                               const void *data, GLsizeiptr size,
                               unsigned long upload)
{
#ifdef FP2_RELEASE_BUILD
    (void)target; (void)buffer; (void)offset; (void)data; (void)size;
    (void)upload;
#else
    const char *prefix = getenv("FP2_GL_DUMP_PREFIX");
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
#endif
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
    /* framebuffer is the logical GLES3 draw binding and therefore also the
     * one physically bound in the GLES2 driver. */
    GLuint framebuffer;
    GLuint read_framebuffer;
    GLboolean color_mask[4];
    GLfloat clear_color[4];
    GLboolean scissor;
} fp2_gl_state;

static fp2_gl_state gl_state = {
    .color_mask = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
};

typedef void (*fp2_bind_framebuffer_fn)(GLenum, GLuint);

static fp2_bind_framebuffer_fn raw_bind_framebuffer(void)
{
    static fp2_bind_framebuffer_fn real;
    if (!real) {
        real = gl_raw("glBindFramebuffer");
        if (!real)
            real = gl_raw("glBindFramebufferOES");
    }
    return real;
}

GLuint fp2_gles3_draw_framebuffer(void)
{
    return gl_state.framebuffer;
}

GLuint fp2_gles3_read_framebuffer(void)
{
    return gl_state.read_framebuffer;
}

/* Operations other than BindFramebuffer still accept the split GLES3 target.
 * Map DRAW to the sole ES2 binding.  A READ operation against a different FBO
 * temporarily binds it, then the caller restores the logical draw FBO. */
static GLenum framebuffer_operation_begin(GLenum target, int *restore_draw)
{
    *restore_draw = 0;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
        return GL_FRAMEBUFFER;
    if (target != GL_READ_FRAMEBUFFER)
        return target;

    if (gl_state.read_framebuffer != gl_state.framebuffer) {
        fp2_bind_framebuffer_fn bind = raw_bind_framebuffer();
        if (bind) {
            bind(GL_FRAMEBUFFER, gl_state.read_framebuffer);
            *restore_draw = 1;
        }
    }
    return GL_FRAMEBUFFER;
}

static void framebuffer_operation_end(int restore_draw)
{
    if (!restore_draw)
        return;
    fp2_bind_framebuffer_fn bind = raw_bind_framebuffer();
    if (bind)
        bind(GL_FRAMEBUFFER, gl_state.framebuffer);
}


/* ---------------------------------------------------------------------------
 * MEDICAO DE IMAGEM POR FBO.  Print nao e' prova (#37): um PPM pode fotografar
 * o FBO errado, e `glDiscardFramebufferEXT` apaga o conteudo antes que qualquer
 * captura chegue nele.  Aqui a amostra sai NO MOMENTO EM QUE O FBO E' DEIXADO
 * -- que no tiler do Mali-450 e' o unico instante em que o tile ainda existe --
 * e o que se registra e' MEDIA por canal, fracao de pixel nao-preto e numero de
 * cores distintas.  Uma tela preta da media ~0 e 1 cor; imagem de verdade nao.
 *
 * A leitura e' uma GRADE ESPARSA (no maximo 64x64 pontos de 1x1) e so' nos
 * quadros pedidos: `glReadPixels` de tela cheia por quadro TRAVA o Mali-450.
 * ------------------------------------------------------------------------- */
#define FP2_STATS_GRID 48

static unsigned long fp2_stats_from_swap;   /* 0 = desligado */
static unsigned long fp2_stats_until_swap;

static void fp2_stats_init(void)
{
    static int known;
    if (known)
        return;
    known = 1;
    const char *v = getenv("FP2_FBO_STATS");
    if (!v || !*v)
        return;
    fp2_stats_from_swap = strtoul(v, NULL, 10);
    if (!fp2_stats_from_swap)
        fp2_stats_from_swap = 1;
    const char *n = getenv("FP2_FBO_STATS_SPAN");
    unsigned long span = n && *n ? strtoul(n, NULL, 10) : 8;
    fp2_stats_until_swap = fp2_stats_from_swap + (span ? span : 1);
}

static void fp2_sample_current_fbo(const char *tag, GLuint fbo)
{
#ifdef FP2_RELEASE_BUILD
    (void)tag;
    (void)fbo;
#else
    fp2_stats_init();
    if (!fp2_stats_from_swap)
        return;
    if (fp2_swap_count < fp2_stats_from_swap ||
        fp2_swap_count >= fp2_stats_until_swap)
        return;

    void (*get_int)(GLenum, GLint *) = gl_raw("glGetIntegerv");
    void (*pixel_store)(GLenum, GLint) = gl_raw("glPixelStorei");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) = gl_raw("glReadPixels");
    if (!get_int || !pixel_store || !read_pixels)
        return;
    GLint viewport[4] = { 0, 0, 0, 0 };
    get_int(GL_VIEWPORT, viewport);
    int width = viewport[2], height = viewport[3];
    if (width <= 0 || height <= 0)
        return;

    unsigned long long sum[4] = { 0, 0, 0, 0 };
    unsigned long taken = 0, non_black = 0;
    /* Conjunto de cores por amostragem: 4096 baldes de RGB de 4 bits por canal
     * bastam para separar "uma cor so'" de "imagem". */
    static unsigned char seen[4096];
    memset(seen, 0, sizeof seen);
    unsigned distinct = 0;

    for (int gy = 0; gy < FP2_STATS_GRID; gy++) {
        for (int gx = 0; gx < FP2_STATS_GRID; gx++) {
            int x = (int)(((long)gx * width) / FP2_STATS_GRID);
            int y = (int)(((long)gy * height) / FP2_STATS_GRID);
            unsigned char px[4] = { 0, 0, 0, 0 };
            pixel_store(GL_PACK_ALIGNMENT, 1);
            read_pixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (int c = 0; c < 4; c++)
                sum[c] += px[c];
            if (px[0] || px[1] || px[2])
                non_black++;
            unsigned bucket = ((unsigned)(px[0] >> 4) << 8) |
                              ((unsigned)(px[1] >> 4) << 4) |
                              (unsigned)(px[2] >> 4);
            if (!seen[bucket]) {
                seen[bucket] = 1;
                distinct++;
            }
            taken++;
        }
    }
    if (!taken)
        return;
    fprintf(stderr,
            "[fp2/fbostat] swap=%lu %s fbo=%u %dx%d amostras=%lu "
            "media=(%.1f,%.1f,%.1f,%.1f) nao-preto=%.1f%% cores=%u\n",
            fp2_swap_count, tag, fbo, width, height, taken,
            (double)sum[0] / taken, (double)sum[1] / taken,
            (double)sum[2] / taken, (double)sum[3] / taken,
            100.0 * (double)non_black / (double)taken, distinct);
    fflush(stderr);
#endif
}

static void my_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
#ifndef FP2_RELEASE_BUILD
    static void (*finish)(void);
#endif
    fp2_bind_framebuffer_fn real = raw_bind_framebuffer();
#ifndef FP2_RELEASE_BUILD
    GLuint previous = gl_state.framebuffer;
    if (previous != 0 && framebuffer == 0 &&
        (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)) {
        /* AO SAIR do FBO: no tiler do Mali-450 e' o ultimo instante em que o
         * conteudo dele ainda existe. */
        fp2_sample_current_fbo("saindo-de", previous);
        capture_current_fbo_if_requested();
    }
    static int fbo_finish = -1;
    if (fbo_finish < 0)
        fbo_finish = fp2_env_flag("FP2_FBO_FINISH");
    if (fbo_finish && previous != 0 && framebuffer == 0 &&
        (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)) {
        if (!finish)
            finish = gl_raw("glFinish");
        if (finish) {
            finish();
            if (fp2_trace_gl)
                fprintf(stderr,
                        "[fp2/gl] forced finish before fbo %u -> 0\n",
                        previous);
        }
    }
#endif
    if (target == GL_FRAMEBUFFER) {
        if (real)
            real(GL_FRAMEBUFFER, framebuffer);
        gl_state.framebuffer = framebuffer;
        gl_state.read_framebuffer = framebuffer;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        if (real)
            real(GL_FRAMEBUFFER, framebuffer);
        gl_state.framebuffer = framebuffer;
    } else if (target == GL_READ_FRAMEBUFFER) {
        /* Keep the GLES2 driver's only binding equal to the logical draw FBO.
         * Read-side operations temporarily switch it when they actually run. */
        gl_state.read_framebuffer = framebuffer;
    } else if (real) {
        real(target, framebuffer);
    }
    framebuffer_calls++;
    if (fp2_trace_gl && framebuffer_calls <= 160)
        fprintf(stderr,
                "[fp2/gl] fbo-bind #%lu target=%#x framebuffer=%u "
                "logical-read=%u logical-draw=%u\n",
                framebuffer_calls, target, framebuffer,
                gl_state.read_framebuffer, gl_state.framebuffer);
}

static void my_glDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{
    static void (*real)(GLsizei, const GLuint *);
    if (!real) {
        real = gl_raw("glDeleteFramebuffers");
        if (!real)
            real = gl_raw("glDeleteFramebuffersOES");
    }
    if (real)
        real(count, framebuffers);
    if (count > 0 && framebuffers) {
        for (GLsizei i = 0; i < count; i++) {
            if (framebuffers[i] == gl_state.framebuffer)
                gl_state.framebuffer = 0;
            if (framebuffers[i] == gl_state.read_framebuffer)
                gl_state.read_framebuffer = 0;
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
#ifndef FP2_RELEASE_BUILD
    /* A cor de limpeza do framebuffer padrao interessa no bring-up da imagem,
     * mas o jogo a define TODO quadro: sem limite isto escrevia ~8 MB por hora
     * no log.txt do port.  Registra as primeiras e depois so' quando muda. */
    if (fp2_trace_gl || gl_state.framebuffer == 0) {
        static unsigned long clears;
        static GLfloat last[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
        int changed = red != last[0] || green != last[1] ||
                      blue != last[2] || alpha != last[3];
        if (fp2_trace_gl || changed || ++clears <= 8) {
            last[0] = red; last[1] = green; last[2] = blue; last[3] = alpha;
            fprintf(stderr,
                    "[fp2/gl] clear-color fbo=%u r=%.3f g=%.3f b=%.3f a=%.3f\n",
                    gl_state.framebuffer, red, green, blue, alpha);
        }
    }
#endif
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
#ifdef FP2_RELEASE_BUILD
    return;
#else
    static int enabled = -1;
    if (enabled < 0)
        enabled = fp2_env_flag("FP2_OPAQUE_BACKBUFFER");
    if (!enabled || fp2_env_flag("FP2_NO_OPAQUE_BACKBUFFER"))
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
            glstate_trace = fp2_env_flag("FP2_GLSTATE_TRACE");
        if (glstate_trace) {
            fprintf(stderr,
                    "[fp2/gl] state fbo=%u mask=%u%u%u%u "
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
#endif
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
    const fp2_keyboard_key *keys = NULL;
    size_t key_count = 0;
    if (!fp2_input_keyboard_snapshot(typed, sizeof typed, &uppercase,
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
        const fp2_keyboard_key *key = &keys[i];
        char dynamic_label[8];
        const char *label = key->label;
        if (key->action == FP2_KEY_CHARACTER) {
            dynamic_label[0] = uppercase ? key->upper : key->lower;
            dynamic_label[1] = '\0';
            label = dynamic_label;
        } else if (key->action == FP2_KEY_SHIFT) {
            snprintf(dynamic_label, sizeof dynamic_label, "%s",
                     uppercase ? "UPPER" : "LOWER");
            label = dynamic_label;
        }
        int highlighted = (int)i == selected ||
            (key->action == FP2_KEY_SHIFT && uppercase);
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
    if (!fp2_input_cursor(&cursor_x, &cursor_y))
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
    fp2_input_set_screen_size(width, height);
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
    pthread_mutex_lock(&shader_record_lock);
    for (size_t i = 0; i < shader_record_count; i++) {
        if (shader_records[i].shader == shader) {
            shader_records[i].type = type;
            pthread_mutex_unlock(&shader_record_lock);
            return;
        }
    }
    if (shader_record_count < sizeof shader_records / sizeof *shader_records) {
        fp2_shader_record *record = &shader_records[shader_record_count];
        memset(record, 0, sizeof *record);
        record->shader = shader;
        record->type = type;
        shader_record_count++;
    } else {
        fprintf(stderr, "[fp2/shader] tracking table full at shader=%u\n",
                shader);
    }
    pthread_mutex_unlock(&shader_record_lock);
}

static GLenum shader_type(GLuint shader)
{
    GLenum type = 0;
    pthread_mutex_lock(&shader_record_lock);
    for (size_t i = 0; i < shader_record_count; i++) {
        if (shader_records[i].shader == shader) {
            type = shader_records[i].type;
            break;
        }
    }
    pthread_mutex_unlock(&shader_record_lock);
    return type;
}

static fp2_shader_record *shader_record_unlocked(GLuint shader)
{
    for (size_t i = 0; i < shader_record_count; i++)
        if (shader_records[i].shader == shader)
            return &shader_records[i];
    return NULL;
}

static void remember_driver_source(GLuint shader, const char *source,
                                   size_t length)
{
#ifdef FP2_RELEASE_BUILD
    (void)shader; (void)source; (void)length;
    return;
#else
    if (!getenv("FP2_SHADER_FAILURE_DUMP") || !source)
        return;
    char *copy = malloc(length + 1);
    if (!copy)
        return;
    memcpy(copy, source, length);
    copy[length] = '\0';
    pthread_mutex_lock(&shader_record_lock);
    fp2_shader_record *record = shader_record_unlocked(shader);
    if (!record) {
        pthread_mutex_unlock(&shader_record_lock);
        free(copy);
        return;
    }
    free(record->driver_source);
    record->driver_source = copy;
    record->driver_source_length = length;
    pthread_mutex_unlock(&shader_record_lock);
#endif
}

static void forget_shader(GLuint shader)
{
    pthread_mutex_lock(&shader_record_lock);
    for (size_t i = 0; i < shader_record_count; i++) {
        if (shader_records[i].shader != shader)
            continue;
        free(shader_records[i].driver_source);
        size_t last = shader_record_count - 1;
        if (i != last)
            shader_records[i] = shader_records[last];
        memset(&shader_records[last], 0, sizeof shader_records[last]);
        shader_record_count--;
        pthread_mutex_unlock(&shader_record_lock);
        return;
    }
    pthread_mutex_unlock(&shader_record_lock);
}

static const char *shader_stage(GLuint shader)
{
    GLenum type = shader_type(shader);
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
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] create %s shader=%u\n",
                type == GL_VERTEX_SHADER ? "vertex"
                : type == GL_FRAGMENT_SHADER ? "fragment" : "unknown",
                shader);
    return shader;
}

static void my_glDeleteShader(GLuint shader)
{
    static void (*real)(GLuint);
    if (!real)
        real = gl_raw("glDeleteShader");
    if (real)
        real(shader);
    forget_shader(shader);
}

static void my_glShaderSource(GLuint shader, GLsizei count,
                              const GLchar *const *strings,
                              const GLint *lengths)
{
    static void (*real)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    if (!real)
        real = gl_raw("glShaderSource");
    pthread_mutex_lock(&shader_record_lock);
    fp2_shader_record *record = shader_record_unlocked(shader);
    if (record) {
        free(record->driver_source);
        record->driver_source = NULL;
        record->driver_source_length = 0;
    }
    pthread_mutex_unlock(&shader_record_lock);
    shader_sources++;
    size_t total = 0;
    int valid_source = count > 0 && strings;
    if (valid_source) {
        for (GLsizei i = 0; i < count; i++) {
            if (!strings[i]) {
                valid_source = 0;
                break;
            }
            size_t n = lengths && lengths[i] >= 0
                         ? (size_t)lengths[i] : strlen(strings[i]);
            if (n > SIZE_MAX - total) {
                valid_source = 0;
                break;
            }
            total += n;
        }
    }
    if (fp2_trace_gl) {
        char preview[161];
        size_t used = 0;
        for (GLsizei i = 0; i < count; i++) {
            size_t n = lengths && lengths[i] >= 0
                         ? (size_t)lengths[i] : strlen(strings[i]);
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
                "[fp2/gl] source #%lu shader=%u stage=%s bytes=%zu: %.160s\n",
                shader_sources, shader, shader_stage(shader), total, preview);
        if (getenv("FP2_GLSOURCE")) {
            fprintf(stderr, "[fp2/gl/source-begin] shader=%u stage=%s\n",
                    shader, shader_stage(shader));
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                fwrite(strings[i], 1, n, stderr);
            }
            fprintf(stderr, "\n[fp2/gl/source-end] shader=%u\n", shader);
        }
    }

    GLenum type = shader_type(shader);
    enum fp2_shader_stage stage = type == GL_VERTEX_SHADER
        ? FP2_SHADER_STAGE_VERTEX
        : type == GL_FRAGMENT_SHADER ? FP2_SHADER_STAGE_FRAGMENT : 0;
    if (valid_source && stage && total < (size_t)INT_MAX) {
        char *joined = malloc(total + 1);
        if (joined) {
            size_t offset = 0;
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                memcpy(joined + offset, strings[i], n);
                offset += n;
            }
            joined[total] = '\0';

            char *translated = NULL;
            size_t translated_len = 0;
            char reason[192] = "";
            fp2_shader_attribs parsed_attribs = { 0 };
            int result = fp2_unity6_translate_shader_ex(
                stage, joined, total, &translated, &translated_len,
                &parsed_attribs, reason, sizeof reason);
            if (result == FP2_SHADER_TRANSLATED &&
                translated_len < (size_t)INT_MAX) {
                shader_translated++;
                pthread_mutex_lock(&shader_record_lock);
                fp2_shader_record *rec = shader_record_unlocked(shader);
                if (rec)
                    rec->attribs = parsed_attribs;
                pthread_mutex_unlock(&shader_record_lock);
#ifndef FP2_RELEASE_BUILD
                if (fp2_trace_gl || shader_translated <= 16)
                    fprintf(stderr,
                            "[fp2/shader] translated #%lu shader=%u "
                            "stage=%s %zu -> %zu bytes (attribs=%zu)\n",
                            shader_translated, shader, shader_stage(shader),
                            total, translated_len, parsed_attribs.count);
                if (getenv("FP2_GLSOURCE_TRANSLATED")) {
                    fprintf(stderr,
                            "[fp2/gl/translated-begin] shader=%u stage=%s\n",
                            shader, shader_stage(shader));
                    fwrite(translated, 1, translated_len, stderr);
                    fprintf(stderr,
                            "\n[fp2/gl/translated-end] shader=%u\n", shader);
                }
#endif
                const GLchar *one_source = translated;
                GLint one_length = (GLint)translated_len;
                remember_driver_source(shader, translated, translated_len);
                real(shader, 1, &one_source, &one_length);
                free(translated);
                free(joined);
                return;
            }
            if (result < 0) {
                shader_rejected++;
                fprintf(stderr,
                        "[fp2/shader] rejected #%lu shader=%u stage=%s: %s\n",
                        shader_rejected, shader, shader_stage(shader),
                        reason[0] ? reason : "translation failed");
            }
            remember_driver_source(shader, joined, total);
            free(translated);
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
    if (fp2_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(shader, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr,
                "[fp2/gl] compile #%lu shader=%u stage=%s ok=%d log=%s\n",
                shader_compiles, shader, shader_stage(shader), ok == GL_TRUE,
                log[0] ? log : "(empty)");
#ifndef FP2_RELEASE_BUILD
        if (ok != GL_TRUE && getenv("FP2_SHADER_FAILURE_DUMP")) {
            char *source = NULL;
            size_t source_length = 0;
            pthread_mutex_lock(&shader_record_lock);
            fp2_shader_record *record = shader_record_unlocked(shader);
            if (record && record->driver_source) {
                source_length = record->driver_source_length;
                source = malloc(source_length + 1);
                if (source) {
                    memcpy(source, record->driver_source, source_length);
                    source[source_length] = '\0';
                }
            }
            pthread_mutex_unlock(&shader_record_lock);
            if (source) {
                fprintf(stderr,
                        "[fp2/gl/failed-source-begin] shader=%u stage=%s "
                        "bytes=%zu\n",
                        shader, shader_stage(shader), source_length);
                fwrite(source, 1, source_length, stderr);
                fprintf(stderr,
                        "\n[fp2/gl/failed-source-end] shader=%u\n", shader);
                fp2_diag_sync();
                free(source);
            }
        }
#endif
    }
}

static void my_glAttachShader(GLuint program, GLuint shader)
{
    static void (*real)(GLuint, GLuint);
    if (!real)
        real = gl_raw("glAttachShader");
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] attach-shader program=%u shader=%u\n",
                program, shader);
    pthread_mutex_lock(&program_record_lock);
    fp2_program_record *prog = NULL;
    for (size_t i = 0; i < program_record_count; i++) {
        if (program_records[i].program == program) {
            prog = &program_records[i];
            break;
        }
    }
    if (!prog && program_record_count < sizeof program_records / sizeof *program_records) {
        prog = &program_records[program_record_count++];
        prog->program = program;
        prog->attached_count = 0;
    }
    if (prog && prog->attached_count < sizeof prog->attached_shaders / sizeof *prog->attached_shaders) {
        prog->attached_shaders[prog->attached_count++] = shader;
    }
    pthread_mutex_unlock(&program_record_lock);
    if (real)
        real(program, shader);
}


/* --------------------------------------------------------------------------
 * QUEM DESENHOU ISSO?  O GL nao conhece o nome do shader da Unity, entao um
 * quadrado errado na tela nao tem autor.  Os shaders GLES2 que este port injeta
 * levam um comentario `// FP2SHADER <nome>|subN|passN` na primeira linha; aqui
 * ele e' lido no link e vira o nome do programa.  `FP2_SKIP_SHADER=<trecho>`
 * pula os desenhos desse programa -- e' assim que se isola um pass suspeito sem
 * regerar os 890 MB de dados a cada tentativa.
 * ------------------------------------------------------------------------ */
#define FP2_PROGRAM_LABELS 1024
#ifndef FP2_RELEASE_BUILD
static char program_labels[FP2_PROGRAM_LABELS][96];
#endif

static void remember_program_label(GLuint program)
{
#ifdef FP2_RELEASE_BUILD
    (void)program;
    return;
#else
    if (program >= FP2_PROGRAM_LABELS)
        return;
    char found[96] = "";
    pthread_mutex_lock(&program_record_lock);
    for (size_t i = 0; i < program_record_count && !found[0]; i++) {
        if (program_records[i].program != program)
            continue;
        for (size_t k = 0; k < program_records[i].attached_count; k++) {
            GLuint shader = program_records[i].attached_shaders[k];
            pthread_mutex_lock(&shader_record_lock);
            fp2_shader_record *record = shader_record_unlocked(shader);
            const char *src = record ? record->driver_source : NULL;
            const char *mark = src ? strstr(src, "FP2SHADER_") : NULL;
            if (mark) {
                mark += strlen("FP2SHADER_");
                size_t n = strcspn(mark, ";\n ");
                if (n >= sizeof found)
                    n = sizeof found - 1;
                memcpy(found, mark, n);
                found[n] = '\0';
            }
            pthread_mutex_unlock(&shader_record_lock);
            if (found[0])
                break;
        }
    }
    pthread_mutex_unlock(&program_record_lock);
    snprintf(program_labels[program], sizeof program_labels[program], "%s",
             found[0] ? found : "(desconhecido)");
    if (fp2_env_flag("FP2_PROGRAM_MAP"))
        fprintf(stderr, "[fp2/gl] programa %u = %s\n", program,
                program_labels[program]);
#endif
}

static int program_is_skipped(GLuint program)
{
#ifdef FP2_RELEASE_BUILD
    (void)program;
    return 0;
#else
    static const char *needle;
    static int known;
    if (!known) {
        needle = getenv("FP2_SKIP_SHADER");
        known = 1;
    }
    if (!needle || !*needle || program >= FP2_PROGRAM_LABELS)
        return 0;
    return strstr(program_labels[program], needle) != NULL;
#endif
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
    remember_program_label(program);
    GLint ok = GL_FALSE, loglen = 0;
    getiv(program, GL_LINK_STATUS, &ok);
    getiv(program, GL_INFO_LOG_LENGTH, &loglen);
    if (fp2_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(program, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr, "[fp2/gl] link #%lu program=%u ok=%d log=%s\n",
                program_links, program, ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
    if (ok == GL_TRUE) {
        static void (*use_prog)(GLuint);
        static GLint (*get_uni_loc)(GLuint, const GLchar *);
        static void (*uni1f)(GLint, GLfloat);
        static void (*uni2f)(GLint, GLfloat, GLfloat);
        static void (*uni4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
        if (!use_prog) use_prog = gl_raw("glUseProgram");
        if (!get_uni_loc) get_uni_loc = gl_raw("glGetUniformLocation");
        if (!uni1f) uni1f = gl_raw("glUniform1f");
        if (!uni2f) uni2f = gl_raw("glUniform2f");
        if (!uni4f) uni4f = gl_raw("glUniform4f");
        if (use_prog && get_uni_loc) {
            use_prog(program);
            GLint loc_sepia = get_uni_loc(program, "_SEPIA_GLOBAL");
            if (loc_sepia >= 0 && uni1f) uni1f(loc_sepia, 0.0f);
            GLint loc_flip = get_uni_loc(program, "_Flip");
            if (loc_flip >= 0 && uni2f) uni2f(loc_flip, 1.0f, 1.0f);
            GLint loc_color = get_uni_loc(program, "_Color");
            if (loc_color >= 0 && uni4f) uni4f(loc_color, 1.0f, 1.0f, 1.0f, 1.0f);
            GLint loc_rcolor = get_uni_loc(program, "_RendererColor");
            if (loc_rcolor >= 0 && uni4f) uni4f(loc_rcolor, 1.0f, 1.0f, 1.0f, 1.0f);
            GLint loc_st = get_uni_loc(program, "_MainTex_ST");
            if (loc_st >= 0 && uni4f) uni4f(loc_st, 1.0f, 1.0f, 0.0f, 0.0f);
            use_prog(current_program);
        }
    }
}

static void my_glUseProgram(GLuint program)
{
    static void (*real)(GLuint);
    current_program = program;
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] use-program %u\n", program);
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
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] attrib program=%u location=%d name=%s\n",
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
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] buffer-bind target=%#x buffer=%u\n",
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
#ifndef FP2_RELEASE_BUILD
    static void (*sub)(GLenum, GLintptr, GLsizeiptr, const void *);
#endif
    buffer_uploads++;
    if (fp2_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[fp2/gl] buffer-data #%lu target=%#x buffer=%u bytes=%ld "
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
#ifndef FP2_RELEASE_BUILD
    static int split_upload = -1;
    if (split_upload < 0)
        split_upload = fp2_env_flag("FP2_BUFFER_SPLIT_UPLOAD");
    if (split_upload && data && size > 0 &&
        usage == GL_DYNAMIC_DRAW) {
        if (!sub)
            sub = gl_raw("glBufferSubData");
        if (sub) {
            real(target, size, NULL, usage);
            sub(target, 0, size, data);
            if (fp2_trace_gl)
                fprintf(stderr,
                        "[fp2/gl] split buffer upload target=%#x "
                        "buffer=%u bytes=%ld\n",
                        target, buffer, (long)size);
            return;
        }
    }
#endif
    real(target, size, data, usage);
}

static void my_glBufferSubData(GLenum target, GLintptr offset,
                               GLsizeiptr size, const void *data)
{
    static void (*real)(GLenum, GLintptr, GLsizeiptr, const void *);
    buffer_uploads++;
    if (fp2_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[fp2/gl] buffer-sub #%lu target=%#x buffer=%u offset=%ld "
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
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/gl] attrib-pointer index=%u size=%d type=%#x norm=%u "
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
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] attrib-enable index=%u\n", index);
    if (!real)
        real = gl_raw("glEnableVertexAttribArray");
    if (real)
        real(index);
}

static void my_glDisableVertexAttribArray(GLuint index)
{
    static void (*real)(GLuint);
    static void (*attrib4f)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] attrib-disable index=%u\n", index);
    if (!real)
        real = gl_raw("glDisableVertexAttribArray");
    if (!attrib4f)
        attrib4f = gl_raw("glVertexAttrib4f");
    if (real)
        real(index);
    if (attrib4f && index > 0)
        attrib4f(index, 1.0f, 1.0f, 1.0f, 1.0f);
}

static void my_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                      GLenum textarget, GLuint texture,
                                      GLint level)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint, GLint);
    int restore_draw;
    GLenum physical_target = framebuffer_operation_begin(target,
                                                          &restore_draw);
    GLenum physical_textarget = fp2_gles3_texture_target(textarget);
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/gl] fbo-texture fbo=%u target=%#x attachment=%#x "
                "textarget=%#x texture=%u level=%d\n",
                gl_state.framebuffer, target, attachment, textarget, texture,
                level);
    if (texture && physical_textarget == GL_TEXTURE_2D) {
        fp2_texture_record *record = find_texture_record(texture, 1);
        if (record)
            record->attached = 1;
    }
    if (!real)
        real = gl_raw("glFramebufferTexture2D");
    if (real)
        real(physical_target, attachment, physical_textarget, texture, level);
    framebuffer_operation_end(restore_draw);
}

static void my_glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                         GLenum renderbuffer_target,
                                         GLuint renderbuffer)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint);
    int restore_draw;
    GLenum physical_target = framebuffer_operation_begin(target,
                                                          &restore_draw);
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/gl] fbo-renderbuffer fbo=%u target=%#x attachment=%#x "
                "renderbuffer-target=%#x renderbuffer=%u\n",
                gl_state.framebuffer, target, attachment,
                renderbuffer_target, renderbuffer);
    if (!real)
        real = gl_raw("glFramebufferRenderbuffer");
    if (real)
        real(physical_target, attachment, renderbuffer_target, renderbuffer);
    framebuffer_operation_end(restore_draw);
}

static void my_glRenderbufferStorage(GLenum target, GLenum internal_format,
                                     GLsizei width, GLsizei height)
{
    static void (*real)(GLenum, GLenum, GLsizei, GLsizei);
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/gl] renderbuffer-storage target=%#x internal=%#x "
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
    int restore_draw;
    GLenum physical_target = framebuffer_operation_begin(target,
                                                          &restore_draw);
    if (!real)
        real = gl_raw("glCheckFramebufferStatus");
    GLenum status = real ? real(physical_target) : 0;
    framebuffer_operation_end(restore_draw);
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/gl] fbo-status read=%u draw=%u target=%#x status=%#x\n",
                gl_state.read_framebuffer, gl_state.framebuffer, target,
                status);
    return status;
}

static void trace_current_fbo_status(void)
{
#ifdef FP2_RELEASE_BUILD
    return;
#else
    if (gl_state.framebuffer == 0)
        return;
    static GLenum (*check)(GLenum);
    if (!check)
        check = gl_raw("glCheckFramebufferStatus");
    if (check) {
        GLenum status = check(GL_FRAMEBUFFER);
        if (status != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
            fprintf(stderr, "[fp2/gl] INCOMPLETE fbo=%u status=%#x\n",
                    gl_state.framebuffer, status);
        } else if (fp2_trace_gl && draw_calls <= 20) {
            fprintf(stderr, "[fp2/gl] pre-draw fbo=%u status=%#x\n",
                    gl_state.framebuffer, status);
        }
    }
#endif
}

static GLint my_glGetUniformLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetUniformLocation");
    GLint location = real ? real(program, name) : -1;
    remember_uniform(program, location, name);
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] uniform program=%u location=%d name=%s\n",
                program, location, name ? name : "(null)");
    return location;
}

static void my_glUniform1i(GLint location, GLint value)
{
    static void (*real)(GLint, GLint);
    if (!real)
        real = gl_raw("glUniform1i");
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] uniform1i location=%d name=%s value=%d\n",
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
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/gl] uniform4f location=%d name=%s "
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
    if (fp2_trace_gl && value && count > 0) {
        fprintf(stderr,
                "[fp2/gl] uniform4fv location=%d name=%s count=%d "
                "first=%.9g,%.9g,%.9g,%.9g\n",
                location, uniform_label(location), count,
                value[0], value[1], value[2], value[3]);
        if (count == 4)
            fprintf(stderr,
                    "[fp2/gl] uniform4fv-matrix location=%d name=%s "
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
    GLenum physical_target = fp2_gles3_texture_target(target);
    texture_binds++;
    if (physical_target == GL_TEXTURE_2D)
        bound_texture_2d = texture;
    if (fp2_trace_gl && texture_binds <= 160)
        fprintf(stderr,
                "[fp2/gl] tex-bind #%lu unit=%#x target=%#x texture=%u\n",
                texture_binds, active_texture_unit, target, texture);
    if (!real)
        real = gl_raw("glBindTexture");
    if (real) {
        real(physical_target, texture);
        extern void fp2_gles3_texture_bound(GLenum, GLenum, GLuint);
        fp2_gles3_texture_bound(active_texture_unit, physical_target, texture);
    }
}

static void my_glDeleteTextures(GLsizei count, const GLuint *textures)
{
    static void (*real)(GLsizei, const GLuint *);
    if (!real)
        real = gl_raw("glDeleteTextures");
    if (real)
        real(count, textures);
    if (!textures || count <= 0)
        return;
    for (GLsizei i = 0; i < count; i++) {
        if (textures[i] == bound_texture_2d)
            bound_texture_2d = 0;
        forget_texture_record(textures[i]);
    }
}

static void my_glPixelStorei(GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLint);
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:
        unpack_alignment = value;
        break;
    case 0x0CF2: /* GL_UNPACK_ROW_LENGTH */
        unpack_row_length = value;
        return;
    case 0x0CF3: /* GL_UNPACK_SKIP_ROWS */
        unpack_skip_rows = value;
        return;
    case 0x0CF4: /* GL_UNPACK_SKIP_PIXELS */
        unpack_skip_pixels = value;
        return;
    case 0x806E: /* GL_UNPACK_IMAGE_HEIGHT */
        unpack_image_height = value;
        return;
    case 0x806D: /* GL_UNPACK_SKIP_IMAGES */
        unpack_skip_images = value;
        return;
    default:
        break;
    }
    if (fp2_trace_gl)
        fprintf(stderr, "[fp2/gl] pixel-store pname=%#x value=%d\n",
                pname, value);
    if (!real)
        real = gl_raw("glPixelStorei");
    if (real)
        real(pname, value);
}

static void my_glTexParameteri(GLenum target, GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLenum, GLint);
    GLenum physical_target = fp2_gles3_texture_target(target);
    texture_parameters++;
    if (fp2_trace_gl && texture_parameters <= 240)
        fprintf(stderr,
                "[fp2/gl] tex-param #%lu unit=%#x texture=%u target=%#x "
                "pname=%#x value=%#x\n",
                texture_parameters, active_texture_unit, bound_texture_2d,
                target, pname, value);
    if (!real)
        real = gl_raw("glTexParameteri");
    if (real)
        real(physical_target, pname, value);
}

static int texture_level_is_full(const fp2_texture_record *record, GLint level,
                                 GLint x, GLint y, GLsizei width,
                                 GLsizei height)
{
    if (!record || record->attached || level < 0 ||
        level >= record->levels || x != 0 || y != 0)
        return 0;
    GLsizei expected_width = record->width;
    GLsizei expected_height = record->height;
    for (GLint current = 0; current < level; current++) {
        if (expected_width > 1)
            expected_width >>= 1;
        if (expected_height > 1)
            expected_height >>= 1;
    }
    return width == expected_width && height == expected_height;
}

static void set_raw_unpack_alignment(GLint value)
{
    static void (*real)(GLenum, GLint);
    if (!real)
        real = gl_raw("glPixelStorei");
    if (real)
        real(GL_UNPACK_ALIGNMENT, value);
}

static void trace_compact_texture(GLuint texture, GLint level, GLsizei width,
                                  GLsizei height, size_t source_size,
                                  size_t uploaded_size, const char *mode)
{
#ifdef FP2_RELEASE_BUILD
    (void)texture; (void)level; (void)width; (void)height;
    (void)source_size; (void)uploaded_size; (void)mode;
#else
    static unsigned long uploads;
    static unsigned long long source_total;
    static unsigned long long uploaded_total;
    uploads++;
    source_total += source_size;
    uploaded_total += uploaded_size;
    if (uploads <= 240 || uploads % 100 == 0)
        fprintf(stderr,
                "[fp2/texture16] upload #%lu texture=%u level=%d size=%dx%d "
                "mode=%s rgba8888=%zu compact=%zu totals=%llu->%llu\n",
                uploads, texture, level, width, height, mode, source_size,
                uploaded_size, source_total, uploaded_total);
#endif
}

static uint16_t *pack_rgba4444(const unsigned char *rgba, int width,
                               int height, size_t *packed_size);
static uint16_t *pack_rgb565(const unsigned char *rgba, int width, int height,
                             size_t *packed_size);

static int texture_record_is_large(const fp2_texture_record *record)
{
    if (!record || record->width <= 0 || record->height <= 0 ||
        (size_t)record->width > SIZE_MAX / 4 / (size_t)record->height)
        return 0;
    size_t bytes = (size_t)record->width * (size_t)record->height * 4;
    return bytes >= texture_16bit_min_bytes();
}

static int fp2_gl_bytes_per_pixel(GLenum format, GLenum type)
{
    int channels = 4;
    switch (format) {
    case 0x1908: /* GL_RGBA */
    case 0x8058: /* GL_RGBA8 */
    case 0x8C43: /* GL_SRGB8_ALPHA8 */
    case 0x80E1: /* GL_BGRA_EXT */
        channels = 4;
        break;
    case 0x1907: /* GL_RGB */
    case 0x8051: /* GL_RGB8 */
    case 0x8C41: /* GL_SRGB8 */
        channels = 3;
        break;
    case 0x190A: /* GL_LUMINANCE_ALPHA */
    case 0x8227: /* GL_RG */
    case 0x822B: /* GL_RG8 */
        channels = 2;
        break;
    case 0x1906: /* GL_ALPHA */
    case 0x1909: /* GL_LUMINANCE */
    case 0x1903: /* GL_RED */
    case 0x8229: /* GL_R8 */
        channels = 1;
        break;
    default:
        channels = 4;
        break;
    }

    switch (type) {
    case 0x1401: /* GL_UNSIGNED_BYTE */
    case 0x1400: /* GL_BYTE */
        return channels;
    case 0x8033: /* GL_UNSIGNED_SHORT_4_4_4_4 */
    case 0x8034: /* GL_UNSIGNED_SHORT_5_5_5_1 */
    case 0x8363: /* GL_UNSIGNED_SHORT_5_6_5 */
        return 2;
    case 0x1403: /* GL_UNSIGNED_SHORT */
    case 0x1402: /* GL_SHORT */
    case 0x8D61: /* GL_HALF_FLOAT_OES */
        return channels * 2;
    case 0x1406: /* GL_FLOAT */
    case 0x1405: /* GL_UNSIGNED_INT */
    case 0x1404: /* GL_INT */
        return channels * 4;
    default:
        return channels;
    }
}

static const void *unpack_subimage_buffer(GLsizei width, GLsizei height,
                                         GLenum format, GLenum type,
                                         const void *data, void **to_free)
{
    *to_free = NULL;
    if (!data || width <= 0 || height <= 0)
        return data;

    int bpp = fp2_gl_bytes_per_pixel(format, type);
    int row_length = unpack_row_length > 0 ? unpack_row_length : width;
    int align = unpack_alignment > 0 ? unpack_alignment : 1;
    int raw_row_bytes = row_length * bpp;
    int src_stride = ((raw_row_bytes + align - 1) / align) * align;
    int dst_stride = width * bpp;

    if (src_stride == dst_stride && unpack_skip_rows == 0 && unpack_skip_pixels == 0) {
        return data;
    }

    size_t total_bytes = (size_t)dst_stride * (size_t)height;
    uint8_t *packed = malloc(total_bytes);
    if (!packed)
        return data;

    const uint8_t *src = (const uint8_t *)data +
                         (size_t)unpack_skip_rows * (size_t)src_stride +
                         (size_t)unpack_skip_pixels * (size_t)bpp;

    for (int y = 0; y < height; y++) {
        memcpy(packed + (size_t)y * dst_stride, src + (size_t)y * src_stride, dst_stride);
    }

    *to_free = packed;
    return packed;
}

static const void *expand_single_channel_to_la(GLsizei width, GLsizei height,
                                               GLenum format, GLenum type,
                                               const void *data, void **to_free)
{
    *to_free = NULL;
    if (!data || width <= 0 || height <= 0 || type != 0x1401 /* GL_UNSIGNED_BYTE */)
        return data;
    if (format != 0x1903 /* GL_RED */ && format != 0x8229 /* GL_R8 */ &&
        format != 0x1906 /* GL_ALPHA */ && format != 0x1909 /* GL_LUMINANCE */)
        return data;

    size_t count = (size_t)width * (size_t)height;
    uint8_t *la = malloc(count * 2);
    if (!la)
        return data;

    const uint8_t *src = (const uint8_t *)data;
    if (format == 0x1906 /* GL_ALPHA */) {
        for (size_t i = 0; i < count; i++) {
            la[i * 2 + 0] = 255;
            la[i * 2 + 1] = src[i];
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            la[i * 2 + 0] = src[i];
            la[i * 2 + 1] = src[i];
        }
    }
    *to_free = la;
    return la;
}

/* CENSO DE FORMATO DE TEXTURA.  Nao adianta supor o que a Unity manda para a
 * GPU: com FP2_TEX_CENSUS=1 cada upload e' contado por formato e o resumo sai
 * no fim.  E' assim que se sabe se as 11.702 ETC2_RGBA8 do pacote chegam
 * mesmo ao driver, se a engine as converte sozinha antes, ou se elas nem sao
 * pedidas. */
#define FP2_CENSUS_SLOTS 32
#ifndef FP2_RELEASE_BUILD
static struct { unsigned key; unsigned long count; unsigned long long pixels; }
    fp2_census[FP2_CENSUS_SLOTS];
static int fp2_census_on = -1;
#endif

static void fp2_census_add(unsigned key, GLsizei width, GLsizei height)
{
#ifdef FP2_RELEASE_BUILD
    (void)key; (void)width; (void)height;
#else
    if (fp2_census_on < 0)
        fp2_census_on = fp2_env_flag("FP2_TEX_CENSUS");
    if (!fp2_census_on)
        return;
    for (int i = 0; i < FP2_CENSUS_SLOTS; i++) {
        if (fp2_census[i].count && fp2_census[i].key != key)
            continue;
        fp2_census[i].key = key;
        fp2_census[i].count++;
        if (width > 0 && height > 0)
            fp2_census[i].pixels += (unsigned long long)width * height;
        return;
    }
#endif
}

void fp2_texture_census_report(void)
{
#ifdef FP2_RELEASE_BUILD
    return;
#else
    if (fp2_census_on <= 0)
        return;
    for (int i = 0; i < FP2_CENSUS_SLOTS; i++) {
        if (!fp2_census[i].count)
            continue;
        fprintf(stderr, "[fp2/tex] formato %#x: %lu upload(s), %.1f Mpx\n",
                fp2_census[i].key, fp2_census[i].count,
                (double)fp2_census[i].pixels / 1e6);
    }
    fflush(stderr);
#endif
}


static void my_glTexImage2D(GLenum target, GLint level, GLint internal_format,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                        GLenum, const void *);
    GLenum physical_target = fp2_gles3_texture_target(target);
    GLenum physical_internal = fp2_gles3_texture_format(internal_format);
    GLenum physical_format = fp2_gles3_texture_format(format);
    texture_images++;
    /* Marcar o formato SEM compressao com o bit alto, para separa-lo dos
     * enums de formato comprimido no mesmo censo. */
    fp2_census_add(0x80000000u | (unsigned)internal_format, width, height);
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[fp2/gl] tex-image #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d border=%d format=%#x "
                "type=%#x data=%s\n",
                texture_images, active_texture_unit, bound_texture_2d, target,
                level, internal_format, width, height, border, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexImage2D");
    if (real) {
        void *unpacked_alloc = NULL;
        const void *unpacked_data = unpack_subimage_buffer(
            width, height, format, type, data, &unpacked_alloc);
        void *la_alloc = NULL;
        const void *final_data = expand_single_channel_to_la(
            width, height, format, type, unpacked_data, &la_alloc);
        GLint old_unpack = unpack_alignment;
        if ((la_alloc || unpacked_alloc) && old_unpack != 1 && final_data)
            set_raw_unpack_alignment(1);
        real(physical_target, level, (GLint)physical_internal, width, height,
             border, physical_format, type, final_data);
        if ((la_alloc || unpacked_alloc) && old_unpack != 1 && final_data)
            set_raw_unpack_alignment(old_unpack);
        if (la_alloc)
            free(la_alloc);
        if (unpacked_alloc)
            free(unpacked_alloc);
    }
    if (physical_target == GL_TEXTURE_2D && bound_texture_2d && level == 0 &&
        width > 0 && height > 0) {
        fp2_texture_record *record = find_texture_record(bound_texture_2d, 1);
        if (record) {
            record->width = width;
            record->height = height;
            if (record->levels < 1)
                record->levels = 1;
            record->internal_format = internal_format;
            record->compact_levels = 0;
            record->compact_mode = 0;
        }
    }
}

static void my_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                               GLsizei width, GLsizei height, GLenum format,
                               GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                        GLenum, const void *);
    static void (*image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                         GLenum, const void *);
    GLenum physical_target = fp2_gles3_texture_target(target);
    GLenum physical_format = fp2_gles3_texture_format(format);
    texture_sub_images++;
    if (trace_texture_call(texture_sub_images, width, height))
        fprintf(stderr,
                "[fp2/gl] tex-sub #%lu unit=%#x texture=%u target=%#x "
                "level=%d xy=%d,%d size=%dx%d format=%#x type=%#x data=%s\n",
                texture_sub_images, active_texture_unit, bound_texture_2d,
                target, level, x, y, width, height, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexSubImage2D");

    void *unpacked_alloc = NULL;
    const void *unpacked_data = unpack_subimage_buffer(
        width, height, format, type, data, &unpacked_alloc);
    void *la_alloc = NULL;
    const void *final_data = expand_single_channel_to_la(
        width, height, format, type, unpacked_data, &la_alloc);

    fp2_texture_record *record = physical_target == GL_TEXTURE_2D
                              ? find_texture_record(bound_texture_2d, 0)
                              : NULL;
    int is_font_or_single = (physical_format == 0x1906 /* GL_ALPHA */ ||
                             physical_format == 0x1909 /* GL_LUMINANCE */ ||
                             physical_format == 0x190A /* GL_LUMINANCE_ALPHA */ ||
                             format == 0x8229 /* GL_R8 */ ||
                             format == 0x1903 /* GL_RED */ ||
                             format == 0x1906 /* GL_ALPHA */ ||
                             (record && (record->internal_format == 0x8229 ||
                                         record->internal_format == 0x1903 ||
                                         record->internal_format == 0x1906 ||
                                         record->internal_format == 0x1909)));

    if (texture_16bit_enabled() && !is_font_or_single && final_data &&
        type == GL_UNSIGNED_BYTE &&
        physical_format == GL_RGBA &&
        fp2_gles3_texture_format(record ? record->internal_format : 0) ==
            GL_RGBA &&
        (record->compact_mode ||
         (level == 0 && texture_record_is_large(record))) &&
        texture_level_is_full(record, level, x, y, width, height)) {
        size_t packed_size = 0;
        if (!record->compact_mode)
            record->compact_mode = fp2_rgba8888_is_opaque(
                final_data, width, height) ? 2 : 1;
        uint16_t *packed = record->compact_mode == 2
                         ? pack_rgb565(final_data, width, height, &packed_size)
                         : pack_rgba4444(final_data, width, height, &packed_size);
        if (packed) {
            if (!image)
                image = gl_raw("glTexImage2D");
            if (image) {
                GLint old_unpack = unpack_alignment;
                if (old_unpack != 1)
                    set_raw_unpack_alignment(1);
                GLenum compact_format = record->compact_mode == 2
                                      ? GL_RGB : GL_RGBA;
                GLenum compact_type = record->compact_mode == 2
                                    ? GL_UNSIGNED_SHORT_5_6_5
                                    : GL_UNSIGNED_SHORT_4_4_4_4;
                image(GL_TEXTURE_2D, level, (GLint)compact_format, width,
                      height, 0, compact_format, compact_type, packed);
                if (old_unpack != 1)
                    set_raw_unpack_alignment(old_unpack);
                if (level < 64)
                    record->compact_levels |= UINT64_C(1) << level;
                trace_compact_texture(bound_texture_2d, level, width, height,
                                      (size_t)width * (size_t)height * 4,
                                      packed_size,
                                      record->compact_mode == 2
                                          ? "rgb565" : "rgba4444");
                free(packed);
                if (la_alloc) free(la_alloc);
                if (unpacked_alloc) free(unpacked_alloc);
                return;
            }
            free(packed);
        }
    }
    if (real) {
        GLint old_unpack = unpack_alignment;
        if ((la_alloc || unpacked_alloc) && old_unpack != 1 && final_data)
            set_raw_unpack_alignment(1);
        real(physical_target, level, x, y, width, height, physical_format,
             type, final_data);
        if ((la_alloc || unpacked_alloc) && old_unpack != 1 && final_data)
            set_raw_unpack_alignment(old_unpack);
    }
    if (la_alloc) free(la_alloc);
    if (unpacked_alloc) free(unpacked_alloc);
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real)
        real = gl_raw("glDrawArrays");
    if (program_is_skipped(current_program))
        return;
#ifndef FP2_RELEASE_BUILD
    if (gl_state.framebuffer == 0 && draw_calls <= 40)
        fprintf(stderr,
                "[fp2/gl] draw #%lu arrays mode=%#x first=%d count=%d "
                "program=%u fbo=%u vbo=%u tex=%u\n",
                draw_calls, mode, first, count, current_program,
                gl_state.framebuffer, bound_array_buffer, bound_texture_2d);
#endif
    trace_current_fbo_status();
    if (current_program == 4 && gl_state.framebuffer == 0) {
        static void (*enable)(GLenum);
        static void (*blend_func)(GLenum, GLenum);
        if (!enable) enable = gl_raw("glEnable");
        if (!blend_func) blend_func = gl_raw("glBlendFunc");
        if (enable && blend_func) {
            enable(GL_BLEND);
            blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    real(mode, first, count);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices)
{
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real)
        real = gl_raw("glDrawElements");
    if (program_is_skipped(current_program))
        return;
    draw_calls++;
#ifndef FP2_RELEASE_BUILD
    if (gl_state.framebuffer == 0 && draw_calls <= 40)
        fprintf(stderr,
                "[fp2/gl] draw #%lu elements mode=%#x count=%d type=%#x "
                "indices=%p program=%u fbo=%u vbo=%u ebo=%u tex=%u\n",
                draw_calls, mode, count, type, indices, current_program,
                gl_state.framebuffer, bound_array_buffer,
                bound_element_array_buffer, bound_texture_2d);
#endif
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

static int native_astc_support(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached;
    const GLubyte *(*get_string)(GLenum) = gl_raw("glGetString");
    const char *extensions = get_string
        ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    cached = extensions &&
        (strstr(extensions, "GL_KHR_texture_compression_astc_ldr") ||
         strstr(extensions, "GL_KHR_texture_compression_astc_hdr"));
    return cached;
}

static int astc_block_size(GLenum format, int *block_x, int *block_y)
{
    static const unsigned char blocks[][2] = {
        {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12},
    };
    unsigned index;
    if (format >= 0x93B0 && format <= 0x93BD)
        index = format - 0x93B0; /* linear RGBA ASTC */
    else if (format >= 0x93D0 && format <= 0x93DD)
        index = format - 0x93D0; /* sRGB-alpha ASTC */
    else
        return 0;
    *block_x = blocks[index][0];
    *block_y = blocks[index][1];
    return 1;
}

static uint16_t *pack_rgba4444(const unsigned char *rgba, int width,
                               int height, size_t *packed_size)
{
    *packed_size = fp2_rgba4444_size(width, height);
    if (!*packed_size)
        return NULL;
    uint16_t *packed = malloc(*packed_size);
    if (!packed)
        return NULL;
    if (fp2_rgba8888_to_rgba4444(rgba, width, height, packed,
                                *packed_size) != 0) {
        free(packed);
        return NULL;
    }
    return packed;
}

static uint16_t *pack_rgb565(const unsigned char *rgba, int width, int height,
                             size_t *packed_size)
{
    *packed_size = fp2_rgb565_size(width, height);
    if (!*packed_size)
        return NULL;
    uint16_t *packed = malloc(*packed_size);
    if (!packed)
        return NULL;
    if (fp2_rgba8888_to_rgb565(rgba, width, height, packed,
                              *packed_size) != 0) {
        free(packed);
        return NULL;
    }
    return packed;
}

static unsigned char *decode_astc(GLenum format, GLsizei width,
                                  GLsizei height, const void *data,
                                  GLsizei image_size)
{
    int block_x, block_y;
    if (!data || image_size < 0 || width <= 0 || height <= 0 ||
        !astc_block_size(format, &block_x, &block_y) ||
        (size_t)width > SIZE_MAX / 4 / (size_t)height)
        return NULL;
    size_t rgba_size = (size_t)width * (size_t)height * 4;
    unsigned char *rgba = malloc(rgba_size);
    if (!rgba)
        return NULL;
    int result = astc_decode_rgba(rgba, rgba_size, data, (size_t)image_size,
                                  width, height, block_x, block_y);
    if (result) {
        fprintf(stderr,
                "[fp2/astc] upload decode failed format=%#x size=%dx%d "
                "block=%dx%d bytes=%d result=%d\n",
                format, width, height, block_x, block_y, image_size, result);
        free(rgba);
        return NULL;
    }
    if (fp2_trace_gl)
        fprintf(stderr,
                "[fp2/astc] decoded format=%#x size=%dx%d block=%dx%d "
                "bytes=%d -> %zu RGBA\n",
                format, width, height, block_x, block_y, image_size,
                rgba_size);
    return rgba;
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
    fp2_census_add(internal_format, width, height);
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[fp2/gl] tex-compressed #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d bytes=%d data=%s\n",
                texture_images, active_texture_unit, bound_texture_2d, target,
                level, internal_format, width, height, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexImage2D");
    if (!plain) plain = gl_raw("glTexImage2D");
    int astc_x, astc_y;
    int is_astc = astc_block_size(internal_format, &astc_x, &astc_y);
    if (is_astc && !native_astc_support()) {
        unsigned char *rgba = decode_astc(internal_format, width, height,
                                          data, image_size);
        if (plain) {
            plain(target, level, GL_RGBA, width, height, border, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
    }
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
                "[fp2/gl] tex-compressed-sub #%lu unit=%#x texture=%u "
                "target=%#x level=%d xy=%d,%d size=%dx%d format=%#x "
                "bytes=%d data=%s\n",
                texture_sub_images, active_texture_unit, bound_texture_2d,
                target, level, x, y, width, height, format, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexSubImage2D");
    if (!plain) plain = gl_raw("glTexSubImage2D");
    int astc_x, astc_y;
    int is_astc = astc_block_size(format, &astc_x, &astc_y);
    if (is_astc && !native_astc_support()) {
        unsigned char *rgba = decode_astc(format, width, height, data,
                                          image_size);
        if (rgba && plain) {
            plain(target, level, x, y, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
        return;
    }
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

/* Unity 6's GLES3 device resolves every GL entry point through
 * eglGetProcAddress.  On Utgard the ES3-only names simply do not exist, so the
 * engine happily stores NULL and later calls it -- the crash lands at pc=0 with
 * no hint of which function was missing.  Log every unresolved gl* name once so
 * the ES3 surface that actually has to be emulated is a measured list instead
 * of a guess. */
static void *fp2_gl_sym_impl(const char *name);

void *fp2_gl_sym(const char *name)
{
    void *f = fp2_gl_sym_impl(name);
    if (!f && name && name[0] == 'g' && name[1] == 'l') {
        static const char *seen[512];
        static int seen_n;
        int known = 0;
        for (int i = 0; i < seen_n; i++)
            if (strcmp(seen[i], name) == 0) { known = 1; break; }
        if (!known && seen_n < 512) {
            seen[seen_n++] = strdup(name);
            fprintf(stderr, "[fp2/gl] MISSING %s\n", name);
            fflush(stderr);
        }
    }
    return f;
}

static void *fp2_gl_sym_impl(const char *name)
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
    if (strcmp(name, "glDeleteShader") == 0)
        return my_glDeleteShader;
    if (strcmp(name, "glShaderSource") == 0)
        return my_glShaderSource;
    if (strcmp(name, "glCompileShader") == 0)
        return my_glCompileShader;
    if (strcmp(name, "glAttachShader") == 0)
        return my_glAttachShader;
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
    if (strcmp(name, "glDeleteTextures") == 0)
        return my_glDeleteTextures;
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
    {
        /* Unity 6 asks for the whole ES3 surface; gles3.c answers what the
         * Utgard driver cannot. */
        extern void *fp2_gles3_sym(const char *name);
        extern void *fp2_gles3_override_sym(const char *name);
        extern void *fp2_gles3_fallback(const char *name);
        void *override = fp2_gles3_override_sym(name);
        if (override)
            return override;
        void *raw = gl_raw(name);
        if (raw)
            return raw;
        void *es3 = fp2_gles3_sym(name);
        if (es3)
            return es3;
        if (name[0] == 'g' && name[1] == 'l')
            return fp2_gles3_fallback(name);
        return NULL;
    }
}

#define SYS(name, ret, ...) \
    static ret (*p_##name)(__VA_ARGS__); \
    static ret r_##name(__VA_ARGS__)

/* --- the two calls we rewrite ------------------------------------------- */

static void *egl_real(const char *name);

static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                       EGLint, EGLint *);

static EGLBoolean my_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib,
                                     EGLConfig *cfgs, EGLint n, EGLint *num)
{
    EGLint copy[64];
    size_t k = 0;
    int patched = 0;
    int saw_stencil = 0;
#ifdef FP2_RELEASE_BUILD
    const int force_stencil = 0;
#else
    int force_stencil = fp2_env_flag("FP2_FORCE_STENCIL") &&
        !(getenv("FP2_NO_FORCE_STENCIL") &&
          strcmp(getenv("FP2_NO_FORCE_STENCIL"), "0") != 0);
#endif
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 58; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_RENDERABLE_TYPE && (val & EGL_OPENGL_ES3_BIT_KHR)) {
                val = (val & ~EGL_OPENGL_ES3_BIT_KHR) | EGL_OPENGL_ES2_BIT;
                patched = 1;
            }
            if (name == EGL_STENCIL_SIZE) {
                saw_stencil = 1;
                if (force_stencil && val < 8) {
                    val = 8;
                    patched = 1;
                }
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
     * FP2_NO_FORCE_STENCIL=1 desliga o forcamento. */
    if (!saw_stencil && k < 60 && force_stencil) {
        copy[k++] = EGL_STENCIL_SIZE;
        copy[k++] = 8;
        patched = 1;
        nx_log("eglChooseConfig: STENCIL 8 forcado (a UI da Unity precisa)");
    }
    copy[k] = EGL_NONE;
    if (patched)
        nx_log("eglChooseConfig: atributos ajustados");
    if (!p_eglChooseConfig)
        p_eglChooseConfig = egl_real("eglChooseConfig");
    EGLBoolean ok = p_eglChooseConfig(dpy, attrib ? copy : NULL, cfgs, n, num);
    fprintf(stderr, "[fp2/egl] chooseConfig -> ok=%d num=%d\n", (int)ok,
            num ? *num : -1);
    fflush(stderr);
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
        p_eglCreateContext = egl_real("eglCreateContext");
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
    extern void *fp2_native_window(void);
    if (!p_eglCreateWindowSurface)
        p_eglCreateWindowSurface = sys("eglCreateWindowSurface");
    EGLNativeWindowType real = (EGLNativeWindowType)fp2_native_window();
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

/* One-shot diagnostic for the raw Mali/fbdev path.  The SDL-owned backend has
 * the equivalent hook in egl_sdl.c; keeping this immediately before the real
 * swap captures exactly what Unity is about to present. */
extern void *fp2_native_window(void);

static void capture_raw_frame_if_requested(void)
{
#ifdef FP2_RELEASE_BUILD
    return;
#else
    static int finished;
    unsigned long frame = fp2_swap_count;

    static const char *path;
    static int path_known;
    if (!path_known) {
        path = getenv("FP2_SCREENSHOT");
        path_known = 1;
    }
    if (!path || !*path)
        return;

    int live_mode = getenv("FP2_SCREENSHOT_LIVE") &&
                    strcmp(getenv("FP2_SCREENSHOT_LIVE"), "0") != 0;
    int live_request = live_mode && access("/tmp/bcshot", F_OK) == 0;
    if (live_mode) {
        if (!live_request)
            return;
        unlink("/tmp/bcshot");
    } else if (finished) {
        return;
    }

    if (!live_mode) {
        long wanted = 300;
        const char *value = getenv("FP2_SCREENSHOT_FRAME");
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
    void (*pixel_store)(GLenum, GLint) = gl_raw("glPixelStorei");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) = gl_raw("glReadPixels");
    if (!get_int || !pixel_store || !read_pixels) {
        nx_log("screenshot: required GLES functions are missing");
        return;
    }

    /* Unity leaves whatever offscreen FBO it used last bound when the frame
     * ends; reading that instead of the presented buffer reports a false
     * black screen.  Bind the default framebuffer for the read and restore
     * the game's binding afterwards. */
    void (*bind_fb)(GLenum, GLuint) = gl_raw("glBindFramebuffer");
    GLint prev_fb = 0;
    get_int(GL_FRAMEBUFFER_BINDING, &prev_fb);
    if (bind_fb && prev_fb != 0)
        bind_fb(GL_FRAMEBUFFER, 0);

    GLint viewport[4] = { 0, 0, 0, 0 };
    get_int(GL_VIEWPORT, viewport);
    int width = viewport[2];
    int height = viewport[3];
    if (width <= 0 || height <= 0) {
        unsigned short *win = (unsigned short *)fp2_native_window();
        if (win) { width = win[0]; height = win[1]; }
    }
    nx_log("screenshot: fbo at swap = %d (reading 0), viewport %dx%d",
           prev_fb, width, height);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        nx_log("screenshot: invalid viewport %dx%d", width, height);
        return;
    }

    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / 4) {
        nx_log("screenshot: viewport size overflow");
        return;
    }
    unsigned char *rgba = malloc(pixels * 4);
    unsigned char *rgb = malloc(pixels * 3);
    if (!rgba || !rgb) {
        nx_log("screenshot: allocation failed");
        free(rgb);
        free(rgba);
        return;
    }

    GLint old_pack = 4;
    get_int(GL_PACK_ALIGNMENT, &old_pack);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    pixel_store(GL_PACK_ALIGNMENT, old_pack);
    if (bind_fb && prev_fb != 0)
        bind_fb(GL_FRAMEBUFFER, (GLuint)prev_fb);

    size_t nonblack = 0;
    size_t alpha_zero = 0;
    for (int y = 0; y < height; y++) {
        const unsigned char *source =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
        unsigned char *dest = rgb + (size_t)y * (size_t)width * 3;
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

    FILE *output = fopen(path, "wb");
    if (!output) {
        nx_log("screenshot: cannot open %s", path);
    } else {
        fprintf(output, "P6\n%d %d\n255\n", width, height);
        size_t written = fwrite(rgb, 3, pixels, output);
        if (fclose(output) == 0 && written == pixels) {
            nx_log("screenshot: frame %lu -> %s (%zu/%zu nonblack, "
                   "%zu alpha-zero)",
                   frame, path, nonblack, pixels, alpha_zero);
        } else {
            nx_log("screenshot: incomplete write to %s", path);
        }
    }
    free(rgb);
    free(rgba);
#endif
}

static EGLBoolean my_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    /* O contador de swap vive AQUI, nao dentro da captura: no caminho em que a
     * SDL e' dona do video a captura nem e' chamada, e o contador ficava parado
     * em zero -- por isso `FP2_FBO_CAPTURE_FRAME` e a medicao por FBO nunca
     * disparavam nesse caminho. */
    ++fp2_swap_count;
    draw_gamepad_keyboard();
    draw_gamepad_cursor();
    /* O FBO 0 e' o unico cujo conteudo ainda esta la' na hora do swap; medir
     * aqui, ANTES do present, e nao depois. */
    fp2_sample_current_fbo("antes-do-swap", gl_state.framebuffer);
    if (fp2_sdl_video_active())
        return fp2_sdl_swap_buffers(display, surface);
    capture_raw_frame_if_requested();
    force_opaque_backbuffer();
    if (!p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");
    EGLint width = 0, height = 0;
    static EGLBoolean (*query_surface)(EGLDisplay, EGLSurface, EGLint,
                                       EGLint *);
    if (!query_surface)
        query_surface = sys("eglQuerySurface");
    if (query_surface) {
        if (!query_surface(display, surface, EGL_WIDTH, &width))
            width = 0;
        if (!query_surface(display, surface, EGL_HEIGHT, &height))
            height = 0;
    }
    /* force_opaque_backbuffer is part of the actual image presented by the
     * Amlogic compositor, so proof must sample after it and immediately before
     * the real raw-EGL swap.  Passing zero dimensions safely falls back to the
     * current GL viewport inside the canonical adapter. */
    nxgl_frame_proof_before_present(width, height);
    return p_eglSwapBuffers(display, surface);
}

static void *my_eglGetProcAddress(const char *name)
{
#ifndef FP2_RELEASE_BUILD
    static int trace = -1;
    static unsigned long lookup_count;
#endif
    void *address;

    if (!name)
        return NULL;
    if (name[0] == 'g' && name[1] == 'l') {
        address = fp2_gl_sym(name);
    } else if (fp2_sdl_video_active()) {
        address = fp2_sdl_egl_proc(name);
    } else {
        static void *(*real)(const char *);
        if (!real)
            real = sys("eglGetProcAddress");
        address = real ? real(name) : NULL;
    }

#ifndef FP2_RELEASE_BUILD
    if (trace < 0)
        trace = fp2_env_flag("FP2_GLPROC_TRACE");
    lookup_count++;
    if (trace)
        fprintf(stderr, "[fp2/glproc] #%lu %s -> %p%s\n",
                lookup_count, name, address, address ? "" : " MISSING");
#endif
    return address;
}

/* --- everything else is a straight forward ------------------------------ */

/* A trampoline table beats writing 20 wrappers: the calls we do not rewrite
 * are resolved to the system symbol directly at start-up. */
static const char *const passthrough[] = {
    "eglInitialize", "eglTerminate", "eglGetConfigs",
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

/* Which implementation the wrappers must call underneath.  Bomb Chicken bound
 * eglChooseConfig/eglCreateContext straight to SDL whenever SDL owned the
 * video, which silently bypassed the ES3->ES2 rewriting below.  Unity 2022
 * never noticed because it was happy with a GLES2 device; Unity 6 always asks
 * for an ES3 config and the driver answers "no configs", which surfaces as
 * "[EGL] Unable to find a configuration matching minimum spec!" and then
 * "Graphics device is null.".  The wrappers now stay in the path in both
 * ownership modes and only their underlying resolver changes. */
static int g_sdl_owned;

static void *egl_real(const char *name)
{
    return g_sdl_owned ? fp2_sdl_egl_proc(name) : sys(name);
}


/* Unity asks eglChooseConfig for an ES3-capable config; we strip the ES3 bit so
 * the Utgard driver can answer at all.  Unity then VERIFIES the config it got
 * by reading EGL_RENDERABLE_TYPE back, sees no ES3 bit, rejects every
 * candidate and reports "[EGL] Unable to find a configuration matching minimum
 * spec!".  The lie has to be consistent on the way out too. */
static EGLBoolean (*p_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint,
                                          EGLint *);

static EGLBoolean my_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg,
                                        EGLint attr, EGLint *value)
{
    if (!p_eglGetConfigAttrib)
        p_eglGetConfigAttrib = egl_real("eglGetConfigAttrib");
    EGLBoolean ok = p_eglGetConfigAttrib(dpy, cfg, attr, value);
    if (ok && value && attr == EGL_RENDERABLE_TYPE &&
        !(*value & EGL_OPENGL_ES3_BIT_KHR)) {
        *value |= EGL_OPENGL_ES3_BIT_KHR;
        nx_log("eglGetConfigAttrib: RENDERABLE_TYPE reportado com ES3");
    }
    return ok;
}

void fp2_egl_init(void)
{
    /* SDL owns KMS/Wayland contexts; Mali fbdev stays on the vendor EGL path,
     * matching the validated Horizon Chase backend split. */
#ifdef FP2_RELEASE_BUILD
    /* The public artifact has no capture/raw ownership mode: its physical
     * proof and post-present contract are both bound to the SDL window. */
    int sdl_owned = fp2_sdl_video_init();
#else
    int sdl_owned = fp2_capture_mode ? 0 : fp2_sdl_video_init();
#endif
    g_sdl_owned = sdl_owned;
    tab_n = 0;
    /* Always our wrappers: they are what turns an ES3 request into something
     * the Utgard driver can actually answer. */
    tab[tab_n++] = (nx_import){ "eglChooseConfig",  (void *)my_eglChooseConfig };
    tab[tab_n++] = (nx_import){ "eglCreateContext", (void *)my_eglCreateContext };
    tab[tab_n++] = (nx_import){ "eglGetConfigAttrib",
        (void *)my_eglGetConfigAttrib };
    tab[tab_n++] = (nx_import){ "eglCreateWindowSurface",
        sdl_owned ? fp2_sdl_egl_proc("eglCreateWindowSurface")
                  : (void *)my_eglCreateWindowSurface };
    tab[tab_n++] = (nx_import){ "eglGetDisplay",
        sdl_owned ? fp2_sdl_egl_proc("eglGetDisplay")
                  : (void *)my_eglGetDisplay };
    tab[tab_n++] = (nx_import){ "eglSwapBuffers",        (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",     (void *)my_eglGetProcAddress };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        void *f = sdl_owned ? fp2_sdl_egl_proc(passthrough[i])
                            : dlsym(RTLD_DEFAULT, passthrough[i]);
        if (!f && !sdl_owned)
            f = sys(passthrough[i]);
        if (f)
            tab[tab_n++] = (nx_import){ passthrough[i], f };
    }
    nx_log("egl table: %zu entries (%s ownership)", tab_n,
           sdl_owned ? "SDL" : "raw");
}

const nx_import *fp2_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *fp2_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return fp2_gl_sym(name);
}
