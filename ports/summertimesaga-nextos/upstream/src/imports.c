
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_GENERATE_MIPMAP_HINT
#define GL_GENERATE_MIPMAP_HINT 0x8192
#endif

#include "egl_shim.h"
#include "etc1.h"
#include "imports.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#endif

#undef feof
#undef ferror

static uint8_t fake_sF[3][0x100];
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

extern void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
extern void glColorPointer(GLint size, GLenum type, GLsizei stride,
                           const void *pointer);
extern void glDisableClientState(GLenum array);
extern void glEnableClientState(GLenum array);
extern void glLoadIdentity(void);
extern void glMatrixMode(GLenum mode);
extern void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top,
                     GLfloat near_val, GLfloat far_val);
extern void glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                              const void *pointer);
extern void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
extern void glVertexPointer(GLint size, GLenum type, GLsizei stride,
                            const void *pointer);

static unsigned dbg_gl_clear_count, dbg_gl_draw_arrays_count;
static unsigned dbg_gl_draw_elements_count, dbg_gl_viewport_count;
static unsigned dbg_gl_bindfb_count;
static unsigned dbg_uint_index_fix_count, dbg_uint_index_fail_count;
static unsigned dbg_attr_pointer_count, dbg_matrix4_count;
static unsigned dbg_teximage_count, dbg_texsubimage_count, dbg_teximage_error_count;
static GLenum dbg_last_draw_mode, dbg_last_draw_type, dbg_last_draw_error;
static GLsizei dbg_last_draw_count;
static GLint dbg_last_draw_ebo;
static GLuint dbg_current_program;
static GLint dbg_last_matrix4_location = -1;
static GLfloat dbg_last_matrix4[16];
static GLsizei dbg_last_tex_w, dbg_last_tex_h;
static GLenum dbg_last_tex_internal, dbg_last_tex_format, dbg_last_tex_type;
static GLenum dbg_last_tex_error;
static GLfloat dbg_clear_color[4];

/* glDrawTexfOES is a legacy GLES1 extension.  The Android Ren'Py library
 * imports it even though the current GLES2 renderer does not normally call
 * it.  Some Mesa/Panfrost stacks (notably ROCKNIX) intentionally omit the
 * symbol, so taking its address directly made the loader fail before main().
 * Resolve it only if the active driver provides it; otherwise keep the import
 * valid and report the first unexpected call instead of aborting at startup. */
typedef void (*SSGlDrawTexfOES)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);

static void wrap_glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat width,
                               GLfloat height) {
  static SSGlDrawTexfOES real_draw_tex;
  static int resolved;
  static int warned;

  if (!resolved) {
    resolved = 1;
    real_draw_tex = (SSGlDrawTexfOES)dlsym(RTLD_DEFAULT, "glDrawTexfOES");
    if (!real_draw_tex)
      real_draw_tex =
          (SSGlDrawTexfOES)SDL_GL_GetProcAddress("glDrawTexfOES");
  }

  if (real_draw_tex) {
    real_draw_tex(x, y, z, width, height);
    return;
  }

  if (!warned) {
    warned = 1;
    debugPrintf("[GL] glDrawTexfOES ausente; chamada legada ignorada\n");
  }
}

typedef struct {
  int valid;
  GLint size;
  GLenum type;
  GLboolean normalized;
  GLsizei stride;
  const void *pointer;
  GLint array_buffer;
  GLfloat sample[8];
} DebugAttribState;

#define DEBUG_ATTR_CAP 16
static DebugAttribState dbg_attrib[DEBUG_ATTR_CAP];

typedef struct {
  GLuint original;
  GLuint shadow;
  GLsizeiptr original_size;
  GLsizeiptr shadow_size;
  int valid;
} IndexShadowBuffer;

#define INDEX_SHADOW_CAP 4096
static IndexShadowBuffer index_shadows[INDEX_SHADOW_CAP];
static GLuint bound_element_array_buffer;

typedef struct {
  GLuint buffer;
  uint8_t *data;
  GLsizeiptr size;
} PixelUnpackBuffer;

#define PBO_EMU_CAP 256
static PixelUnpackBuffer pbo_emu[PBO_EMU_CAP];
static GLuint bound_pixel_unpack_buffer;
static GLint unpack_row_length;

static IndexShadowBuffer *index_shadow_find(GLuint original, int create) {
  if (!original)
    return NULL;

  IndexShadowBuffer *empty = NULL;
  for (size_t i = 0; i < INDEX_SHADOW_CAP; i++) {
    if (index_shadows[i].original == original)
      return &index_shadows[i];
    if (!index_shadows[i].original && !empty)
      empty = &index_shadows[i];
  }

  if (!create || !empty)
    return NULL;

  empty->original = original;
  empty->shadow = 0;
  empty->original_size = 0;
  empty->shadow_size = 0;
  empty->valid = 0;
  return empty;
}

static void index_shadow_delete(GLuint original) {
  IndexShadowBuffer *entry = index_shadow_find(original, 0);
  if (!entry)
    return;

  if (entry->shadow)
    glDeleteBuffers(1, &entry->shadow);

  memset(entry, 0, sizeof(*entry));
}

static PixelUnpackBuffer *pbo_find(GLuint buffer, int create) {
  if (!buffer)
    return NULL;

  PixelUnpackBuffer *empty = NULL;
  for (size_t i = 0; i < PBO_EMU_CAP; i++) {
    if (pbo_emu[i].buffer == buffer)
      return &pbo_emu[i];
    if (!pbo_emu[i].buffer && !empty)
      empty = &pbo_emu[i];
  }

  if (!create || !empty)
    return NULL;

  empty->buffer = buffer;
  empty->data = NULL;
  empty->size = 0;
  return empty;
}

static void pbo_delete(GLuint buffer) {
  PixelUnpackBuffer *entry = pbo_find(buffer, 0);
  if (!entry)
    return;

  free(entry->data);
  memset(entry, 0, sizeof(*entry));
}

static int is_mipmap_filter(GLint param) {
  return param == GL_NEAREST_MIPMAP_NEAREST ||
         param == GL_LINEAR_MIPMAP_NEAREST ||
         param == GL_NEAREST_MIPMAP_LINEAR ||
         param == GL_LINEAR_MIPMAP_LINEAR;
}

static int convert_uint_indices(const void *data, GLsizeiptr size,
                                uint16_t **out, GLsizeiptr *out_size) {
  if (!data || size <= 0 || (size % (GLsizeiptr)sizeof(uint32_t)) != 0)
    return 0;

  size_t count = (size_t)(size / (GLsizeiptr)sizeof(uint32_t));
  if (count > SIZE_MAX / sizeof(uint16_t))
    return 0;

  uint16_t *converted = malloc(count * sizeof(uint16_t));
  if (!converted)
    return 0;

  const uint32_t *src = (const uint32_t *)data;
  for (size_t i = 0; i < count; i++) {
    if (src[i] > 0xffffu) {
      free(converted);
      return 0;
    }
    converted[i] = (uint16_t)src[i];
  }

  *out = converted;
  *out_size = (GLsizeiptr)(count * sizeof(uint16_t));
  return 1;
}

static void upload_index_shadow(IndexShadowBuffer *entry, GLintptr offset,
                                GLsizeiptr size, const void *data,
                                GLenum usage, int replace_all) {
  if (!entry)
    return;

  if (offset < 0 || size < 0 || (offset % 4) != 0 || (size % 4) != 0) {
    entry->valid = 0;
    return;
  }

  if (!entry->shadow)
    glGenBuffers(1, &entry->shadow);
  if (!entry->shadow) {
    entry->valid = 0;
    return;
  }

  GLsizeiptr shadow_offset = offset / 2;
  GLsizeiptr shadow_size = size / 2;
  GLint previous = 0;
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previous);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, entry->shadow);

  if (!data) {
    if (replace_all) {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, shadow_size, NULL, usage);
      entry->shadow_size = shadow_size;
      entry->valid = 1;
    } else {
      entry->valid = 0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)previous);
    return;
  }

  uint16_t *converted = NULL;
  GLsizeiptr converted_size = 0;
  if (!convert_uint_indices(data, size, &converted, &converted_size)) {
    entry->valid = 0;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)previous);
    return;
  }

  if (replace_all || entry->shadow_size < shadow_offset + converted_size) {
    GLsizeiptr alloc_size = replace_all ? converted_size : shadow_offset + converted_size;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, alloc_size, NULL, usage);
    entry->shadow_size = alloc_size;
  }
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, shadow_offset, converted_size, converted);
  entry->valid = 1;

  free(converted);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)previous);
}

static void wrap_glClearColor(GLfloat red, GLfloat green, GLfloat blue,
                              GLfloat alpha) {
  dbg_clear_color[0] = red;
  dbg_clear_color[1] = green;
  dbg_clear_color[2] = blue;
  dbg_clear_color[3] = alpha;
  glClearColor(red, green, blue, alpha);
}

static void wrap_glClear(GLbitfield mask) {
  dbg_gl_clear_count++;
  glClear(mask);
}

static void wrap_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  dbg_gl_viewport_count++;
  glViewport(x, y, width, height);
}

static void wrap_glUseProgram(GLuint program) {
  dbg_current_program = program;
  glUseProgram(program);
}

static void wrap_glBindFramebuffer(GLenum target, GLuint framebuffer) {
  dbg_gl_bindfb_count++;
  glBindFramebuffer(target, framebuffer);
}

static void wrap_glBindBuffer(GLenum target, GLuint buffer) {
  if (target == GL_ELEMENT_ARRAY_BUFFER) {
    bound_element_array_buffer = buffer;
  } else if (target == GL_PIXEL_UNPACK_BUFFER) {
    bound_pixel_unpack_buffer = buffer;
    return;
  }
  glBindBuffer(target, buffer);
}

static void wrap_glBufferData(GLenum target, GLsizeiptr size, const void *data,
                              GLenum usage) {
  if (target == GL_PIXEL_UNPACK_BUFFER) {
    PixelUnpackBuffer *entry = pbo_find(bound_pixel_unpack_buffer, 1);
    if (!entry)
      return;

    uint8_t *copy = NULL;
    if (size > 0) {
      copy = malloc((size_t)size);
      if (copy && data)
        memcpy(copy, data, (size_t)size);
      else if (copy)
        memset(copy, 0, (size_t)size);
    }

    free(entry->data);
    entry->data = copy;
    entry->size = copy ? size : 0;
    return;
  }

  GLuint original_ebo = bound_element_array_buffer;
  glBufferData(target, size, data, usage);

  if (target == GL_ELEMENT_ARRAY_BUFFER && original_ebo && size > 0) {
    IndexShadowBuffer *entry = index_shadow_find(original_ebo, 1);
    if (entry) {
      entry->original_size = size;
      entry->valid = 0;
      upload_index_shadow(entry, 0, size, data, usage, 1);
    }
  }
}

static void wrap_glBufferSubData(GLenum target, GLintptr offset,
                                 GLsizeiptr size, const void *data) {
  if (target == GL_PIXEL_UNPACK_BUFFER) {
    PixelUnpackBuffer *entry = pbo_find(bound_pixel_unpack_buffer, 0);
    if (!entry || !entry->data || !data || offset < 0 || size < 0)
      return;
    if (offset + size > entry->size)
      return;
    memcpy(entry->data + offset, data, (size_t)size);
    return;
  }

  GLuint original_ebo = bound_element_array_buffer;
  glBufferSubData(target, offset, size, data);

  if (target == GL_ELEMENT_ARRAY_BUFFER && original_ebo && size > 0 && data) {
    IndexShadowBuffer *entry = index_shadow_find(original_ebo, 0);
    if (entry)
      upload_index_shadow(entry, offset, size, data, GL_STATIC_DRAW, 0);
  }
}

static void wrap_glDeleteBuffers(GLsizei n, const GLuint *buffers) {
  if (buffers) {
    for (GLsizei i = 0; i < n; i++) {
      index_shadow_delete(buffers[i]);
      pbo_delete(buffers[i]);
    }
  }
  glDeleteBuffers(n, buffers);
}

static GLint g_unpack_alignment = 4;

static void wrap_glPixelStorei(GLenum pname, GLint param) {
  if (pname == GL_UNPACK_ROW_LENGTH) {
    unpack_row_length = param;
    return;
  }
  if (pname == GL_UNPACK_ALIGNMENT)
    g_unpack_alignment = param;
  glPixelStorei(pname, param);
}

/* ======================================================================
 * Texturas 16-bit (SUMMERTIME_TEX16, default ON): converte uploads RGBA8888
 * grandes para RGB565 (opacas) / RGBA4444 (com alpha) no glTexImage2D.
 * Metade da VRAM/banda de textura — e no Mali-450 VRAM = RAM do sistema.
 * Rastreia o formato por textura para converter tambem os TexSubImage
 * (GLES2 exige formato do SubImage == formato da textura).
 * ====================================================================== */
#define TEX16_MAX_IDS 65536
static uint8_t g_tex16_fmt[TEX16_MAX_IDS]; /* 0=8888, 1=565, 2=4444 */
/* downscale real por textura: 0 = sem scale; senao num/den = dst/orig */
static uint16_t g_texds_num[TEX16_MAX_IDS], g_texds_den[TEX16_MAX_IDS];
static GLuint g_bound_tex2d;
static unsigned g_tex16_count565, g_tex16_count4444, g_texds_count;

/* Downscale RGBA8888 por box-filter (media de bloco). UVs sao normalizadas,
 * entao encolher a TEXTURA nao mexe no layout — so na resolucao (igual
 * mipmap). E o downscale REAL que o max_texture_size do Ren'Py nao faz
 * (ele fatia em tiles em vez de encolher — visto no log: upload 1920x1080
 * inteiro mesmo com cap 1280). */
static uint8_t *texds_scale(const uint8_t *src, GLsizei w, GLsizei h,
                            size_t src_stride, GLsizei dw, GLsizei dh) {
  uint8_t *dst = malloc((size_t)dw * dh * 4u);
  if (!dst)
    return NULL;
  for (GLsizei y = 0; y < dh; y++) {
    GLsizei sy0 = (GLsizei)((int64_t)y * h / dh);
    GLsizei sy1 = (GLsizei)((int64_t)(y + 1) * h / dh);
    if (sy1 <= sy0)
      sy1 = sy0 + 1;
    for (GLsizei x = 0; x < dw; x++) {
      GLsizei sx0 = (GLsizei)((int64_t)x * w / dw);
      GLsizei sx1 = (GLsizei)((int64_t)(x + 1) * w / dw);
      if (sx1 <= sx0)
        sx1 = sx0 + 1;
      unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
      for (GLsizei yy = sy0; yy < sy1; yy++) {
        const uint8_t *row = src + (size_t)yy * src_stride;
        for (GLsizei xx = sx0; xx < sx1; xx++) {
          const uint8_t *p = row + (size_t)xx * 4u;
          r += p[0]; g += p[1]; b += p[2]; a += p[3];
          n++;
        }
      }
      uint8_t *d = dst + ((size_t)y * dw + x) * 4u;
      d[0] = (uint8_t)(r / n);
      d[1] = (uint8_t)(g / n);
      d[2] = (uint8_t)(b / n);
      d[3] = (uint8_t)(a / n);
    }
  }
  return dst;
}

static int texds_max(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("SUMMERTIME_TEXDS_MAX");
    v = e && *e ? atoi(e) : 1280;
    if (v < 720)
      v = 720;
  }
  return v;
}

static int tex16_enabled(void) {
  static int en = -1;
  if (en < 0) {
    const char *v = getenv("SUMMERTIME_TEX16");
    en = !(v && strcmp(v, "0") == 0);
  }
  return en;
}

/* ======================================================================
 * ETC1 dupla-camada (SUMMERTIME_ETC1, default ON):
 * Imagens do jogo chegam aqui pelo caminho premultiplied (im.py troca o
 * Texture.load e avisa via ss_tex_hint). Para essas:
 *   - premultiplica o alpha em C (o dado vem reto do PNG/WebP);
 *   - RGB vira ETC1 (4bpp) via glCompressedTexImage2D;
 *   - se tem alpha, o canal A vira uma SEGUNDA textura ETC1 (cinza) — a
 *     "gemea" — e o fragment shader (remendado no glShaderSource) troca
 *     c.a pela amostra da gemea quando u_ss_dualN=1 (setado no draw).
 * RGBA8888 32bpp -> 8bpp com alpha, 4bpp opaco. No Mali-450 textura mora
 * na RAM do sistema, entao isso corta RAM de verdade.
 * ====================================================================== */

#define SS_MAX_UNITS 16
#define SS_PROG_CAP 512

static uint8_t g_tex_kind[TEX16_MAX_IDS]; /* 0=normal 1=etc1 2=etc1+gemea */
static GLuint g_tex_twin[TEX16_MAX_IDS];
static GLuint g_unit_tex[SS_MAX_UNITS];
static int g_active_unit;
static int g_ss_hint;
static unsigned g_etc1_count, g_etc1_dual_count, g_etc1_sub_skips;
static unsigned g_shader_patch_count;

/* exportado (-rdynamic) pro Python chamar via ctypes.CDLL(None) */
void ss_tex_hint(int mode) { g_ss_hint = mode; }

static int ss_etc1_enabled(void) {
  static int en = -1;
  if (en < 0) {
    const char *v = getenv("SUMMERTIME_ETC1");
    en = !(v && strcmp(v, "0") == 0);
  }
  return en;
}

static int ss_etc1_min(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("SUMMERTIME_ETC1_MIN");
    v = e && *e ? atoi(e) : 64;
    if (v < 8)
      v = 8;
  }
  return v;
}

/* copia com stride colapsado + premultiply; diz se ha alpha real */
static uint8_t *ss_premul_copy(const uint8_t *src, GLsizei w, GLsizei h,
                               size_t src_stride, int *has_alpha_out) {
  uint8_t *dst = malloc((size_t)w * h * 4u);
  if (!dst)
    return NULL;
  int has_alpha = 0;
  for (GLsizei y = 0; y < h; y++) {
    const uint8_t *row = src + (size_t)y * src_stride;
    uint8_t *drow = dst + (size_t)y * w * 4u;
    for (GLsizei x = 0; x < w; x++) {
      const uint8_t *p = row + (size_t)x * 4u;
      uint8_t *d = drow + (size_t)x * 4u;
      unsigned a = p[3];
      if (a == 255) {
        d[0] = p[0];
        d[1] = p[1];
        d[2] = p[2];
      } else {
        has_alpha = 1;
        d[0] = (uint8_t)((p[0] * a + 127) / 255);
        d[1] = (uint8_t)((p[1] * a + 127) / 255);
        d[2] = (uint8_t)((p[2] * a + 127) / 255);
      }
      d[3] = (uint8_t)a;
    }
  }
  *has_alpha_out = has_alpha;
  return dst;
}

/* apaga a gemea e volta o registro pro estado normal */
static void ss_tex_reset(GLuint tex) {
  if (tex >= TEX16_MAX_IDS)
    return;
  if (g_tex_twin[tex]) {
    GLuint twin = g_tex_twin[tex];
    glDeleteTextures(1, &twin);
    for (int u = 0; u < SS_MAX_UNITS; u++)
      if (g_unit_tex[u] == twin)
        g_unit_tex[u] = 0;
    g_tex_twin[tex] = 0;
  }
  g_tex_kind[tex] = 0;
}

/* sobe ETC1 (e a gemea de alpha, se precisar) pra textura ligada agora */
static int ss_etc1_upload(GLsizei w, GLsizei h, const uint8_t *premul,
                          int has_alpha) {
  size_t sz = ss_etc1_size(w, h);
  uint8_t *enc = malloc(sz);
  if (!enc)
    return 0;

  ss_etc1_encode_rgba(premul, w, h, (size_t)w * 4u, enc);
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, w, h, 0,
                         (GLsizei)sz, enc);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    /* driver recusou (sem extensao?) — cai fora e deixa o caller usar 16-bit */
    free(enc);
    dbg_teximage_error_count++;
    dbg_last_tex_error = err;
    return 0;
  }

  GLuint orig = g_bound_tex2d;
  if (has_alpha && orig < TEX16_MAX_IDS) {
    GLuint twin = 0;
    glGenTextures(1, &twin);
    if (twin) {
      ss_etc1_encode_alpha(premul, w, h, (size_t)w * 4u, enc);
      glBindTexture(GL_TEXTURE_2D, twin);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, w, h, 0,
                             (GLsizei)sz, enc);
      glBindTexture(GL_TEXTURE_2D, orig);
      if (glGetError() == GL_NO_ERROR) {
        g_tex_twin[orig] = twin;
        g_tex_kind[orig] = 2;
        g_etc1_dual_count++;
      } else {
        glDeleteTextures(1, &twin);
        g_tex_kind[orig] = 1; /* RGB ficou; alpha vira 1.0 — melhor que nada */
      }
    }
  } else if (orig < TEX16_MAX_IDS) {
    g_tex_kind[orig] = 1;
    g_etc1_count++;
  }

  free(enc);
  if ((g_etc1_count + g_etc1_dual_count) <= 4)
    debugPrintf("etc1: #%u %dx%d %s\n", g_etc1_count + g_etc1_dual_count, w, h,
                has_alpha ? "dual" : "opaque");
  return 1;
}

/* --- programas: sampler->unidade e uniforms do remendo ------------------- */

typedef struct {
  GLuint prog;
  GLint loc_tex[3];
  GLint loc_dual[3];
  GLint loc_atex[3];
  int unit_tex[3];
  int patched;
} SSProgInfo;

static SSProgInfo g_prog[SS_PROG_CAP];

static SSProgInfo *ss_prog_find(GLuint prog, int create) {
  if (!prog)
    return NULL;
  SSProgInfo *empty = NULL;
  for (int i = 0; i < SS_PROG_CAP; i++) {
    if (g_prog[i].prog == prog)
      return &g_prog[i];
    if (!g_prog[i].prog && !empty)
      empty = &g_prog[i];
  }
  if (!create || !empty)
    return NULL;
  memset(empty, 0, sizeof(*empty));
  empty->prog = prog;
  return empty;
}

static void wrap_glLinkProgram(GLuint prog) {
  glLinkProgram(prog);
  SSProgInfo *pi = ss_prog_find(prog, 1);
  if (!pi)
    return;
  static const char *tex_names[3] = {"tex0", "tex1", "tex2"};
  static const char *dual_names[3] = {"u_ss_dual0", "u_ss_dual1", "u_ss_dual2"};
  static const char *atex_names[3] = {"u_ss_atex0", "u_ss_atex1", "u_ss_atex2"};
  pi->patched = 0;
  for (int s = 0; s < 3; s++) {
    pi->loc_tex[s] = glGetUniformLocation(prog, tex_names[s]);
    pi->loc_dual[s] = glGetUniformLocation(prog, dual_names[s]);
    pi->loc_atex[s] = glGetUniformLocation(prog, atex_names[s]);
    pi->unit_tex[s] = (s < SS_MAX_UNITS) ? s : 0; /* palpite ate o Uniform1i */
    if (pi->loc_dual[s] >= 0)
      pi->patched = 1;
  }
}

static void wrap_glDeleteProgram(GLuint prog) {
  SSProgInfo *pi = ss_prog_find(prog, 0);
  if (pi)
    memset(pi, 0, sizeof(*pi));
  glDeleteProgram(prog);
}

static void wrap_glUniform1i(GLint location, GLint v0) {
  SSProgInfo *pi = ss_prog_find(dbg_current_program, 0);
  if (pi && location >= 0) {
    for (int s = 0; s < 3; s++)
      if (pi->loc_tex[s] == location)
        pi->unit_tex[s] = v0;
  }
  glUniform1i(location, v0);
}

static void wrap_glActiveTexture(GLenum unit) {
  int idx = (int)(unit - GL_TEXTURE0);
  if (idx >= 0 && idx < SS_MAX_UNITS)
    g_active_unit = idx;
  glActiveTexture(unit);
}

static int ss_alpha_unit(int s) {
  static GLint max_units = 0;
  if (!max_units) {
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_units);
    if (max_units < 8 || max_units > SS_MAX_UNITS)
      max_units = 8;
  }
  return max_units - 1 - s;
}

/* antes de cada draw: liga u_ss_dualN e a gemea pro que estiver em texN */
static void ss_predraw(void) {
  SSProgInfo *pi = ss_prog_find(dbg_current_program, 0);
  if (!pi || !pi->patched)
    return;
  int restore = 0;
  for (int s = 0; s < 3; s++) {
    if (pi->loc_dual[s] < 0)
      continue;
    int unit = pi->unit_tex[s];
    GLuint tex = (unit >= 0 && unit < SS_MAX_UNITS) ? g_unit_tex[unit] : 0;
    int dual = (tex && tex < TEX16_MAX_IDS && g_tex_kind[tex] == 2 &&
                g_tex_twin[tex]);
    glUniform1f(pi->loc_dual[s], dual ? 1.0f : 0.0f);
    if (dual) {
      int aunit = ss_alpha_unit(s);
      glActiveTexture((GLenum)(GL_TEXTURE0 + aunit));
      glBindTexture(GL_TEXTURE_2D, g_tex_twin[tex]);
      g_unit_tex[aunit] = g_tex_twin[tex];
      if (pi->loc_atex[s] >= 0)
        glUniform1i(pi->loc_atex[s], aunit);
      restore = 1;
    }
  }
  if (restore)
    glActiveTexture((GLenum)(GL_TEXTURE0 + g_active_unit));
}

/* --- remendo do fragment shader ------------------------------------------ */

static const char SS_FRAG_PRELUDE[] =
    "uniform sampler2D u_ss_atex0;\nuniform mediump float u_ss_dual0;\n"
    "uniform sampler2D u_ss_atex1;\nuniform mediump float u_ss_dual1;\n"
    "uniform sampler2D u_ss_atex2;\nuniform mediump float u_ss_dual2;\n"
    "vec4 ss_fix0(vec4 c, vec2 uv){ if (u_ss_dual0 > 0.5) c.a = "
    "texture2D(u_ss_atex0, uv).g; return c; }\n"
    "vec4 ss_fix1(vec4 c, vec2 uv){ if (u_ss_dual1 > 0.5) c.a = "
    "texture2D(u_ss_atex1, uv).g; return c; }\n"
    "vec4 ss_fix2(vec4 c, vec2 uv){ if (u_ss_dual2 > 0.5) c.a = "
    "texture2D(u_ss_atex2, uv).g; return c; }\n"
    "vec4 ss_tex0(sampler2D t, vec2 uv){ return ss_fix0(texture2D(t, uv), uv); }\n"
    "vec4 ss_tex0(sampler2D t, vec2 uv, float b){ return ss_fix0(texture2D(t, "
    "uv, b), uv); }\n"
    "vec4 ss_tex1(sampler2D t, vec2 uv){ return ss_fix1(texture2D(t, uv), uv); }\n"
    "vec4 ss_tex1(sampler2D t, vec2 uv, float b){ return ss_fix1(texture2D(t, "
    "uv, b), uv); }\n"
    "vec4 ss_tex2(sampler2D t, vec2 uv){ return ss_fix2(texture2D(t, uv), uv); }\n"
    "vec4 ss_tex2(sampler2D t, vec2 uv, float b){ return ss_fix2(texture2D(t, "
    "uv, b), uv); }\n";

/* troca todo "texture2D(texN," por "ss_texN(texN," (mesma assinatura) */
static char *ss_patch_fragment(const char *src) {
  if (!strstr(src, "gl_FragColor"))
    return NULL;
  static const char *pats[3] = {"texture2D(tex0,", "texture2D(tex1,",
                                "texture2D(tex2,"};
  static const char *reps[3] = {"ss_tex0(tex0,", "ss_tex1(tex1,",
                                "ss_tex2(tex2,"};
  int found = 0;
  for (int i = 0; i < 3; i++)
    if (strstr(src, pats[i]))
      found = 1;
  if (!found)
    return NULL;

  /* insere o preludio depois do bloco de precisao (ou do #version) */
  size_t ins = 0;
  const char *endif = strstr(src, "#endif");
  if (endif && (size_t)(endif - src) < 512) {
    const char *nl = strchr(endif, '\n');
    ins = nl ? (size_t)(nl - src) + 1 : (size_t)(endif - src) + 6;
  } else {
    const char *ver = strstr(src, "#version");
    if (ver) {
      const char *nl = strchr(ver, '\n');
      if (nl)
        ins = (size_t)(nl - src) + 1;
    }
  }

  size_t srclen = strlen(src);
  size_t cap = srclen + sizeof(SS_FRAG_PRELUDE) + 256;
  char *out = malloc(cap);
  if (!out)
    return NULL;

  memcpy(out, src, ins);
  size_t o = ins;
  memcpy(out + o, SS_FRAG_PRELUDE, sizeof(SS_FRAG_PRELUDE) - 1);
  o += sizeof(SS_FRAG_PRELUDE) - 1;

  const char *p = src + ins;
  while (*p) {
    int hit = -1;
    for (int i = 0; i < 3; i++) {
      size_t n = strlen(pats[i]);
      if (strncmp(p, pats[i], n) == 0) {
        hit = i;
        break;
      }
    }
    if (hit >= 0) {
      size_t rn = strlen(reps[hit]);
      if (o + rn + 1 >= cap)
        break;
      memcpy(out + o, reps[hit], rn);
      o += rn;
      p += strlen(pats[hit]);
    } else {
      if (o + 2 >= cap)
        break;
      out[o++] = *p++;
    }
  }
  out[o] = '\0';
  g_shader_patch_count++;
  return out;
}

static void wrap_glShaderSource(GLuint shader, GLsizei count,
                                const GLchar *const *string,
                                const GLint *length) {
  if (!ss_etc1_enabled() || !string || count <= 0) {
    glShaderSource(shader, count, string, length);
    return;
  }

  /* junta as fontes num buffer so (Ren'Py manda 1, mas por via das duvidas) */
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!string[i])
      continue;
    total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
  }
  char *joined = malloc(total + 1);
  if (!joined) {
    glShaderSource(shader, count, string, length);
    return;
  }
  size_t o = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!string[i])
      continue;
    size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    memcpy(joined + o, string[i], n);
    o += n;
  }
  joined[o] = '\0';

  char *patched = ss_patch_fragment(joined);
  const char *final_src = patched ? patched : joined;
  glShaderSource(shader, 1, &final_src, NULL);
  free(patched);
  free(joined);
}

static void wrap_glDeleteTextures(GLsizei n, const GLuint *textures) {
  if (textures) {
    for (GLsizei i = 0; i < n; i++) {
      ss_tex_reset(textures[i]);
      for (int u = 0; u < SS_MAX_UNITS; u++)
        if (g_unit_tex[u] == textures[i])
          g_unit_tex[u] = 0;
    }
  }
  glDeleteTextures(n, textures);
}

static void wrap_glCopyTexImage2D(GLenum target, GLint level,
                                  GLenum internalformat, GLint x, GLint y,
                                  GLsizei width, GLsizei height, GLint border) {
  if (target == GL_TEXTURE_2D && g_bound_tex2d < TEX16_MAX_IDS) {
    ss_tex_reset(g_bound_tex2d);
    g_tex16_fmt[g_bound_tex2d] = 0;
    g_texds_num[g_bound_tex2d] = 0;
    g_texds_den[g_bound_tex2d] = 0;
  }
  glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

static void wrap_glBindTexture(GLenum target, GLuint tex) {
  if (target == GL_TEXTURE_2D) {
    g_bound_tex2d = tex;
    if (g_active_unit >= 0 && g_active_unit < SS_MAX_UNITS)
      g_unit_tex[g_active_unit] = tex;
  }
  glBindTexture(target, tex);
}

/* converte RGBA8888 -> 565/4444; devolve buffer malloc'd (tight). fmt_out:
 * 1=565 (sem alpha real), 2=4444. src_stride em BYTES. */
static uint16_t *tex16_convert(const uint8_t *src, GLsizei w, GLsizei h,
                               size_t src_stride, int *fmt_out) {
  /* passada 1: tem alpha real? */
  int has_alpha = 0;
  for (GLsizei y = 0; y < h && !has_alpha; y++) {
    const uint8_t *row = src + (size_t)y * src_stride;
    for (GLsizei x = 0; x < w; x++) {
      if (row[x * 4 + 3] != 0xFF) {
        has_alpha = 1;
        break;
      }
    }
  }
  uint16_t *dst = malloc((size_t)w * h * 2u);
  if (!dst)
    return NULL;
  for (GLsizei y = 0; y < h; y++) {
    const uint8_t *row = src + (size_t)y * src_stride;
    uint16_t *drow = dst + (size_t)y * w;
    if (has_alpha) {
      for (GLsizei x = 0; x < w; x++) {
        const uint8_t *p = row + x * 4;
        drow[x] = (uint16_t)(((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) |
                             ((p[2] >> 4) << 4) | (p[3] >> 4));
      }
    } else {
      for (GLsizei x = 0; x < w; x++) {
        const uint8_t *p = row + x * 4;
        drow[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) |
                             (p[2] >> 3));
      }
    }
  }
  *fmt_out = has_alpha ? 2 : 1;
  return dst;
}

/* upload de buffer 16-bit tight com alinhamento seguro (linhas w*2 bytes) */
static void tex16_upload(int sub, GLenum target, GLint level, GLint x, GLint y,
                         GLsizei w, GLsizei h, int fmt, const uint16_t *px) {
  GLenum gfmt = (fmt == 1) ? GL_RGB : GL_RGBA;
  GLenum gtype = (fmt == 1) ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_SHORT_4_4_4_4;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
  if (sub)
    glTexSubImage2D(target, level, x, y, w, h, gfmt, gtype, px);
  else
    glTexImage2D(target, level, (GLint)gfmt, w, h, 0, gfmt, gtype, px);
  glPixelStorei(GL_UNPACK_ALIGNMENT, g_unpack_alignment);
}

static void wrap_glTexParameteri(GLenum target, GLenum pname, GLint param) {
  if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MAX_LEVEL)
    return;

  if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER &&
      is_mipmap_filter(param))
    param = GL_LINEAR;

  glTexParameteri(target, pname, param);
}

static void wrap_glGenerateMipmap(GLenum target) {
  (void)target;
}

static void wrap_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                       GLboolean normalized, GLsizei stride,
                                       const void *pointer) {
  dbg_attr_pointer_count++;

  if (index < DEBUG_ATTR_CAP) {
    DebugAttribState *state = &dbg_attrib[index];
    memset(state, 0, sizeof(*state));
    state->valid = 1;
    state->size = size;
    state->type = type;
    state->normalized = normalized;
    state->stride = stride;
    state->pointer = pointer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);

    if (!state->array_buffer && pointer && type == GL_FLOAT && size > 0) {
      const GLfloat *f = (const GLfloat *)pointer;
      int sample_count = size < 8 ? size : 8;
      for (int i = 0; i < sample_count; i++)
        state->sample[i] = f[i];
    }
  }

  glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

static void wrap_glUniformMatrix4fv(GLint location, GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value) {
  dbg_matrix4_count++;
  dbg_last_matrix4_location = location;
  if (value && count > 0) {
    for (int i = 0; i < 16; i++)
      dbg_last_matrix4[i] = value[i];
  }
  glUniformMatrix4fv(location, count, transpose, value);
}

static void wrap_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type,
                              const void *pixels) {
  dbg_teximage_count++;
  dbg_last_tex_w = width;
  dbg_last_tex_h = height;
  dbg_last_tex_internal = (GLenum)internalformat;
  dbg_last_tex_format = format;
  dbg_last_tex_type = type;

  const void *upload_pixels = pixels;
  uint8_t *repacked = NULL;
  PixelUnpackBuffer *pbo = pbo_find(bound_pixel_unpack_buffer, 0);

  if (pbo && pbo->data) {
    uintptr_t offset = (uintptr_t)pixels;
    if (offset < (uintptr_t)pbo->size)
      upload_pixels = pbo->data + offset;
    else
      upload_pixels = NULL;
  }

  if (target == GL_TEXTURE_2D)
    ss_tex_reset(g_bound_tex2d); /* redefiniu a textura: some a gemea antiga */

  /* Caminho ETC1 (imagens do jogo, avisadas pelo im.py via ss_tex_hint):
   * premultiply em C + downscale + ETC1 dupla-camada. */
  if (g_ss_hint && upload_pixels && target == GL_TEXTURE_2D &&
      format == GL_RGBA && type == GL_UNSIGNED_BYTE && width > 0 &&
      height > 0) {
    size_t src_stride =
        (size_t)(unpack_row_length > width ? unpack_row_length : width) * 4u;
    int has_alpha = 0;
    uint8_t *premul = ss_premul_copy((const uint8_t *)upload_pixels, width,
                                     height, src_stride, &has_alpha);
    if (premul) {
      GLsizei up_w = width, up_h = height;
      uint16_t ds_num = 0, ds_den = 0;
      int mx = texds_max();
      GLsizei longest = width > height ? width : height;
      if (longest > mx) {
        if (width >= height) {
          up_w = mx;
          up_h = (GLsizei)((int64_t)height * mx / width);
        } else {
          up_h = mx;
          up_w = (GLsizei)((int64_t)width * mx / height);
        }
        if (up_w < 1) up_w = 1;
        if (up_h < 1) up_h = 1;
        uint8_t *scaled = texds_scale(premul, width, height,
                                      (size_t)width * 4u, up_w, up_h);
        if (scaled) {
          free(premul);
          premul = scaled;
          ds_num = (uint16_t)up_w;
          ds_den = (uint16_t)width;
          g_texds_count++;
        } else {
          up_w = width;
          up_h = height;
        }
      }

      if (g_bound_tex2d < TEX16_MAX_IDS) {
        g_tex16_fmt[g_bound_tex2d] = 0;
        g_texds_num[g_bound_tex2d] = ds_num;
        g_texds_den[g_bound_tex2d] = ds_den;
      }

      int done = 0;
      if (ss_etc1_enabled() && up_w >= ss_etc1_min() && up_h >= ss_etc1_min())
        done = ss_etc1_upload(up_w, up_h, premul, has_alpha);

      if (!done) {
        /* fallback: 16-bit (dado ja premultiplicado, que e o que o caminho
         * premultiplied espera de qualquer jeito) */
        int fmt = 0;
        uint16_t *px16 = tex16_convert(premul, up_w, up_h, (size_t)up_w * 4u,
                                       &fmt);
        if (px16) {
          tex16_upload(0, target, level, 0, 0, up_w, up_h, fmt, px16);
          free(px16);
          if (g_bound_tex2d < TEX16_MAX_IDS)
            g_tex16_fmt[g_bound_tex2d] = (uint8_t)fmt;
          done = 1;
        }
      }

      if (!done)
        glTexImage2D(target, level, internalformat, up_w, up_h, border,
                     format, type, premul);
      free(premul);
      GLenum errh = glGetError();
      if (errh != GL_NO_ERROR) {
        dbg_teximage_error_count++;
        dbg_last_tex_error = errh;
      }
      return;
    }
  }

  /* Pipeline NextOS: (1) downscale REAL p/ <=SUMMERTIME_TEXDS_MAX (800) —
   * o que o max_texture_size do Ren'Py finge fazer; (2) conversao 16-bit.
   * Juntos: bg 1920x1080 8888 (8.3MB) -> 800x450 565 (0.7MB), 12x menos. */
  if (tex16_enabled() && upload_pixels && target == GL_TEXTURE_2D &&
      format == GL_RGBA && type == GL_UNSIGNED_BYTE &&
      width >= 64 && height >= 64) {
    size_t src_stride =
        (size_t)(unpack_row_length > width ? unpack_row_length : width) * 4u;
    const uint8_t *src = (const uint8_t *)upload_pixels;
    uint8_t *scaled = NULL;
    GLsizei up_w = width, up_h = height;
    uint16_t ds_num = 0, ds_den = 0;

    int mx = texds_max();
    GLsizei longest = width > height ? width : height;
    if (longest > mx) {
      if (width >= height) {
        up_w = mx;
        up_h = (GLsizei)((int64_t)height * mx / width);
      } else {
        up_h = mx;
        up_w = (GLsizei)((int64_t)width * mx / height);
      }
      if (up_w < 1) up_w = 1;
      if (up_h < 1) up_h = 1;
      scaled = texds_scale(src, width, height, src_stride, up_w, up_h);
      if (scaled) {
        src = scaled;
        src_stride = (size_t)up_w * 4u;
        ds_num = (uint16_t)up_w;
        ds_den = (uint16_t)width;
        g_texds_count++;
        if (g_texds_count <= 4)
          debugPrintf("texds: #%u %dx%d -> %dx%d\n", g_texds_count, width,
                      height, up_w, up_h);
      } else {
        up_w = width;
        up_h = height;
      }
    }

    int fmt = 0;
    uint16_t *px16 = tex16_convert(src, up_w, up_h, src_stride, &fmt);
    free(scaled);
    if (px16) {
      tex16_upload(0, target, level, 0, 0, up_w, up_h, fmt, px16);
      free(px16);
      free(repacked);
      if (g_bound_tex2d < TEX16_MAX_IDS) {
        g_tex16_fmt[g_bound_tex2d] = (uint8_t)fmt;
        g_texds_num[g_bound_tex2d] = ds_num;
        g_texds_den[g_bound_tex2d] = ds_den;
      }
      if (fmt == 1)
        g_tex16_count565++;
      else
        g_tex16_count4444++;
      if ((g_tex16_count565 + g_tex16_count4444) <= 4)
        debugPrintf("tex16: #%u %dx%d -> %s\n",
                    g_tex16_count565 + g_tex16_count4444, up_w, up_h,
                    fmt == 1 ? "RGB565" : "RGBA4444");
      GLenum err16 = glGetError();
      if (err16 != GL_NO_ERROR) {
        dbg_teximage_error_count++;
        dbg_last_tex_error = err16;
      }
      return;
    }
  }
  if (g_bound_tex2d < TEX16_MAX_IDS && target == GL_TEXTURE_2D) {
    g_tex16_fmt[g_bound_tex2d] = 0;
    g_texds_num[g_bound_tex2d] = 0;
    g_texds_den[g_bound_tex2d] = 0;
  }

  if (upload_pixels && unpack_row_length > width &&
      format == GL_RGBA && type == GL_UNSIGNED_BYTE && width > 0 && height > 0) {
    size_t dst_stride = (size_t)width * 4u;
    size_t src_stride = (size_t)unpack_row_length * 4u;
    repacked = malloc(dst_stride * (size_t)height);
    if (repacked) {
      const uint8_t *src = (const uint8_t *)upload_pixels;
      for (GLsizei y = 0; y < height; y++)
        memcpy(repacked + (size_t)y * dst_stride,
               src + (size_t)y * src_stride, dst_stride);
      upload_pixels = repacked;
    }
  }

  glTexImage2D(target, level, internalformat, width, height, border, format,
               type, upload_pixels);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    dbg_teximage_error_count++;
    dbg_last_tex_error = err;
  }
  free(repacked);
}

static void wrap_glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                                 GLint yoffset, GLsizei width, GLsizei height,
                                 GLenum format, GLenum type,
                                 const void *pixels) {
  dbg_texsubimage_count++;
  dbg_last_tex_w = width;
  dbg_last_tex_h = height;
  dbg_last_tex_internal = 0;
  dbg_last_tex_format = format;
  dbg_last_tex_type = type;

  /* textura ETC1 nao aceita TexSubImage — ignora (imagens do im.py nunca
   * sao atualizadas por retangulo; se acontecer, o contador dedura) */
  if (target == GL_TEXTURE_2D && g_bound_tex2d < TEX16_MAX_IDS &&
      g_tex_kind[g_bound_tex2d] != 0) {
    g_etc1_sub_skips++;
    return;
  }

  /* Se a textura de destino foi convertida p/ 16-bit, o SubImage TEM que
   * ir no mesmo formato (regra do GLES2). Converte o retangulo — e se a
   * textura foi ENCOLHIDA (texds), escala o retangulo na mesma proporcao. */
  if (target == GL_TEXTURE_2D && g_bound_tex2d < TEX16_MAX_IDS &&
      g_tex16_fmt[g_bound_tex2d] != 0 && pixels &&
      format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
    int want = g_tex16_fmt[g_bound_tex2d];
    size_t src_stride =
        (size_t)(unpack_row_length > width ? unpack_row_length : width) * 4u;
    const uint8_t *sub_src = (const uint8_t *)pixels;
    uint8_t *sub_scaled = NULL;
    uint16_t num = g_texds_num[g_bound_tex2d], den = g_texds_den[g_bound_tex2d];
    if (num && den) {
      GLint nx = (GLint)((int64_t)xoffset * num / den);
      GLint ny = (GLint)((int64_t)yoffset * num / den);
      GLsizei nw = (GLsizei)((int64_t)width * num / den);
      GLsizei nh = (GLsizei)((int64_t)height * num / den);
      if (nw < 1) nw = 1;
      if (nh < 1) nh = 1;
      sub_scaled = texds_scale(sub_src, width, height, src_stride, nw, nh);
      if (!sub_scaled)
        goto sub_fallback;
      sub_src = sub_scaled;
      src_stride = (size_t)nw * 4u;
      xoffset = nx;
      yoffset = ny;
      width = nw;
      height = nh;
    }
    int got = 0;
    uint16_t *px16 = tex16_convert(sub_src, width, height,
                                   src_stride, &got);
    free(sub_scaled);
    if (px16) {
      /* forca o formato registrado (mesmo que o retangulo seja opaco,
       * uma textura 4444 exige subimage 4444) */
      if (want == 2 && got == 1) {
        /* re-expande 565->4444 rapido: refaz com alpha F */
        for (GLsizei i = 0; i < width * height; i++) {
          uint16_t p = px16[i];
          uint8_t r = (uint8_t)((p >> 11) & 0x1F) << 3;
          uint8_t g = (uint8_t)((p >> 5) & 0x3F) << 2;
          uint8_t b = (uint8_t)(p & 0x1F) << 3;
          px16[i] = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) |
                               ((b >> 4) << 4) | 0xF);
        }
        got = 2;
      }
      if (got == want) {
        tex16_upload(1, target, level, xoffset, yoffset, width, height, want,
                     px16);
        free(px16);
        GLenum err16 = glGetError();
        if (err16 != GL_NO_ERROR) {
          dbg_teximage_error_count++;
          dbg_last_tex_error = err16;
        }
        return;
      }
      free(px16); /* want=565 mas veio alpha: cai no caminho 8888 (erro GL
                     inofensivo e melhor que corromper) */
    }
  }
sub_fallback:

  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                  pixels);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    dbg_teximage_error_count++;
    dbg_last_tex_error = err;
  }
}

static void wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  dbg_gl_draw_arrays_count++;
  ss_predraw();
  glDrawArrays(mode, first, count);
}

static void wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                const void *indices) {
  dbg_gl_draw_elements_count++;
  ss_predraw();
  dbg_last_draw_mode = mode;
  dbg_last_draw_count = count;
  dbg_last_draw_type = type;
  dbg_last_draw_ebo = 0;
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &dbg_last_draw_ebo);

  if (type == GL_UNSIGNED_INT && count > 0) {
    if (dbg_last_draw_ebo) {
      IndexShadowBuffer *entry = index_shadow_find((GLuint)dbg_last_draw_ebo, 0);
      uintptr_t original_offset = (uintptr_t)indices;
      if (entry && entry->valid && entry->shadow && (original_offset % 4) == 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, entry->shadow);
        glDrawElements(mode, count, GL_UNSIGNED_SHORT,
                       (const void *)(original_offset / 2));
        GLenum err = glGetError();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)dbg_last_draw_ebo);
        if (err == GL_NO_ERROR) {
          dbg_uint_index_fix_count++;
          return;
        }
        dbg_last_draw_error = err;
        dbg_uint_index_fail_count++;
        return;
      }
    } else if (indices) {
      uint16_t *converted = NULL;
      GLsizeiptr converted_size = 0;
      GLsizeiptr original_size = (GLsizeiptr)count * (GLsizeiptr)sizeof(uint32_t);
      if (convert_uint_indices(indices, original_size, &converted, &converted_size)) {
        (void)converted_size;
        glDrawElements(mode, count, GL_UNSIGNED_SHORT, converted);
        GLenum err = glGetError();
        free(converted);
        if (err == GL_NO_ERROR) {
          dbg_uint_index_fix_count++;
          return;
        }
        dbg_last_draw_error = err;
        dbg_uint_index_fail_count++;
        return;
      }
    }
  }

  glDrawElements(mode, count, type, indices);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    dbg_last_draw_error = err;
    if (type == GL_UNSIGNED_INT)
      dbg_uint_index_fail_count++;
  }
}

void *summertime_gl_lookup(const char *name) {
  if (!name) return NULL;
  if (strcmp(name, "glClearColor") == 0) return (void *)wrap_glClearColor;
  if (strcmp(name, "glClear") == 0) return (void *)wrap_glClear;
  if (strcmp(name, "glViewport") == 0) return (void *)wrap_glViewport;
  if (strcmp(name, "glUseProgram") == 0) return (void *)wrap_glUseProgram;
  if (strcmp(name, "glBindBuffer") == 0) return (void *)wrap_glBindBuffer;
  if (strcmp(name, "glBindFramebuffer") == 0) return (void *)wrap_glBindFramebuffer;
  if (strcmp(name, "glBufferData") == 0) return (void *)wrap_glBufferData;
  if (strcmp(name, "glBufferSubData") == 0) return (void *)wrap_glBufferSubData;
  if (strcmp(name, "glDeleteBuffers") == 0) return (void *)wrap_glDeleteBuffers;
  if (strcmp(name, "glDrawArrays") == 0) return (void *)wrap_glDrawArrays;
  if (strcmp(name, "glDrawElements") == 0) return (void *)wrap_glDrawElements;
  if (strcmp(name, "glGenerateMipmap") == 0) return (void *)wrap_glGenerateMipmap;
  if (strcmp(name, "glPixelStorei") == 0) return (void *)wrap_glPixelStorei;
  if (strcmp(name, "glBindTexture") == 0) return (void *)wrap_glBindTexture;
  if (strcmp(name, "glTexImage2D") == 0) return (void *)wrap_glTexImage2D;
  if (strcmp(name, "glTexParameteri") == 0) return (void *)wrap_glTexParameteri;
  if (strcmp(name, "glTexSubImage2D") == 0) return (void *)wrap_glTexSubImage2D;
  if (strcmp(name, "glUniformMatrix4fv") == 0) return (void *)wrap_glUniformMatrix4fv;
  if (strcmp(name, "glVertexAttribPointer") == 0) return (void *)wrap_glVertexAttribPointer;
  if (strcmp(name, "glActiveTexture") == 0) return (void *)wrap_glActiveTexture;
  if (strcmp(name, "glShaderSource") == 0) return (void *)wrap_glShaderSource;
  if (strcmp(name, "glLinkProgram") == 0) return (void *)wrap_glLinkProgram;
  if (strcmp(name, "glDeleteProgram") == 0) return (void *)wrap_glDeleteProgram;
  if (strcmp(name, "glUniform1i") == 0) return (void *)wrap_glUniform1i;
  if (strcmp(name, "glDeleteTextures") == 0) return (void *)wrap_glDeleteTextures;
  if (strcmp(name, "glCopyTexImage2D") == 0) return (void *)wrap_glCopyTexImage2D;
  return NULL;
}

void summertime_gl_debug_frame(void) {
  if (!getenv("SUMMERTIME_GLDBG"))
    return;

  static unsigned frame;
  frame++;

  if (!(frame <= 20 || frame % 60 == 0)) {
    dbg_gl_clear_count = dbg_gl_draw_arrays_count = dbg_gl_draw_elements_count = 0;
    dbg_gl_viewport_count = dbg_gl_bindfb_count = 0;
    return;
  }

  GLint vp[4] = {0, 0, 0, 0};
  GLint fb = -1;
  GLint program = -1;
  GLboolean mask[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  GLenum err = glGetError();

  fprintf(stderr,
          "[GLDBG] frame=%u clear=%u drawA=%u drawE=%u viewport_calls=%u "
          "bindfb=%u fb=%d vp=%d,%d %dx%d prog=%d mask=%d%d%d%d "
          "clear=%.2f,%.2f,%.2f,%.2f lastDraw=0x%x/%d/0x%x "
          "ebo=%d drawErr=0x%x uintFix=%u uintFail=%u "
          "m4=%u@%d[%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g] "
          "attr=%u a0=%d/0x%x/%d/buf%d[%.1f %.1f %.1f %.1f] "
          "a1=%d/0x%x/%d/buf%d[%.3f %.3f %.3f %.3f] "
          "tex=%u/%u last=%dx%d if=0x%x fmt=0x%x type=0x%x texerr=%u/0x%x err=0x%x\n",
          frame, dbg_gl_clear_count, dbg_gl_draw_arrays_count,
          dbg_gl_draw_elements_count, dbg_gl_viewport_count, dbg_gl_bindfb_count,
          fb, vp[0], vp[1], vp[2], vp[3], program,
          mask[0], mask[1], mask[2], mask[3],
          dbg_clear_color[0], dbg_clear_color[1], dbg_clear_color[2],
          dbg_clear_color[3], dbg_last_draw_mode, dbg_last_draw_count,
          dbg_last_draw_type, dbg_last_draw_ebo, dbg_last_draw_error,
          dbg_uint_index_fix_count, dbg_uint_index_fail_count,
          dbg_matrix4_count, dbg_last_matrix4_location,
          dbg_last_matrix4[0], dbg_last_matrix4[1], dbg_last_matrix4[2], dbg_last_matrix4[3],
          dbg_last_matrix4[4], dbg_last_matrix4[5], dbg_last_matrix4[6], dbg_last_matrix4[7],
          dbg_last_matrix4[8], dbg_last_matrix4[9], dbg_last_matrix4[10], dbg_last_matrix4[11],
          dbg_last_matrix4[12], dbg_last_matrix4[13], dbg_last_matrix4[14], dbg_last_matrix4[15],
          dbg_attr_pointer_count,
          dbg_attrib[0].size, dbg_attrib[0].type, dbg_attrib[0].stride,
          dbg_attrib[0].array_buffer, dbg_attrib[0].sample[0],
          dbg_attrib[0].sample[1], dbg_attrib[0].sample[2],
          dbg_attrib[0].sample[3],
          dbg_attrib[1].size, dbg_attrib[1].type, dbg_attrib[1].stride,
          dbg_attrib[1].array_buffer, dbg_attrib[1].sample[0],
          dbg_attrib[1].sample[1], dbg_attrib[1].sample[2],
          dbg_attrib[1].sample[3],
          dbg_teximage_count, dbg_texsubimage_count, dbg_last_tex_w,
          dbg_last_tex_h, dbg_last_tex_internal, dbg_last_tex_format,
          dbg_last_tex_type, dbg_teximage_error_count, dbg_last_tex_error, err);

  fprintf(stderr,
          "[GLDBG] etc1: opaque=%u dual=%u sub_skips=%u shader_patches=%u "
          "tex16=%u/%u ds=%u\n",
          g_etc1_count, g_etc1_dual_count, g_etc1_sub_skips,
          g_shader_patch_count, g_tex16_count565, g_tex16_count4444,
          g_texds_count);

  dbg_gl_clear_count = dbg_gl_draw_arrays_count = dbg_gl_draw_elements_count = 0;
  dbg_gl_viewport_count = dbg_gl_bindfb_count = 0;
  dbg_uint_index_fix_count = dbg_uint_index_fail_count = 0;
  dbg_attr_pointer_count = dbg_matrix4_count = 0;
  dbg_teximage_count = dbg_texsubimage_count = dbg_teximage_error_count = 0;
  dbg_last_tex_error = GL_NO_ERROR;
  dbg_last_draw_error = GL_NO_ERROR;
}

// The engine uses bionic stdio: stdin/out/err == &__sF[0/1/2]. We map __sF to
// fake_sF, but our resolved fprintf/fwrite/... are glibc's and would reject a
// fake FILE*. Translate any pointer inside fake_sF to the real glibc stream.
static FILE *map_sf(void *f) {
  uintptr_t p = (uintptr_t)f;
  uintptr_t base = (uintptr_t)fake_sF;
  if (p >= base && p < base + sizeof(fake_sF)) {
    int idx = (int)((p - base) / 0x100);
    if (idx == 0)
      return stdin;
    if (idx == 1)
      return stdout;
    return stderr;
  }
  return (FILE *)f;
}

static int sf_fprintf(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_sf(f), fmt, ap);
  va_end(ap);
  return r;
}
static int sf_vfprintf(void *f, const char *fmt, va_list ap) {
  return vfprintf(map_sf(f), fmt, ap);
}
static int sf_fputc(int c, void *f) { return fputc(c, map_sf(f)); }
static int sf_fputs(const char *s, void *f) { return fputs(s, map_sf(f)); }
static size_t sf_fwrite(const void *p, size_t sz, size_t n, void *f) {
  return fwrite(p, sz, n, map_sf(f));
}
static int sf_fflush(void *f) { return fflush(f ? map_sf(f) : NULL); }
static int sf_ferror(void *f) { return ferror(map_sf(f)); }
static int sf_feof(void *f) { return feof(map_sf(f)); }
static int sf_fileno(void *f) { return fileno(map_sf(f)); }
static int sf_fgetc(void *f) { return fgetc(map_sf(f)); }
static char *sf_fgets(char *s, int n, void *f) { return fgets(s, n, map_sf(f)); }
extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;

static void __stack_chk_fail_stub(void) {
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  uintptr_t g = tls ? *(uintptr_t *)(tls + 0x28) : 0;
  debugPrintf("__stack_chk_fail! caller=%p tls+0x28=0x%lx text_base=%p\n",
              __builtin_return_address(0), (unsigned long)g, text_base);
}

static int *__errno_fake(void) { return &errno; }
extern int *__h_errno_location(void);
static int *__get_h_errno_fake(void) { return __h_errno_location(); }

typedef struct {
  void *ptr;
  size_t size;
  uint8_t state; // 0 empty, 1 live, 2 tombstone
} AllocSlot;

#define ALLOC_TABLE_CAP 1048576u
static AllocSlot alloc_table[ALLOC_TABLE_CAP];
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t alloc_hash(void *ptr) {
  uintptr_t x = (uintptr_t)ptr >> 4;
  x ^= x >> 33;
  x *= (uintptr_t)0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x & (ALLOC_TABLE_CAP - 1u);
}

static ssize_t alloc_find_locked(void *ptr, ssize_t *insert_at) {
  size_t start = alloc_hash(ptr);
  ssize_t first_tombstone = -1;
  for (size_t i = 0; i < ALLOC_TABLE_CAP; i++) {
    size_t idx = (start + i) & (ALLOC_TABLE_CAP - 1u);
    if (alloc_table[idx].state == 1 && alloc_table[idx].ptr == ptr) {
      if (insert_at)
        *insert_at = (ssize_t)idx;
      return (ssize_t)idx;
    }
    if (alloc_table[idx].state == 2 && first_tombstone < 0)
      first_tombstone = (ssize_t)idx;
    if (alloc_table[idx].state == 0) {
      if (insert_at)
        *insert_at = first_tombstone >= 0 ? first_tombstone : (ssize_t)idx;
      return -1;
    }
  }
  if (insert_at)
    *insert_at = first_tombstone;
  return -1;
}

static void alloc_track(void *ptr, size_t size) {
  if (!ptr)
    return;
  pthread_mutex_lock(&alloc_lock);
  ssize_t insert_at = -1;
  ssize_t found = alloc_find_locked(ptr, &insert_at);
  ssize_t idx = found >= 0 ? found : insert_at;
  if (idx >= 0) {
    alloc_table[idx].ptr = ptr;
    alloc_table[idx].size = size;
    alloc_table[idx].state = 1;
  }
  pthread_mutex_unlock(&alloc_lock);
}

static int alloc_untrack(void *ptr, size_t *size_out) {
  int ok = 0;
  pthread_mutex_lock(&alloc_lock);
  ssize_t found = alloc_find_locked(ptr, NULL);
  if (found >= 0) {
    if (size_out)
      *size_out = alloc_table[found].size;
    alloc_table[found].ptr = NULL;
    alloc_table[found].size = 0;
    alloc_table[found].state = 2;
    ok = 1;
  }
  pthread_mutex_unlock(&alloc_lock);
  return ok;
}

static void *my_malloc(size_t size) {
  void *ptr = malloc(size);
  alloc_track(ptr, size);
  return ptr;
}

static void *my_calloc(size_t nmemb, size_t size) {
  void *ptr = calloc(nmemb, size);
  alloc_track(ptr, nmemb * size);
  return ptr;
}

static void my_free(void *ptr) {
  if (!ptr)
    return;
  if (!alloc_untrack(ptr, NULL)) {
    if (getenv("SUMMERTIME_FREE_STRICT"))
      free(ptr);
    return;
  }
  if (!getenv("SUMMERTIME_NO_REAL_FREE"))
    free(ptr);
}

static void *my_realloc(void *ptr, size_t size) {
  if (!ptr)
    return my_malloc(size);
  if (size == 0) {
    my_free(ptr);
    return NULL;
  }
  size_t old_size = 0;
  if (!alloc_untrack(ptr, &old_size)) {
    void *new_ptr = malloc(size);
    alloc_track(new_ptr, size);
    return new_ptr;
  }
  void *new_ptr = malloc(size);
  if (!new_ptr) {
    alloc_track(ptr, old_size);
    return NULL;
  }
  memcpy(new_ptr, ptr, old_size < size ? old_size : size);
  alloc_track(new_ptr, size);
  if (!getenv("SUMMERTIME_NO_REAL_FREE"))
    free(ptr);
  return new_ptr;
}

static int my_posix_memalign(void **memptr, size_t alignment, size_t size) {
  int rc = posix_memalign(memptr, alignment, size);
  if (rc == 0)
    alloc_track(*memptr, size);
  return rc;
}

static char *my_strdup(const char *s) {
  char *ptr = strdup(s);
  if (ptr)
    alloc_track(ptr, strlen(ptr) + 1);
  return ptr;
}

static int my_vasprintf(char **strp, const char *fmt, va_list ap) {
  int rc = vasprintf(strp, fmt, ap);
  if (rc >= 0 && strp && *strp)
    alloc_track(*strp, (size_t)rc + 1);
  return rc;
}

static int log_verbose(void) {
  static int v = -1;
  if (v < 0)
    v = getenv("SUMMERTIME_VERBOSE") ? 1 : 0;
  return v;
}

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  if (!log_verbose())
    return 0;
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("LOG [%s]: %s\n", tag, string);
  return 0;
}

int __android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  if (!log_verbose())
    return 0;
  debugPrintf("LOG [%s]: %s\n", tag, text);
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memmove(dst, src, n);
}

char *__strcat_chk(char *dst, const char *src, size_t dst_buf_size) {
  (void)dst_buf_size;
  return strcat(dst, src);
}

char *__strcpy_chk(char *dst, const char *src, size_t dst_len) {
  (void)dst_len;
  return strcpy(dst, src);
}

size_t __strlen_chk(const char *s, size_t max_len) {
  (void)max_len;
  return strlen(s);
}

char *__strrchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return (char *)strrchr(s, c);
}

char *__strchr_chk(const char *s, int c, size_t slen) {
  (void)slen;
  return (char *)strchr(s, c);
}

char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len,
                     size_t src_len) {
  (void)dst_len;
  (void)src_len;
  return strncpy(dst, src, n);
}

char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return strncpy(dst, src, n);
}

int __vsprintf_chk(char *dst, int flags, size_t dst_len, const char *fmt,
                   va_list ap) {
  (void)flags;
  (void)dst_len;
  return vsprintf(dst, fmt, ap);
}

int __vsnprintf_chk(char *dst, size_t supplied_size, int flags, size_t dst_len,
                    const char *fmt, va_list ap) {
  (void)flags;
  (void)dst_len;
  return vsnprintf(dst, supplied_size, fmt, ap);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

void __FD_CLR_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  FD_CLR(fd, set);
}
void __FD_SET_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  FD_SET(fd, set);
}
int __FD_ISSET_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  return FD_ISSET(fd, set);
}

static int g_fopen_log = -1;
static FILE *my_fopen(const char *pathname, const char *mode) {
  const char *r = resolve_android_path(pathname);
  FILE *f = fopen(r, mode);
  if (g_fopen_log < 0)
    g_fopen_log = getenv("SUMMERTIME_FOPENLOG") ? 1 : 0;
  if (g_fopen_log)
    debugPrintf("fopen(\"%s\" -> \"%s\", \"%s\") = %s\n", pathname, r, mode,
                f ? "OK" : "FAIL");
  return f;
}

static int my_stat(const char *pathname, struct stat *st) {
  return stat(resolve_android_path(pathname), st);
}

/* glibc < 2.33 nao exporta a familia stat como simbolo dinamico (o binario
 * linka via libc_nonshared, mas o import do .so fica NAO RESOLVIDO no
 * ArkOS/Buster). Mesmo caso: strlcpy/strlcat/wcslcpy/wcslcat so existem na
 * glibc 2.38+ (origem BSD/bionic). Wrappers proprios p/ qualquer firmware. */
static int my_lstat(const char *pathname, struct stat *st) {
  return lstat(resolve_android_path(pathname), st);
}

static int my_fstatat(int dirfd, const char *pathname, struct stat *st,
                      int flags) {
  return fstatat(dirfd, resolve_android_path(pathname), st, flags);
}

static int my_mknod(const char *pathname, mode_t mode, dev_t dev) {
  return mknod(resolve_android_path(pathname), mode, dev);
}

static int my_mknodat(int dirfd, const char *pathname, mode_t mode,
                      dev_t dev) {
  return mknodat(dirfd, resolve_android_path(pathname), mode, dev);
}

/* mbstate_t: bionic tem 4 bytes, glibc tem 8. O librenpython aloca o de 4
 * na propria stack e passa para a libc do host — a glibc escreve 8 e esmaga
 * o local vizinho (na 2.28 o padrao de escrita zera um endereco de retorno:
 * ret -> PC=0 no init do Python). Side-table: o ponteiro do chamador vira
 * chave, o estado real fica num slot nosso; os 4 bytes do chamador guardam
 * so um magic p/ detectar re-inicializacao (memset 0). */
/* NAO usar _Thread_local aqui: TLS novo deslocaria o g_bionic_guard_pad do
 * offset 0 e quebraria a canary bionic (o audit do build barra). O caminho
 * e frio (decodificacao de locale), entao um mutex baixo custo resolve. */
#define SS_MB_MAGIC 0x53534D42u
static struct {
  void *key;
  unsigned long tid;
  mbstate_t st;
} ss_mb_slots[16];
static int ss_mb_next;
static pthread_mutex_t ss_mb_lock = PTHREAD_MUTEX_INITIALIZER;

static mbstate_t *ss_mbstate(void *ps) {
  if (!ps)
    return NULL;
  unsigned *tag = (unsigned *)ps;
  unsigned long tid = (unsigned long)pthread_self();
  pthread_mutex_lock(&ss_mb_lock);
  mbstate_t *st = NULL;
  for (int i = 0; i < 16; i++) {
    if (ss_mb_slots[i].key == ps && ss_mb_slots[i].tid == tid) {
      if (*tag != SS_MB_MAGIC) {
        memset(&ss_mb_slots[i].st, 0, sizeof(mbstate_t));
        *tag = SS_MB_MAGIC;
      }
      st = &ss_mb_slots[i].st;
      break;
    }
  }
  if (!st) {
    int i = (ss_mb_next++) & 15;
    ss_mb_slots[i].key = ps;
    ss_mb_slots[i].tid = tid;
    memset(&ss_mb_slots[i].st, 0, sizeof(mbstate_t));
    *tag = SS_MB_MAGIC;
    st = &ss_mb_slots[i].st;
  }
  pthread_mutex_unlock(&ss_mb_lock);
  return st;
}

static size_t my_mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps) {
  return mbrtowc(pwc, s, n, ss_mbstate(ps));
}

static size_t my_mbrlen(const char *s, size_t n, void *ps) {
  return mbrlen(s, n, ss_mbstate(ps));
}

static size_t my_mbsrtowcs(wchar_t *dst, const char **src, size_t len,
                           void *ps) {
  return mbsrtowcs(dst, src, len, ss_mbstate(ps));
}

static size_t my_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms,
                            size_t len, void *ps) {
  return mbsnrtowcs(dst, src, nms, len, ss_mbstate(ps));
}

static size_t my_wcrtomb(char *s, wchar_t wc, void *ps) {
  return wcrtomb(s, wc, ss_mbstate(ps));
}

static size_t my_wcsrtombs(char *dst, const wchar_t **src, size_t len,
                           void *ps) {
  return wcsrtombs(dst, src, len, ss_mbstate(ps));
}

static size_t my_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc,
                            size_t len, void *ps) {
  return wcsnrtombs(dst, src, nwc, len, ss_mbstate(ps));
}

static int my_mbsinit(const void *ps) {
  if (!ps || *(const unsigned *)ps != SS_MB_MAGIC)
    return 1;
  for (int i = 0; i < 8; i++)
    if (ss_mb_slots[i].key == (void *)ps)
      return mbsinit(&ss_mb_slots[i].st);
  return 1;
}

static size_t ss_strlcpy(char *dst, const char *src, size_t dsize) {
  const char *osrc = src;
  size_t nleft = dsize;
  if (nleft != 0) {
    while (--nleft != 0) {
      if ((*dst++ = *src++) == '\0')
        break;
    }
  }
  if (nleft == 0) {
    if (dsize != 0)
      *dst = '\0';
    while (*src++)
      ;
  }
  return (size_t)(src - osrc - 1);
}

static size_t ss_strlcat(char *dst, const char *src, size_t dsize) {
  const char *odst = dst;
  const char *osrc = src;
  size_t n = dsize;
  size_t dlen;
  while (n-- != 0 && *dst != '\0')
    dst++;
  dlen = (size_t)(dst - odst);
  n = dsize - dlen;
  if (n-- == 0)
    return dlen + strlen(src);
  while (*src != '\0') {
    if (n != 0) {
      *dst++ = *src;
      n--;
    }
    src++;
  }
  *dst = '\0';
  return dlen + (size_t)(src - osrc);
}

static size_t ss_wcslcpy(wchar_t *dst, const wchar_t *src, size_t dsize) {
  const wchar_t *osrc = src;
  size_t nleft = dsize;
  if (nleft != 0) {
    while (--nleft != 0) {
      if ((*dst++ = *src++) == L'\0')
        break;
    }
  }
  if (nleft == 0) {
    if (dsize != 0)
      *dst = L'\0';
    while (*src++)
      ;
  }
  return (size_t)(src - osrc - 1);
}

static size_t ss_wcslcat(wchar_t *dst, const wchar_t *src, size_t dsize) {
  const wchar_t *odst = dst;
  const wchar_t *osrc = src;
  size_t n = dsize;
  size_t dlen;
  while (n-- != 0 && *dst != L'\0')
    dst++;
  dlen = (size_t)(dst - odst);
  n = dsize - dlen;
  if (n-- == 0)
    return dlen + wcslen(src);
  while (*src != L'\0') {
    if (n != 0) {
      *dst++ = *src;
      n--;
    }
    src++;
  }
  *dst = L'\0';
  return dlen + (size_t)(src - osrc);
}

static int my_access(const char *pathname, int amode) {
  return access(resolve_android_path(pathname), amode);
}

static int my_open(const char *pathname, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    return open(resolve_android_path(pathname), flags, mode);
  }
  return open(resolve_android_path(pathname), flags);
}

static int my_open_2(const char *pathname, int flags) {
  return open(resolve_android_path(pathname), flags);
}

static int mkdir_fake(const char *pathname, mode_t mode) {
  const char *resolved = resolve_android_path(pathname);
  int ret = mkdir(resolved, mode);
  if (ret == 0)
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = 0\n", pathname, resolved,
                (unsigned)mode);
  else
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = -1 (errno=%d: %s)\n", pathname,
                resolved, (unsigned)mode, errno, strerror(errno));
  return ret;
}

static int remove_fake(const char *pathname) {
  const char *resolved = resolve_android_path(pathname);
  int ret = remove(resolved);
  if (ret == 0)
    debugPrintf("remove(\"%s\" -> \"%s\") = 0\n", pathname, resolved);
  else
    debugPrintf("remove(\"%s\" -> \"%s\") = -1 (errno=%d: %s)\n", pathname,
                resolved, errno, strerror(errno));
  return ret;
}

static int rename_fake(const char *oldpath, const char *newpath) {
  char resolved_old[2048];
  char resolved_new[2048];
  const char *resolved_old_src = resolve_android_path(oldpath);
  const char *resolved_new_src;
  int ret;

  snprintf(resolved_old, sizeof(resolved_old), "%s", resolved_old_src);
  resolved_new_src = resolve_android_path(newpath);
  snprintf(resolved_new, sizeof(resolved_new), "%s", resolved_new_src);
  ret = rename(resolved_old, resolved_new);
  if (ret == 0) {
    debugPrintf("rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = 0\n", oldpath,
                resolved_old, newpath, resolved_new);
  } else {
    debugPrintf(
        "rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = -1 (errno=%d: %s)\n",
        oldpath, resolved_old, newpath, resolved_new, errno, strerror(errno));
  }
  return ret;
}

// LSW PTHREAD HACKS

typedef struct HostMutexEntry {
  void *guest_addr;
  pthread_mutex_t mutex;
  struct HostMutexEntry *next;
} HostMutexEntry;

typedef struct HostCondEntry {
  void *guest_addr;
  pthread_cond_t cond;
  struct HostCondEntry *next;
} HostCondEntry;

static HostMutexEntry *g_mutex_entries = NULL;
static HostCondEntry *g_cond_entries = NULL;
static pthread_mutex_t g_mutex_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cond_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *lookup_host_mutex(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;
  pthread_mutex_lock(&g_mutex_registry_lock);
  for (HostMutexEntry *entry = g_mutex_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_mutex_registry_lock);
      return &entry->mutex;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_mutex_registry_lock);
    return NULL;
  }
  HostMutexEntry *entry = calloc(1, sizeof(*entry));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&entry->mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  entry->guest_addr = guest_addr;
  entry->next = g_mutex_entries;
  g_mutex_entries = entry;
  pthread_mutex_unlock(&g_mutex_registry_lock);
  return &entry->mutex;
}

static int destroy_host_mutex(void *guest_addr) {
  if (!guest_addr)
    return 0;
  pthread_mutex_lock(&g_mutex_registry_lock);
  HostMutexEntry **link = &g_mutex_entries;
  while (*link) {
    HostMutexEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
      *link = entry->next;
      pthread_mutex_unlock(&g_mutex_registry_lock);
      pthread_mutex_destroy(&entry->mutex);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_mutex_registry_lock);
  return 0;
}

static pthread_cond_t *lookup_host_cond(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;
  pthread_mutex_lock(&g_cond_registry_lock);
  for (HostCondEntry *entry = g_cond_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_cond_registry_lock);
      return &entry->cond;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_cond_registry_lock);
    return NULL;
  }
  HostCondEntry *entry = calloc(1, sizeof(*entry));
  pthread_cond_init(&entry->cond, NULL);
  entry->guest_addr = guest_addr;
  entry->next = g_cond_entries;
  g_cond_entries = entry;
  pthread_mutex_unlock(&g_cond_registry_lock);
  return &entry->cond;
}

static int destroy_host_cond(void *guest_addr) {
  if (!guest_addr)
    return 0;
  pthread_mutex_lock(&g_cond_registry_lock);
  HostCondEntry **link = &g_cond_entries;
  while (*link) {
    HostCondEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
      *link = entry->next;
      pthread_mutex_unlock(&g_cond_registry_lock);
      pthread_cond_destroy(&entry->cond);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_cond_registry_lock);
  return 0;
}

int pthread_mutex_init_fake(pthread_mutex_t *uid, const int *mutexattr) {
  return lookup_host_mutex(uid, 1) ? 0 : -1;
}
int pthread_mutex_destroy_fake(pthread_mutex_t *uid) {
  return destroy_host_mutex(uid);
}
int pthread_mutex_lock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_lock(lookup_host_mutex(uid, 1));
}
int pthread_mutex_trylock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_trylock(lookup_host_mutex(uid, 1));
}
int pthread_mutex_unlock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_unlock(lookup_host_mutex(uid, 1));
}

int pthread_cond_init_fake(pthread_cond_t *cnd, const int *condattr) {
  return lookup_host_cond(cnd, 1) ? 0 : -1;
}
int pthread_cond_destroy_fake(pthread_cond_t *cnd) {
  return destroy_host_cond(cnd);
}
int pthread_cond_wait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx) {
  return pthread_cond_wait(lookup_host_cond(cnd, 1), lookup_host_mutex(mtx, 1));
}
int pthread_cond_timedwait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx,
                                const struct timespec *t) {
  return pthread_cond_timedwait(lookup_host_cond(cnd, 1),
                                lookup_host_mutex(mtx, 1), t);
}
int pthread_cond_signal_fake(pthread_cond_t *cnd) {
  return pthread_cond_signal(lookup_host_cond(cnd, 1));
}
int pthread_cond_broadcast_fake(pthread_cond_t *cnd) {
  return pthread_cond_broadcast(lookup_host_cond(cnd, 1));
}

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadWrapper;
static void *thread_wrapper_func(void *data) {
  ThreadWrapper *w = (ThreadWrapper *)data;
  void *(*entry)(void *) = w->entry;
  void *arg = w->arg;
  free(w);
  return entry(arg);
}
int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                        void *arg) {
  ThreadWrapper *w = malloc(sizeof(ThreadWrapper));
  w->entry = entry;
  w->arg = arg;
  pthread_attr_t real_attr;
  pthread_attr_init(&real_attr);
  pthread_attr_setstacksize(&real_attr, 2 * 1024 * 1024);
  int ret = pthread_create(thread, &real_attr, thread_wrapper_func, w);
  pthread_attr_destroy(&real_attr);
  if (ret != 0)
    free(w);
  return ret;
}

static void *pthread_getspecific_fake(pthread_key_t key) {
  return pthread_getspecific(key);
}
static int pthread_setspecific_fake(pthread_key_t key, const void *value) {
  return pthread_setspecific(key, value);
}
int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  return pthread_once((pthread_once_t *)once_control, init_routine);
}

// ============================================================================
// Summertime/Ren'Py so-loader shims (SDL2 Android, Mali fbdev)
// ============================================================================

// Screen geometry, set by main() before the engine runs.
int g_summertime_screen_w = 1280;
int g_summertime_screen_h = 720;

// Mali fbdev native window: EGL/fbdev_window.h => { unsigned short w,h }.
typedef struct {
  unsigned short width;
  unsigned short height;
} summertime_fbdev_window;
static summertime_fbdev_window g_fbdev_window;

// SDL android video calls ANativeWindow_fromSurface(env, surface) and passes
// the result to eglCreateWindowSurface. On Mali fbdev that must be a
// fbdev_window*. We ignore the (fake) surface and return our fbdev window.
static void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env;
  (void)surface;
  g_fbdev_window.width = (unsigned short)g_summertime_screen_w;
  g_fbdev_window.height = (unsigned short)g_summertime_screen_h;
  return &g_fbdev_window;
}
static void ANativeWindow_release_fake(void *w) { (void)w; }
static int ANativeWindow_getWidth_fake(void *w) {
  (void)w;
  return g_summertime_screen_w;
}
static int ANativeWindow_getHeight_fake(void *w) {
  (void)w;
  return g_summertime_screen_h;
}
static int ANativeWindow_getFormat_fake(void *w) {
  (void)w;
  return 1; // WINDOW_FORMAT_RGBA_8888
}
static int ANativeWindow_setBuffersGeometry_fake(void *w, int width, int height,
                                                 int format) {
  (void)w;
  (void)width;
  (void)height;
  (void)format;
  return 0;
}

// --- dlopen/dlsym self-reference -------------------------------------------
// nativeRunMain() does dlopen(mainlib) + dlsym(handle, "SDL_main"). We map
// librenpython self-names to a sentinel whose dlsym resolves
// against the loaded blob. Other libs (libEGL/libGLESv2...) pass through to
// the real loader so SDL's SDL_EGL_LoadLibrary finds Mali.
#define SUMMERTIME_SELF_HANDLE ((void *)0x50754E)
// libEGL -> our egl_shim (EGL backed by the device SDL2). The game's static SDL
// renders through whatever backend the device SDL2 picks (fbdev on Mali-450,
// KMSDRM/Wayland on R36S). Same idea Bully/Dysmantle use.
#define SUMMERTIME_EGL_HANDLE ((void *)0x50754F)
#define SUMMERTIME_OPENSL_HANDLE ((void *)0x507550)

// Tiny EGL stubs for functions SDL 2.0.8 REQUIRES but egl_shim doesn't model
// (SDL fails LoadLibrary if any is NULL). Harmless no-ops.
static unsigned eglstub_true(void) { return 1; }            // EGL_TRUE
static unsigned eglstub_queryapi(void) { return 0x30A0; }   // EGL_OPENGL_ES_API
static void *eglstub_curdisplay(void) { return (void *)"display"; }
static int sl_record_tag;
static const void *sl_IID_RECORD_fake = &sl_record_tag;

static void *egl_shim_lookup(const char *name) {
  static const struct {
    const char *n;
    void *f;
  } map[] = {
      {"eglWaitNative", (void *)eglstub_true},
      {"eglWaitGL", (void *)eglstub_true},
      {"eglQueryAPI", (void *)eglstub_queryapi},
      {"eglGetCurrentDisplay", (void *)eglstub_curdisplay},
      {"eglReleaseThread", (void *)eglstub_true},
      {"eglGetDisplay", (void *)egl_shim_GetDisplay},
      {"eglInitialize", (void *)egl_shim_Initialize},
      {"eglTerminate", (void *)egl_shim_Terminate},
      {"eglChooseConfig", (void *)egl_shim_ChooseConfig},
      {"eglCreateWindowSurface", (void *)egl_shim_CreateWindowSurface},
      {"eglCreatePbufferSurface", (void *)egl_shim_CreatePbufferSurface},
      {"eglCreateContext", (void *)egl_shim_CreateContext},
      {"eglMakeCurrent", (void *)egl_shim_MakeCurrent},
      {"eglSwapBuffers", (void *)egl_shim_SwapBuffers},
      {"eglDestroySurface", (void *)egl_shim_DestroySurface},
      {"eglDestroyContext", (void *)egl_shim_DestroyContext},
      {"eglQuerySurface", (void *)egl_shim_QuerySurface},
      {"eglGetConfigAttrib", (void *)egl_shim_GetConfigAttrib},
      {"eglGetError", (void *)egl_shim_GetError},
      {"eglGetProcAddress", (void *)egl_shim_GetProcAddress},
      {"eglBindAPI", (void *)egl_shim_BindAPI},
      {"eglQueryString", (void *)egl_shim_QueryString},
      {"eglSwapInterval", (void *)egl_shim_SwapInterval},
      {"eglGetCurrentContext", (void *)egl_shim_GetCurrentContext},
      {"eglGetCurrentSurface", (void *)egl_shim_GetCurrentSurface},
      {"eglSurfaceAttrib", (void *)egl_shim_SurfaceAttrib},
  };
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (strcmp(name, map[i].n) == 0)
      return map[i].f;
  return NULL;
}

static void *my_dlopen(const char *name, int flag) {
  if (!name)
    return dlopen(NULL, flag ? flag : RTLD_NOW);
  if (getenv("SUMMERTIME_VERBOSE"))
    debugPrintf("my_dlopen(\"%s\")\n", name);
  if (strstr(name, "librenpython"))
    return SUMMERTIME_SELF_HANDLE;
  if (strstr(name, "libEGL"))
    return SUMMERTIME_EGL_HANDLE;
  if (strstr(name, "OpenSLES"))
    return SUMMERTIME_OPENSL_HANDLE;
  void *h = dlopen(name, flag ? flag : (RTLD_NOW | RTLD_GLOBAL));
  if (h)
    return h;
  debugPrintf("my_dlopen(\"%s\") failed (%s); returning self-handle\n", name,
              dlerror());
  return SUMMERTIME_SELF_HANDLE;
}

static void *my_dlsym(void *handle, const char *name) {
  void *gl_debug = summertime_gl_lookup(name);
  if (gl_debug)
    return gl_debug;

  if (handle == SUMMERTIME_EGL_HANDLE) {
    void *f = egl_shim_lookup(name);
    return f; // unknown egl* (eglWaitGL/...) -> NULL (harmless)
  }
  if (handle == SUMMERTIME_OPENSL_HANDLE) {
    if (!name)
      return NULL;
    if (strcmp(name, "slCreateEngine") == 0)
      return (void *)slCreateEngine_shim;
    if (strcmp(name, "SL_IID_ENGINE") == 0)
      return (void *)&sl_IID_ENGINE;
    if (strcmp(name, "SL_IID_PLAY") == 0)
      return (void *)&sl_IID_PLAY;
    if (strcmp(name, "SL_IID_VOLUME") == 0)
      return (void *)&sl_IID_VOLUME;
    if (strcmp(name, "SL_IID_RECORD") == 0)
      return (void *)&sl_IID_RECORD_fake;
    if (strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0 ||
        strcmp(name, "SL_IID_BUFFERQUEUE") == 0)
      return (void *)&sl_IID_BUFFERQUEUE;
    return NULL;
  }
  if (handle == SUMMERTIME_SELF_HANDLE) {
    uintptr_t a = so_find_addr_safe(name);
    if (a)
      return (void *)a;
    return dlsym(RTLD_DEFAULT, name);
  }
  return dlsym(handle, name);
}

static int my_dlclose(void *handle) {
  if (getenv("SUMMERTIME_VERBOSE"))
    debugPrintf("my_dlclose(%p)\n", handle);
  if (handle == SUMMERTIME_SELF_HANDLE || handle == SUMMERTIME_EGL_HANDLE ||
      handle == SUMMERTIME_OPENSL_HANDLE)
    return 0; // fake handles: no-op (dlclose on them would crash ld.so)
  // Defensive: never dlclose a non-heap pointer (would crash ld.so).
  if ((uintptr_t)handle < 0x10000)
    return 0;
  return dlclose(handle);
}

// AASSETMANAGER EMULATION
// this bit here redirects apk reads directly to the assets folder

typedef struct {
  FILE *f;
  size_t size;
} FakeAsset;

typedef struct {
  DIR *d;
} FakeAssetDir;

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env;
  (void)assetManager;
  return (void *)0x1337;
}

static const char *asset_root(void) {
  const char *e = getenv("SUMMERTIME_ASSETS");
  return (e && e[0]) ? e : "./assets";
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr;
  (void)mode;
  char path[1024];
  if (!filename)
    return NULL;

  const char *relative_path = filename;
  if (strncmp(filename, "assets/", 7) == 0) {
    relative_path = filename + 7;
  } else if (strncmp(filename, "./assets/", 9) == 0) {
    relative_path = filename + 9;
  }
  snprintf(path, sizeof(path), "%s/%s", asset_root(), relative_path);
  FILE *f = fopen(path, "rb");
  if (!f) {
    f = fopen(resolve_android_path(filename), "rb");
  }

  if (getenv("SUMMERTIME_ASSETLOG"))
    debugPrintf("AAssetManager_open(\"%s\" -> \"%s\") = %p\n", filename, path, f);

  if (!f)
    return NULL;

  FakeAsset *asset = malloc(sizeof(FakeAsset));
  asset->f = f;
  fseek(f, 0, SEEK_END);
  asset->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  return asset;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  return fread(buf, 1, count, a->f);
}

void AAsset_close_fake(void *asset) {
  if (!asset)
    return;
  FakeAsset *a = (FakeAsset *)asset;
  fclose(a->f);
  free(a);
}

off_t AAsset_getLength_fake(void *asset) {
  if (!asset)
    return 0;
  FakeAsset *a = (FakeAsset *)asset;
  return a->size;
}

long AAsset_getLength64_fake(void *asset) {
  return (long)AAsset_getLength_fake(asset);
}

off_t AAsset_getRemainingLength_fake(void *asset) {
  if (!asset)
    return 0;
  FakeAsset *a = (FakeAsset *)asset;
  off_t cur = ftell(a->f);
  return a->size - cur;
}

off_t AAsset_seek_fake(void *asset, off_t offset, int whence) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  fseek(a->f, offset, whence);
  return ftell(a->f);
}

long AAsset_seek64_fake(void *asset, long offset, int whence) {
  return (long)AAsset_seek_fake(asset, (off_t)offset, whence);
}

void *AAssetManager_openDir_fake(void *mgr, const char *dirName) {
  (void)mgr;
  char path[1024];
  const char *relative_path = dirName ? dirName : "";
  if (strncmp(relative_path, "assets/", 7) == 0) {
    relative_path += 7;
  } else if (strncmp(relative_path, "./assets/", 9) == 0) {
    relative_path += 9;
  }
  snprintf(path, sizeof(path), "%s/%s", asset_root(), relative_path);
  DIR *d = opendir(path);
  if (!d)
    return NULL;
  FakeAssetDir *adir = malloc(sizeof(FakeAssetDir));
  adir->d = d;
  return adir;
}

const char *AAssetDir_getNextFileName_fake(void *assetDir) {
  if (!assetDir)
    return NULL;
  FakeAssetDir *ad = (FakeAssetDir *)assetDir;
  struct dirent *ent = readdir(ad->d);
  while (ent && ent->d_name[0] == '.')
    ent = readdir(ad->d);
  return ent ? ent->d_name : NULL;
}

void AAssetDir_close_fake(void *assetDir) {
  if (!assetDir)
    return;
  FakeAssetDir *ad = (FakeAssetDir *)assetDir;
  closedir(ad->d);
  free(ad);
}

size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

int dl_iterate_phdr_fake(void *callback, void *data) { return 0; }
static unsigned long getauxval_stub(unsigned long type) { return 0; }
static int my___system_property_get(const char *name, char *value) {
  const char *ret = "";
  if (name && strcmp(name, "ro.build.version.sdk") == 0)
    ret = "28";
  else if (name && strcmp(name, "ro.product.manufacturer") == 0)
    ret = "NextOS";
  else if (name && strcmp(name, "ro.product.model") == 0)
    ret = "NextOS";
  else if (name && strcmp(name, "ro.hardware") == 0)
    ret = "amlogic";
  if (value)
    strcpy(value, ret);
  return (int)strlen(ret);
}
static void __assert2_stub(const char *file, int line, const char *func,
                           const char *expr) {
  debugPrintf("__assert2: %s:%d %s: %s\n", file ? file : "?", line,
              func ? func : "?", expr ? expr : "?");
}
static unsigned int arc4random_fake(void) {
  static unsigned int state = 0x12345678u;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  state ^= (unsigned int)tv.tv_usec + (unsigned int)getpid();
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
static void *AConfiguration_new_fake(void) {
  static int cfg;
  return &cfg;
}
static void AConfiguration_delete_fake(void *config) { (void)config; }
static void AConfiguration_fromAssetManager_fake(void *config, void *mgr) {
  (void)config;
  (void)mgr;
}
static void AConfiguration_getLanguage_fake(void *config, char *out) {
  (void)config;
  if (out) {
    out[0] = 'e';
    out[1] = 'n';
  }
}
static void AConfiguration_getCountry_fake(void *config, char *out) {
  (void)config;
  if (out) {
    out[0] = 'U';
    out[1] = 'S';
  }
}
static void *ALooper_prepare_fake(int opts) {
  (void)opts;
  static int looper;
  return &looper;
}
static void *ALooper_forThread_fake(void) {
  static int looper;
  return &looper;
}
static int ALooper_pollOnce_fake(int timeout_ms, int *out_fd, int *out_events,
                                 void **out_data) {
  (void)timeout_ms;
  if (out_fd)
    *out_fd = -1;
  if (out_events)
    *out_events = 0;
  if (out_data)
    *out_data = NULL;
  return -3; // ALOOPER_POLL_TIMEOUT
}
static void *ASensorManager_getInstance_fake(void) {
  static int mgr;
  return &mgr;
}
static int ASensorManager_getSensorList_fake(void *mgr, void ***list) {
  (void)mgr;
  if (list)
    *list = NULL;
  return 0;
}
static void *ASensorManager_createEventQueue_fake(void *mgr, void *looper,
                                                  int ident, void *callback,
                                                  void *data) {
  (void)mgr;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
  static int queue;
  return &queue;
}
static int ASensorManager_destroyEventQueue_fake(void *mgr, void *queue) {
  (void)mgr;
  (void)queue;
  return 0;
}
static const char *ASensor_getName_fake(void *sensor) {
  (void)sensor;
  return "none";
}
static int ASensor_getType_fake(void *sensor) {
  (void)sensor;
  return 0;
}
static int ASensor_getMinDelay_fake(void *sensor) {
  (void)sensor;
  return 0;
}
static int ASensorEventQueue_enableSensor_fake(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return -1;
}
static int ASensorEventQueue_disableSensor_fake(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}
static int ASensorEventQueue_setEventRate_fake(void *queue, void *sensor,
                                               int usec) {
  (void)queue;
  (void)sensor;
  (void)usec;
  return 0;
}
static int ASensorEventQueue_getEvents_fake(void *queue, void *events,
                                            size_t count) {
  (void)queue;
  (void)events;
  (void)count;
  return 0;
}
static void sincos_stub(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}
static void sincosf_stub(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}
static long double strtold_l_stub(const char *nptr, char **endptr, void *loc) {
  return strtold(nptr, endptr);
}
static long long strtoll_l_stub(const char *nptr, char **endptr, int base,
                                void *loc) {
  return strtoll(nptr, endptr, base);
}
static unsigned long long strtoull_l_stub(const char *nptr, char **endptr,
                                          int base, void *loc) {
  return strtoull(nptr, endptr, base);
}
static void *newlocale_stub(int category_mask, const char *locale, void *base) {
  return NULL;
}
static void freelocale_stub(void *locobj) {}
static void *uselocale_stub(void *newloc) { return NULL; }
static int initgroups_stub(const char *user, gid_t group) { return 0; }
static int sysinfo_stub(void *info) { return 0; }
static void syslog_stub(int priority, const char *format, ...) {}
static void closelog_stub(void) {}
static void openlog_stub(const char *ident, int option, int facility) {}
static int tcgetattr_stub(int fd, void *termios_p) { return 0; }
static int tcsetattr_stub(int fd, int optional_actions, const void *termios_p) {
  return 0;
}
static int mlock_stub(const void *addr, size_t len) { return 0; }
static void *funopen_stub(const void *cookie,
                          int (*readfn)(void *, char *, int),
                          int (*writefn)(void *, const char *, int),
                          long (*seekfn)(void *, long, int),
                          int (*closefn)(void *)) {
  return NULL;
}
static int sigsetjmp_stub(void *env, int savesigs) { return 0; }
static void siglongjmp_stub(void *env, int val) {}

static int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum;
  (void)act;
  (void)oldact;
  return 0;
}

/* sigset_t: bionic tem 8 bytes; o da glibc tem 128. A glibc ate a 2.32 faz
 * memset dos 128 no sigemptyset/sigfillset -> 120 bytes de ZEROS por cima do
 * frame do chamador bionic (x29/x30 salvos!) -> ret para PC=0 no boot do
 * Python (glibc 2.33+ so toca 8 bytes, por isso o NextOS nunca viu isso).
 * Wrappers com semantica de sigset64 do bionic: operam SO nos 8 bytes. */
static int my_sigemptyset(void *set) {
  if (!set) { errno = EINVAL; return -1; }
  memset(set, 0, 8);
  return 0;
}

static int my_sigfillset(void *set) {
  if (!set) { errno = EINVAL; return -1; }
  memset(set, 0xff, 8);
  return 0;
}

static int my_sigaddset(void *set, int sig) {
  if (!set || sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  *(unsigned long *)set |= 1UL << (sig - 1);
  return 0;
}

static int my_sigdelset(void *set, int sig) {
  if (!set || sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  *(unsigned long *)set &= ~(1UL << (sig - 1));
  return 0;
}

static int my_sigismember(const void *set, int sig) {
  if (!set || sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  return (*(const unsigned long *)set >> (sig - 1)) & 1UL;
}

static int my_sigpending(void *set) {
  if (!set) { errno = EINVAL; return -1; }
  return (int)syscall(SYS_rt_sigpending, set, 8);
}

static int my_sigprocmask(int how, const void *set, void *old) {
  return (int)syscall(SYS_rt_sigprocmask, how, set, old, 8);
}

static int my_sigwait(const void *set, int *sig) {
  /* rt_sigtimedwait com sigset de 8 bytes; repete em EINTR como sigwait */
  for (;;) {
    long r = syscall(SYS_rt_sigtimedwait, set, NULL, NULL, 8);
    if (r >= 0) {
      if (sig) *sig = (int)r;
      return 0;
    }
    if (errno != EINTR)
      return errno;
  }
}

DynLibFunction dynlib_functions[] = {
    {"abort", (uintptr_t)&abort},
    {"accept", (uintptr_t)&accept},
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"__android_log_write", (uintptr_t)&__android_log_write_fake},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_fake},
    {"__assert2", (uintptr_t)&__assert2_stub},
    {"arc4random", (uintptr_t)&arc4random_fake},
    {"asin", (uintptr_t)&asin},
    {"atan", (uintptr_t)&atan},
    {"atan2f", (uintptr_t)&atan2f},
    {"atanf", (uintptr_t)&atanf},
    {"atof", (uintptr_t)&atof},
    {"atoi", (uintptr_t)&atoi},
    {"atoll", (uintptr_t)&atoll},
    {"bind", (uintptr_t)&bind},
    {"btowc", (uintptr_t)&btowc},
    {"calloc", (uintptr_t)&my_calloc},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"close", (uintptr_t)&close},
    {"closedir", (uintptr_t)&closedir},
    {"closelog", (uintptr_t)&closelog_stub},
    {"connect", (uintptr_t)&connect},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"dlclose", (uintptr_t)&my_dlclose},
    {"dlerror", (uintptr_t)&dlerror},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"dlopen", (uintptr_t)&my_dlopen},
    {"dlsym", (uintptr_t)&my_dlsym},
    {"__errno", (uintptr_t)&__errno_fake},
    {"__get_h_errno", (uintptr_t)&__get_h_errno_fake},
    {"_exit", (uintptr_t)&_exit},
    {"exit", (uintptr_t)&exit},
    {"exp", (uintptr_t)&exp},
    {"expf", (uintptr_t)&expf},
    {"fclose", (uintptr_t)&fclose},
    {"fcntl", (uintptr_t)&fcntl},
    {"__FD_CLR_chk", (uintptr_t)&__FD_CLR_chk},
    {"__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk},
    {"__FD_SET_chk", (uintptr_t)&__FD_SET_chk},
    {"feof", (uintptr_t)&sf_feof},
    {"ferror", (uintptr_t)&sf_ferror},
    {"fflush", (uintptr_t)&sf_fflush},
    {"fgetc", (uintptr_t)&sf_fgetc},
    {"fgets", (uintptr_t)&sf_fgets},
    {"fileno", (uintptr_t)&sf_fileno},
    {"fmodf", (uintptr_t)&fmodf},
    {"fopen", (uintptr_t)&my_fopen},
    {"fprintf", (uintptr_t)&sf_fprintf},
    {"fputc", (uintptr_t)&sf_fputc},
    {"fputs", (uintptr_t)&sf_fputs},
    {"fread", (uintptr_t)&fread},
    {"free", (uintptr_t)&my_free},
    {"freeaddrinfo", (uintptr_t)&freeaddrinfo},
    {"freelocale", (uintptr_t)&freelocale_stub},
    {"fseek", (uintptr_t)&fseek},
    {"fseeko", (uintptr_t)&fseeko},
    {"fstat", (uintptr_t)&fstat},
    {"ftell", (uintptr_t)&ftell},
    {"ftello", (uintptr_t)&ftello},
    {"ftruncate", (uintptr_t)&ftruncate},
    {"funopen", (uintptr_t)&funopen_stub},
    {"fwrite", (uintptr_t)&sf_fwrite},
    {"gai_strerror", (uintptr_t)&gai_strerror},
    {"getaddrinfo", (uintptr_t)&getaddrinfo},
    {"getauxval", (uintptr_t)&getauxval_stub},
    {"getenv", (uintptr_t)&getenv},
    {"gethostbyname", (uintptr_t)&gethostbyname},
    {"getnameinfo", (uintptr_t)&getnameinfo},
    {"getpagesize", (uintptr_t)&getpagesize},
    {"getpeername", (uintptr_t)&getpeername},
    {"getpid", (uintptr_t)&getpid},
    {"getpwuid", (uintptr_t)&getpwuid},
    {"getrlimit", (uintptr_t)&getrlimit},
    {"getsockname", (uintptr_t)&getsockname},
    {"getsockopt", (uintptr_t)&getsockopt},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"getuid", (uintptr_t)&getuid},

    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&wrap_glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&wrap_glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&wrap_glBindTexture},
    {"glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate},
    {"glBufferData", (uintptr_t)&wrap_glBufferData},
    {"glBufferSubData", (uintptr_t)&wrap_glBufferSubData},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&wrap_glClear},
    {"glClearColor", (uintptr_t)&wrap_glClearColor},
    {"glColor4f", (uintptr_t)&glColor4f},
    {"glClearDepthf", (uintptr_t)&glClearDepthf},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glColorPointer", (uintptr_t)&glColorPointer},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D},
    {"glCreateProgram", (uintptr_t)&glCreateProgram},
    {"glCreateShader", (uintptr_t)&glCreateShader},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteBuffers", (uintptr_t)&wrap_glDeleteBuffers},
    {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
    {"glDeleteProgram", (uintptr_t)&glDeleteProgram},
    {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},
    {"glDeleteShader", (uintptr_t)&glDeleteShader},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableClientState", (uintptr_t)&glDisableClientState},
    {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},
    {"glDrawArrays", (uintptr_t)&wrap_glDrawArrays},
    {"glDrawElements", (uintptr_t)&wrap_glDrawElements},
    {"glDrawTexfOES", (uintptr_t)&wrap_glDrawTexfOES},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableClientState", (uintptr_t)&glEnableClientState},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
    {"glGenBuffers", (uintptr_t)&glGenBuffers},
    {"glGenerateMipmap", (uintptr_t)&wrap_glGenerateMipmap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetFramebufferAttachmentParameteriv",
     (uintptr_t)&glGetFramebufferAttachmentParameteriv},
    {"glFinish", (uintptr_t)&glFinish},
    {"glGetAttribLocation", (uintptr_t)&glGetAttribLocation},
    {"glGetError", (uintptr_t)&glGetError},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glLoadIdentity", (uintptr_t)&glLoadIdentity},
    {"glMatrixMode", (uintptr_t)&glMatrixMode},
    {"glOrthof", (uintptr_t)&glOrthof},
    {"glPixelStorei", (uintptr_t)&wrap_glPixelStorei},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderBinary", (uintptr_t)&glShaderBinary},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexImage2D", (uintptr_t)&wrap_glTexImage2D},
    {"glTexCoordPointer", (uintptr_t)&glTexCoordPointer},
    {"glTexEnvf", (uintptr_t)&glTexEnvf},
    {"glTexParameteri", (uintptr_t)&wrap_glTexParameteri},
    {"glTexParameteriv", (uintptr_t)&glTexParameteriv},
    {"glTexSubImage2D", (uintptr_t)&wrap_glTexSubImage2D},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform2fv", (uintptr_t)&glUniform2fv},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform4f", (uintptr_t)&glUniform4f},
    {"glUniform4fv", (uintptr_t)&glUniform4fv},
    {"glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},
    {"glUniformMatrix4fv", (uintptr_t)&wrap_glUniformMatrix4fv},
    {"glUseProgram", (uintptr_t)&wrap_glUseProgram},
    {"glVertexAttribPointer", (uintptr_t)&wrap_glVertexAttribPointer},
    {"glVertexPointer", (uintptr_t)&glVertexPointer},
    {"glViewport", (uintptr_t)&wrap_glViewport},

    {"gmtime_r", (uintptr_t)&gmtime_r},
    {"inet_ntop", (uintptr_t)&inet_ntop},
    {"inet_pton", (uintptr_t)&inet_pton},
    {"initgroups", (uintptr_t)&initgroups_stub},
    {"ioctl", (uintptr_t)&ioctl},
    {"isalnum", (uintptr_t)&isalnum},
    {"isalpha", (uintptr_t)&isalpha},
    {"islower", (uintptr_t)&islower},
    {"isspace", (uintptr_t)&isspace},
    {"isupper", (uintptr_t)&isupper},
    {"iswalpha", (uintptr_t)&iswalpha},
    {"iswblank", (uintptr_t)&iswblank},
    {"iswcntrl", (uintptr_t)&iswcntrl},
    {"iswdigit", (uintptr_t)&iswdigit},
    {"iswlower", (uintptr_t)&iswlower},
    {"iswprint", (uintptr_t)&iswprint},
    {"iswpunct", (uintptr_t)&iswpunct},
    {"iswspace", (uintptr_t)&iswspace},
    {"iswupper", (uintptr_t)&iswupper},
    {"iswxdigit", (uintptr_t)&iswxdigit},
    {"isxdigit", (uintptr_t)&isxdigit},
    {"kill", (uintptr_t)&kill},
    {"ldexp", (uintptr_t)&ldexp},
    {"ldexpf", (uintptr_t)&ldexpf},
    {"listen", (uintptr_t)&listen},
    {"localeconv", (uintptr_t)&localeconv},
    {"localtime", (uintptr_t)&localtime},
    {"localtime_r", (uintptr_t)&localtime_r},
    {"log", (uintptr_t)&log},
    {"log10", (uintptr_t)&log10},
    {"log10f", (uintptr_t)&log10f},
    {"logf", (uintptr_t)&logf},
    {"lseek", (uintptr_t)&lseek},
    {"madvise", (uintptr_t)&madvise},
    {"malloc", (uintptr_t)&my_malloc},
    {"mbrlen", (uintptr_t)&my_mbrlen},
    {"mbrtowc", (uintptr_t)&my_mbrtowc},
    {"mbsinit", (uintptr_t)&my_mbsinit},
    {"mbsnrtowcs", (uintptr_t)&my_mbsnrtowcs},
    {"mbsrtowcs", (uintptr_t)&my_mbsrtowcs},
    {"mbstowcs", (uintptr_t)&mbstowcs},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"memchr", (uintptr_t)&memchr},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"memmove", (uintptr_t)&memmove},
    {"__memmove_chk", (uintptr_t)&__memmove_chk},
    {"memset", (uintptr_t)&memset},
    {"mkdir", (uintptr_t)&mkdir_fake},
    {"mktime", (uintptr_t)&mktime},
    {"mlock", (uintptr_t)&mlock_stub},
    {"mmap", (uintptr_t)&mmap},
    {"modf", (uintptr_t)&modf},
    {"mprotect", (uintptr_t)&mprotect},
    {"munmap", (uintptr_t)&munmap},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"newlocale", (uintptr_t)&newlocale_stub},
    {"open", (uintptr_t)&my_open},
    {"__open_2", (uintptr_t)&my_open_2},
    {"opendir", (uintptr_t)&opendir},
    {"openlog", (uintptr_t)&openlog_stub},
    {"perror", (uintptr_t)&perror},
    {"pipe", (uintptr_t)&pipe},
    {"poll", (uintptr_t)&poll},
    {"posix_memalign", (uintptr_t)&my_posix_memalign},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"printf", (uintptr_t)&printf},

    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_kill", (uintptr_t)&pthread_kill},
    {"pthread_mutexattr_destroy", (uintptr_t)&ret0},
    {"pthread_mutexattr_init", (uintptr_t)&ret0},
    {"pthread_mutexattr_settype", (uintptr_t)&ret0},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy},
    {"pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},

    {"puts", (uintptr_t)&puts},
    {"qsort", (uintptr_t)&qsort},
    {"rand", (uintptr_t)&rand},
    {"read", (uintptr_t)&read},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"readdir", (uintptr_t)&readdir},
    {"realloc", (uintptr_t)&my_realloc},
    {"recv", (uintptr_t)&recv},
    {"recvfrom", (uintptr_t)&recvfrom},
    {"remove", (uintptr_t)&remove_fake},
    {"rename", (uintptr_t)&rename_fake},
    {"rewind", (uintptr_t)&rewind},
    {"rmdir", (uintptr_t)&rmdir},
    {"sched_yield", (uintptr_t)&sched_yield},
    {"select", (uintptr_t)&select},
    {"send", (uintptr_t)&send},
    {"sendto", (uintptr_t)&sendto},
    {"setgid", (uintptr_t)&setgid},
    {"setlocale", (uintptr_t)&setlocale},
    {"setsockopt", (uintptr_t)&setsockopt},
    {"setuid", (uintptr_t)&setuid},
    {"__sF", (uintptr_t)&fake_sF},
    {"shutdown", (uintptr_t)&shutdown},
    {"sigaction", (uintptr_t)&sigaction_fake},
    {"sigaddset", (uintptr_t)&my_sigaddset},
    {"sigdelset", (uintptr_t)&my_sigdelset},
    {"sigemptyset", (uintptr_t)&my_sigemptyset},
    {"sigismember", (uintptr_t)&my_sigismember},
    {"sigpending", (uintptr_t)&my_sigpending},
    {"sigwait", (uintptr_t)&my_sigwait},
    {"sigfillset", (uintptr_t)&my_sigfillset},
    {"siglongjmp", (uintptr_t)&siglongjmp_stub},
    {"signal", (uintptr_t)&signal},
    {"sigprocmask", (uintptr_t)&my_sigprocmask},
    {"sigsetjmp", (uintptr_t)&sigsetjmp_stub},
    {"sin", (uintptr_t)&sin},
    {"sincos", (uintptr_t)&sincos_stub},
    {"sincosf", (uintptr_t)&sincosf_stub},
    {"sinf", (uintptr_t)&sinf},

    {"snprintf", (uintptr_t)&snprintf},
    {"socket", (uintptr_t)&socket},
    {"sprintf", (uintptr_t)&sprintf},
    {"srand", (uintptr_t)&srand},
    {"sscanf", (uintptr_t)&sscanf},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_stub},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"stat", (uintptr_t)&my_stat},
    {"lstat", (uintptr_t)&my_lstat},
    {"fstatat", (uintptr_t)&my_fstatat},
    {"mknod", (uintptr_t)&my_mknod},
    {"mknodat", (uintptr_t)&my_mknodat},
    {"strlcpy", (uintptr_t)&ss_strlcpy},
    {"strlcat", (uintptr_t)&ss_strlcat},
    {"wcslcpy", (uintptr_t)&ss_wcslcpy},
    {"wcslcat", (uintptr_t)&ss_wcslcat},
    {"access", (uintptr_t)&my_access},
    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strcat", (uintptr_t)&strcat},
    {"__strcat_chk", (uintptr_t)&__strcat_chk},
    {"strchr", (uintptr_t)&strchr},
    {"__strchr_chk", (uintptr_t)&__strchr_chk},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcoll", (uintptr_t)&strcoll},
    {"strcpy", (uintptr_t)&strcpy},
    {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
    {"strcspn", (uintptr_t)&strcspn},
    {"strdup", (uintptr_t)&my_strdup},
    {"strerror", (uintptr_t)&strerror},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"strftime", (uintptr_t)&strftime},
    {"strlen", (uintptr_t)&strlen},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"strncasecmp", (uintptr_t)&strncasecmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"__strncpy_chk", (uintptr_t)&__strncpy_chk},
    {"__strncpy_chk2", (uintptr_t)&__strncpy_chk2},
    {"strrchr", (uintptr_t)&strrchr},
    {"__strrchr_chk", (uintptr_t)&__strrchr_chk},
    {"strspn", (uintptr_t)&strspn},
    {"strstr", (uintptr_t)&strstr},
    {"strtod", (uintptr_t)&strtod},
    {"strtof", (uintptr_t)&strtof},
    {"strtol", (uintptr_t)&strtol},
    {"strtold", (uintptr_t)&strtold},
    {"strtold_l", (uintptr_t)&strtold_l_stub},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoll_l", (uintptr_t)&strtoll_l_stub},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtoull_l", (uintptr_t)&strtoull_l_stub},
    {"strxfrm", (uintptr_t)&strxfrm},
    {"swprintf", (uintptr_t)&swprintf},
    {"syscall", (uintptr_t)&syscall},
    {"sysconf", (uintptr_t)&sysconf},
    {"sysinfo", (uintptr_t)&sysinfo_stub},
    {"syslog", (uintptr_t)&syslog_stub},
    {"__system_property_get", (uintptr_t)&my___system_property_get},
    {"system", (uintptr_t)&system},
    {"tan", (uintptr_t)&tan},
    {"tanf", (uintptr_t)&tanf},
    {"tcgetattr", (uintptr_t)&tcgetattr_stub},
    {"tcsetattr", (uintptr_t)&tcsetattr_stub},
    {"time", (uintptr_t)&time},
    {"tolower", (uintptr_t)&tolower},
    {"toupper", (uintptr_t)&toupper},
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"uname", (uintptr_t)&uname},
    {"unlink", (uintptr_t)&unlink},
    {"uselocale", (uintptr_t)&uselocale_stub},
    {"usleep", (uintptr_t)&usleep},
    {"vasprintf", (uintptr_t)&my_vasprintf},
    {"vfprintf", (uintptr_t)&sf_vfprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"vswprintf", (uintptr_t)&vswprintf},
    {"wcrtomb", (uintptr_t)&my_wcrtomb},
    {"wcscat", (uintptr_t)&wcscat},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcscpy", (uintptr_t)&wcscpy},
    {"wcslen", (uintptr_t)&wcslen},
    {"wcsnrtombs", (uintptr_t)&my_wcsnrtombs},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstold", (uintptr_t)&wcstold},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstombs", (uintptr_t)&wcstombs},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"wctob", (uintptr_t)&wctob},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"write", (uintptr_t)&write},

    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake},
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
    {"AAsset_close", (uintptr_t)&AAsset_close_fake},
    {"AAsset_read", (uintptr_t)&AAsset_read_fake},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
    {"AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake},
    {"AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_fake},
    {"AAsset_seek64", (uintptr_t)&AAsset_seek64_fake},
    {"AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake},
    {"AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake},
    {"AAssetDir_close", (uintptr_t)&AAssetDir_close_fake},

    {"ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake},
    {"ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake},
    {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake},
    {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake},
    {"ANativeWindow_getFormat", (uintptr_t)&ANativeWindow_getFormat_fake},
    {"ANativeWindow_setBuffersGeometry",
     (uintptr_t)&ANativeWindow_setBuffersGeometry_fake},

    {"AConfiguration_new", (uintptr_t)&AConfiguration_new_fake},
    {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete_fake},
    {"AConfiguration_fromAssetManager",
     (uintptr_t)&AConfiguration_fromAssetManager_fake},
    {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage_fake},
    {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry_fake},
    {"ALooper_prepare", (uintptr_t)&ALooper_prepare_fake},
    {"ALooper_forThread", (uintptr_t)&ALooper_forThread_fake},
    {"ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce_fake},
    {"ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance_fake},
    {"ASensorManager_getSensorList",
     (uintptr_t)&ASensorManager_getSensorList_fake},
    {"ASensorManager_createEventQueue",
     (uintptr_t)&ASensorManager_createEventQueue_fake},
    {"ASensorManager_destroyEventQueue",
     (uintptr_t)&ASensorManager_destroyEventQueue_fake},
    {"ASensor_getName", (uintptr_t)&ASensor_getName_fake},
    {"ASensor_getType", (uintptr_t)&ASensor_getType_fake},
    {"ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay_fake},
    {"ASensorEventQueue_enableSensor",
     (uintptr_t)&ASensorEventQueue_enableSensor_fake},
    {"ASensorEventQueue_disableSensor",
     (uintptr_t)&ASensorEventQueue_disableSensor_fake},
    {"ASensorEventQueue_setEventRate",
     (uintptr_t)&ASensorEventQueue_setEventRate_fake},
    {"ASensorEventQueue_getEvents",
     (uintptr_t)&ASensorEventQueue_getEvents_fake},

    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},
    {"SL_IID_RECORD", (uintptr_t)&sl_IID_RECORD_fake},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},

    // OES framebuffer/blend aliases -> GLES2 core (identical on ES2).
    {"glBindFramebufferOES", (uintptr_t)&wrap_glBindFramebuffer},
    {"glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers},
    {"glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers},
    {"glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus},
    {"glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D},
    {"glBlendEquationOES", (uintptr_t)&glBlendEquation},
    {"glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate},
    {"glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate},
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
