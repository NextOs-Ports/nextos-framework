#define _GNU_SOURCE
#include "huntdown_video_gl.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only the GLES2 subset this presenter needs.  Resolving through SDL keeps the
 * loader independent of which libGLESv2 the firmware installs. */
#define HD_GL_TEXTURE_2D 0x0DE1
#define HD_GL_TEXTURE0 0x84C0
#define HD_GL_RGBA 0x1908
#define HD_GL_UNSIGNED_BYTE 0x1401
#define HD_GL_TEXTURE_MIN_FILTER 0x2801
#define HD_GL_TEXTURE_MAG_FILTER 0x2800
#define HD_GL_TEXTURE_WRAP_S 0x2802
#define HD_GL_TEXTURE_WRAP_T 0x2803
#define HD_GL_LINEAR 0x2601
#define HD_GL_CLAMP_TO_EDGE 0x812F
#define HD_GL_ARRAY_BUFFER 0x8892
#define HD_GL_STATIC_DRAW 0x88E4
#define HD_GL_FRAGMENT_SHADER 0x8B30
#define HD_GL_VERTEX_SHADER 0x8B31
#define HD_GL_COMPILE_STATUS 0x8B81
#define HD_GL_LINK_STATUS 0x8B82
#define HD_GL_FLOAT 0x1406
#define HD_GL_TRIANGLE_STRIP 0x0005
#define HD_GL_COLOR_BUFFER_BIT 0x00004000
#define HD_GL_DEPTH_TEST 0x0B71
#define HD_GL_BLEND 0x0BE2
#define HD_GL_CULL_FACE 0x0B44
#define HD_GL_SCISSOR_TEST 0x0C11
#define HD_GL_COLOR_CLEAR_VALUE 0x0C22
#define HD_GL_COLOR_WRITEMASK 0x0C23
#define HD_GL_CURRENT_PROGRAM 0x8B8D
#define HD_GL_ARRAY_BUFFER_BINDING 0x8894
#define HD_GL_TEXTURE_BINDING_2D 0x8069
#define HD_GL_ACTIVE_TEXTURE 0x84E0
#define HD_GL_FRAMEBUFFER 0x8D40
#define HD_GL_FRAMEBUFFER_BINDING 0x8CA6
#define HD_GL_VIEWPORT 0x0BA2
#define HD_GL_UNPACK_ALIGNMENT 0x0CF5
#define HD_GL_VERTEX_ARRAY_BINDING 0x85B5
#define HD_GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define HD_GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623
#define HD_GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624
#define HD_GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625
#define HD_GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A
#define HD_GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define HD_GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F

struct hd_gl {
  void (*ActiveTexture)(unsigned);
  void (*AttachShader)(unsigned, unsigned);
  void (*BindAttribLocation)(unsigned, unsigned, const char *);
  void (*BindBuffer)(unsigned, unsigned);
  void (*BindFramebuffer)(unsigned, unsigned);
  void (*BindTexture)(unsigned, unsigned);
  void (*BufferData)(unsigned, long, const void *, unsigned);
  void (*Clear)(unsigned);
  void (*ClearColor)(float, float, float, float);
  void (*ColorMask)(unsigned char, unsigned char, unsigned char,
                    unsigned char);
  void (*CompileShader)(unsigned);
  unsigned (*CreateProgram)(void);
  unsigned (*CreateShader)(unsigned);
  void (*DeleteBuffers)(int, const unsigned *);
  void (*DeleteProgram)(unsigned);
  void (*DeleteShader)(unsigned);
  void (*DeleteTextures)(int, const unsigned *);
  void (*Disable)(unsigned);
  void (*DisableVertexAttribArray)(unsigned);
  void (*Enable)(unsigned);
  void (*DrawArrays)(unsigned, int, int);
  void (*EnableVertexAttribArray)(unsigned);
  void (*GenBuffers)(int, unsigned *);
  void (*GenTextures)(int, unsigned *);
  void (*GetBooleanv)(unsigned, unsigned char *);
  void (*GetFloatv)(unsigned, float *);
  void (*GetIntegerv)(unsigned, int *);
  void (*GetProgramInfoLog)(unsigned, int, int *, char *);
  void (*GetProgramiv)(unsigned, unsigned, int *);
  void (*GetShaderInfoLog)(unsigned, int, int *, char *);
  void (*GetShaderiv)(unsigned, unsigned, int *);
  int (*GetUniformLocation)(unsigned, const char *);
  void (*GetVertexAttribiv)(unsigned, unsigned, int *);
  void (*GetVertexAttribPointerv)(unsigned, unsigned, void **);
  unsigned char (*IsEnabled)(unsigned);
  void (*LinkProgram)(unsigned);
  void (*PixelStorei)(unsigned, int);
  void (*ShaderSource)(unsigned, int, const char *const *, const int *);
  void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned,
                     const void *);
  void (*TexParameteri)(unsigned, unsigned, int);
  void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned,
                        const void *);
  void (*Uniform1i)(int, int);
  void (*UseProgram)(unsigned);
  void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int,
                              const void *);
  void (*Viewport)(int, int, int, int);
  /* Core in GLES3 and exposed with the OES suffix on GLES2 drivers.  These
   * are optional because the complete vertex-attrib fallback below also
   * isolates the presenter on implementations without VAOs. */
  void (*BindVertexArray)(unsigned);
  void (*DeleteVertexArrays)(int, const unsigned *);
  void (*GenVertexArrays)(int, unsigned *);
};

static struct hd_gl g_gl;
static int g_resolved;
static int g_failed;
static unsigned g_program;
static unsigned g_texture;
static unsigned g_buffer;
static unsigned g_vertex_array;
static int g_sampler;
static int g_width;
static int g_height;

static const char *const HD_VERTEX_SOURCE =
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "  vTexCoord = aTexCoord;\n"
    "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "}\n";

static const char *const HD_FRAGMENT_SOURCE =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D uFrame;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uFrame, vTexCoord);\n"
    "}\n";

static void *hd_gl_symbol(const char *name) {
  void *address = SDL_GL_GetProcAddress(name);
  if (!address) fprintf(stderr, "[HD-VIDEO] GL: %s ausente\n", name);
  return address;
}

static int hd_gl_resolve(void) {
  if (g_resolved) return !g_failed;
  g_resolved = 1;
#define HD_GL_LOAD(field, name)                                   \
  do {                                                            \
    *(void **)&g_gl.field = hd_gl_symbol(name);                   \
    if (!g_gl.field) g_failed = 1;                                \
  } while (0)
  HD_GL_LOAD(ActiveTexture, "glActiveTexture");
  HD_GL_LOAD(AttachShader, "glAttachShader");
  HD_GL_LOAD(BindAttribLocation, "glBindAttribLocation");
  HD_GL_LOAD(BindBuffer, "glBindBuffer");
  HD_GL_LOAD(BindFramebuffer, "glBindFramebuffer");
  HD_GL_LOAD(BindTexture, "glBindTexture");
  HD_GL_LOAD(BufferData, "glBufferData");
  HD_GL_LOAD(Clear, "glClear");
  HD_GL_LOAD(ClearColor, "glClearColor");
  HD_GL_LOAD(ColorMask, "glColorMask");
  HD_GL_LOAD(CompileShader, "glCompileShader");
  HD_GL_LOAD(CreateProgram, "glCreateProgram");
  HD_GL_LOAD(CreateShader, "glCreateShader");
  HD_GL_LOAD(DeleteBuffers, "glDeleteBuffers");
  HD_GL_LOAD(DeleteProgram, "glDeleteProgram");
  HD_GL_LOAD(DeleteShader, "glDeleteShader");
  HD_GL_LOAD(DeleteTextures, "glDeleteTextures");
  HD_GL_LOAD(Disable, "glDisable");
  HD_GL_LOAD(DisableVertexAttribArray, "glDisableVertexAttribArray");
  HD_GL_LOAD(Enable, "glEnable");
  HD_GL_LOAD(DrawArrays, "glDrawArrays");
  HD_GL_LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
  HD_GL_LOAD(GenBuffers, "glGenBuffers");
  HD_GL_LOAD(GenTextures, "glGenTextures");
  HD_GL_LOAD(GetBooleanv, "glGetBooleanv");
  HD_GL_LOAD(GetFloatv, "glGetFloatv");
  HD_GL_LOAD(GetIntegerv, "glGetIntegerv");
  HD_GL_LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
  HD_GL_LOAD(GetProgramiv, "glGetProgramiv");
  HD_GL_LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
  HD_GL_LOAD(GetShaderiv, "glGetShaderiv");
  HD_GL_LOAD(GetUniformLocation, "glGetUniformLocation");
  HD_GL_LOAD(GetVertexAttribiv, "glGetVertexAttribiv");
  HD_GL_LOAD(GetVertexAttribPointerv, "glGetVertexAttribPointerv");
  HD_GL_LOAD(IsEnabled, "glIsEnabled");
  HD_GL_LOAD(LinkProgram, "glLinkProgram");
  HD_GL_LOAD(PixelStorei, "glPixelStorei");
  HD_GL_LOAD(ShaderSource, "glShaderSource");
  HD_GL_LOAD(TexImage2D, "glTexImage2D");
  HD_GL_LOAD(TexParameteri, "glTexParameteri");
  HD_GL_LOAD(TexSubImage2D, "glTexSubImage2D");
  HD_GL_LOAD(Uniform1i, "glUniform1i");
  HD_GL_LOAD(UseProgram, "glUseProgram");
  HD_GL_LOAD(VertexAttribPointer, "glVertexAttribPointer");
  HD_GL_LOAD(Viewport, "glViewport");
#undef HD_GL_LOAD

  *(void **)&g_gl.BindVertexArray = SDL_GL_GetProcAddress("glBindVertexArray");
  *(void **)&g_gl.DeleteVertexArrays =
      SDL_GL_GetProcAddress("glDeleteVertexArrays");
  *(void **)&g_gl.GenVertexArrays = SDL_GL_GetProcAddress("glGenVertexArrays");
  if (!g_gl.BindVertexArray || !g_gl.DeleteVertexArrays ||
      !g_gl.GenVertexArrays) {
    *(void **)&g_gl.BindVertexArray =
        SDL_GL_GetProcAddress("glBindVertexArrayOES");
    *(void **)&g_gl.DeleteVertexArrays =
        SDL_GL_GetProcAddress("glDeleteVertexArraysOES");
    *(void **)&g_gl.GenVertexArrays =
        SDL_GL_GetProcAddress("glGenVertexArraysOES");
  }
  return !g_failed;
}

static unsigned hd_gl_compile(unsigned type, const char *source) {
  unsigned shader = g_gl.CreateShader(type);
  if (!shader) return 0;
  g_gl.ShaderSource(shader, 1, &source, NULL);
  g_gl.CompileShader(shader);
  int status = 0;
  g_gl.GetShaderiv(shader, HD_GL_COMPILE_STATUS, &status);
  if (!status) {
    char log[512] = {0};
    g_gl.GetShaderInfoLog(shader, (int)sizeof log - 1, NULL, log);
    fprintf(stderr, "[HD-VIDEO] GL: shader %s falhou: %s\n",
            type == HD_GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    g_gl.DeleteShader(shader);
    return 0;
  }
  return shader;
}

static int hd_gl_build_program(void) {
  if (g_program) return 1;
  unsigned vertex = hd_gl_compile(HD_GL_VERTEX_SHADER, HD_VERTEX_SOURCE);
  unsigned fragment = hd_gl_compile(HD_GL_FRAGMENT_SHADER, HD_FRAGMENT_SOURCE);
  if (!vertex || !fragment) {
    if (vertex) g_gl.DeleteShader(vertex);
    if (fragment) g_gl.DeleteShader(fragment);
    return 0;
  }
  unsigned program = g_gl.CreateProgram();
  if (!program) {
    g_gl.DeleteShader(vertex);
    g_gl.DeleteShader(fragment);
    return 0;
  }
  g_gl.AttachShader(program, vertex);
  g_gl.AttachShader(program, fragment);
  g_gl.BindAttribLocation(program, 0, "aPosition");
  g_gl.BindAttribLocation(program, 1, "aTexCoord");
  g_gl.LinkProgram(program);
  g_gl.DeleteShader(vertex);
  g_gl.DeleteShader(fragment);
  int status = 0;
  g_gl.GetProgramiv(program, HD_GL_LINK_STATUS, &status);
  if (!status) {
    char log[512] = {0};
    g_gl.GetProgramInfoLog(program, (int)sizeof log - 1, NULL, log);
    fprintf(stderr, "[HD-VIDEO] GL: link falhou: %s\n", log);
    g_gl.DeleteProgram(program);
    return 0;
  }
  g_program = program;
  g_sampler = g_gl.GetUniformLocation(program, "uFrame");

  /* The decoder already letterboxes to the drawable, so a single full-screen
   * strip is enough.  V is flipped because the raw frames are top-down. */
  static const float quad[16] = {
      -1.0f, -1.0f, 0.0f, 1.0f,
       1.0f, -1.0f, 1.0f, 1.0f,
      -1.0f,  1.0f, 0.0f, 0.0f,
       1.0f,  1.0f, 1.0f, 0.0f,
  };
  g_gl.GenBuffers(1, &g_buffer);
  if (!g_buffer) return 0;
  int previous_buffer = 0;
  g_gl.GetIntegerv(HD_GL_ARRAY_BUFFER_BINDING, &previous_buffer);
  g_gl.BindBuffer(HD_GL_ARRAY_BUFFER, g_buffer);
  g_gl.BufferData(HD_GL_ARRAY_BUFFER, (long)sizeof quad, quad,
                  HD_GL_STATIC_DRAW);
  g_gl.BindBuffer(HD_GL_ARRAY_BUFFER, (unsigned)previous_buffer);
  return 1;
}

int hd_video_gl_begin(int width, int height) {
  if (width <= 0 || height <= 0) return 0;
  if (!hd_gl_resolve()) return 0;
  if (!hd_gl_build_program()) return 0;

  if (g_gl.GenVertexArrays && !g_vertex_array)
    g_gl.GenVertexArrays(1, &g_vertex_array);

  if (g_texture && (width != g_width || height != g_height)) {
    g_gl.DeleteTextures(1, &g_texture);
    g_texture = 0;
  }
  if (!g_texture) {
    int previous_texture = 0, previous_unit = 0;
    g_gl.GetIntegerv(HD_GL_ACTIVE_TEXTURE, &previous_unit);
    g_gl.ActiveTexture(HD_GL_TEXTURE0);
    g_gl.GetIntegerv(HD_GL_TEXTURE_BINDING_2D, &previous_texture);
    g_gl.GenTextures(1, &g_texture);
    if (!g_texture) {
      g_gl.ActiveTexture((unsigned)previous_unit);
      return 0;
    }
    g_gl.BindTexture(HD_GL_TEXTURE_2D, g_texture);
    /* Clamp and the linear filter belong to this texture only.  Nothing here
     * changes the global sampler policy the engine relies on. */
    g_gl.TexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_MIN_FILTER,
                       HD_GL_LINEAR);
    g_gl.TexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_MAG_FILTER,
                       HD_GL_LINEAR);
    g_gl.TexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_WRAP_S,
                       HD_GL_CLAMP_TO_EDGE);
    g_gl.TexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_WRAP_T,
                       HD_GL_CLAMP_TO_EDGE);
    g_gl.TexImage2D(HD_GL_TEXTURE_2D, 0, HD_GL_RGBA, width, height, 0,
                    HD_GL_RGBA, HD_GL_UNSIGNED_BYTE, NULL);
    g_gl.BindTexture(HD_GL_TEXTURE_2D, (unsigned)previous_texture);
    g_gl.ActiveTexture((unsigned)previous_unit);
    g_width = width;
    g_height = height;
    fprintf(stderr, "[HD-VIDEO] GL: textura %dx%d pronta\n", width, height);
  }
  return 1;
}

int hd_video_gl_present(const unsigned char *rgba) {
  if (!g_program || !g_texture) return 0;

  int previous_program = 0, previous_buffer = 0, previous_texture = 0;
  int previous_unit = 0, previous_framebuffer = 0, previous_viewport[4] = {0};
  int previous_unpack_alignment = 4, previous_vertex_array = 0;
  float previous_clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  unsigned char previous_color_mask[4] = {1, 1, 1, 1};
  struct {
    int enabled;
    int size;
    int stride;
    int type;
    int normalized;
    int buffer;
    void *pointer;
  } previous_attrib[2] = {{0}};
  int isolated_vao = g_vertex_array && g_gl.BindVertexArray;
  unsigned char depth = g_gl.IsEnabled(HD_GL_DEPTH_TEST);
  unsigned char blend = g_gl.IsEnabled(HD_GL_BLEND);
  unsigned char cull = g_gl.IsEnabled(HD_GL_CULL_FACE);
  unsigned char scissor = g_gl.IsEnabled(HD_GL_SCISSOR_TEST);
  g_gl.GetIntegerv(HD_GL_CURRENT_PROGRAM, &previous_program);
  g_gl.GetIntegerv(HD_GL_ARRAY_BUFFER_BINDING, &previous_buffer);
  g_gl.GetIntegerv(HD_GL_ACTIVE_TEXTURE, &previous_unit);
  g_gl.GetIntegerv(HD_GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
  g_gl.GetIntegerv(HD_GL_VIEWPORT, previous_viewport);
  g_gl.GetIntegerv(HD_GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
  g_gl.GetFloatv(HD_GL_COLOR_CLEAR_VALUE, previous_clear_color);
  g_gl.GetBooleanv(HD_GL_COLOR_WRITEMASK, previous_color_mask);
  if (isolated_vao) {
    g_gl.GetIntegerv(HD_GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
    g_gl.BindVertexArray(g_vertex_array);
  } else {
    /* Vertex attribute arrays and their pointer/buffer tuples are VAO state.
     * Saving just GL_ARRAY_BUFFER is not enough: disabling attributes 0/1 in
     * Unity's current VAO makes every frame after the movie render empty. */
    for (unsigned index = 0; index < 2; ++index) {
      g_gl.GetVertexAttribiv(index, HD_GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                             &previous_attrib[index].enabled);
      g_gl.GetVertexAttribiv(index, HD_GL_VERTEX_ATTRIB_ARRAY_SIZE,
                             &previous_attrib[index].size);
      g_gl.GetVertexAttribiv(index, HD_GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                             &previous_attrib[index].stride);
      g_gl.GetVertexAttribiv(index, HD_GL_VERTEX_ATTRIB_ARRAY_TYPE,
                             &previous_attrib[index].type);
      g_gl.GetVertexAttribiv(index, HD_GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                             &previous_attrib[index].normalized);
      g_gl.GetVertexAttribiv(index, HD_GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                             &previous_attrib[index].buffer);
      g_gl.GetVertexAttribPointerv(index, HD_GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                   &previous_attrib[index].pointer);
    }
  }

  g_gl.BindFramebuffer(HD_GL_FRAMEBUFFER, 0);
  g_gl.Disable(HD_GL_DEPTH_TEST);
  g_gl.Disable(HD_GL_BLEND);
  g_gl.Disable(HD_GL_CULL_FACE);
  g_gl.Disable(HD_GL_SCISSOR_TEST);
  g_gl.ColorMask(1, 1, 1, 1);
  g_gl.Viewport(0, 0, g_width, g_height);
  g_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  g_gl.Clear(HD_GL_COLOR_BUFFER_BIT);

  g_gl.ActiveTexture(HD_GL_TEXTURE0);
  g_gl.GetIntegerv(HD_GL_TEXTURE_BINDING_2D, &previous_texture);
  g_gl.BindTexture(HD_GL_TEXTURE_2D, g_texture);
  if (rgba) {
    g_gl.PixelStorei(HD_GL_UNPACK_ALIGNMENT, 4);
    g_gl.TexSubImage2D(HD_GL_TEXTURE_2D, 0, 0, 0, g_width, g_height,
                       HD_GL_RGBA, HD_GL_UNSIGNED_BYTE, rgba);
  }

  g_gl.UseProgram(g_program);
  if (g_sampler >= 0) g_gl.Uniform1i(g_sampler, 0);
  g_gl.BindBuffer(HD_GL_ARRAY_BUFFER, g_buffer);
  g_gl.EnableVertexAttribArray(0);
  g_gl.EnableVertexAttribArray(1);
  g_gl.VertexAttribPointer(0, 2, HD_GL_FLOAT, 0, 4 * (int)sizeof(float),
                           (const void *)0);
  g_gl.VertexAttribPointer(1, 2, HD_GL_FLOAT, 0, 4 * (int)sizeof(float),
                           (const void *)(2 * sizeof(float)));
  g_gl.DrawArrays(HD_GL_TRIANGLE_STRIP, 0, 4);

  if (isolated_vao) {
    g_gl.BindVertexArray((unsigned)previous_vertex_array);
  } else {
    for (unsigned index = 0; index < 2; ++index) {
      g_gl.BindBuffer(HD_GL_ARRAY_BUFFER,
                      (unsigned)previous_attrib[index].buffer);
      g_gl.VertexAttribPointer(index, previous_attrib[index].size,
                               (unsigned)previous_attrib[index].type,
                               (unsigned char)previous_attrib[index].normalized,
                               previous_attrib[index].stride,
                               previous_attrib[index].pointer);
      if (previous_attrib[index].enabled)
        g_gl.EnableVertexAttribArray(index);
      else
        g_gl.DisableVertexAttribArray(index);
    }
  }

  g_gl.BindTexture(HD_GL_TEXTURE_2D, (unsigned)previous_texture);
  g_gl.ActiveTexture((unsigned)previous_unit);
  g_gl.BindBuffer(HD_GL_ARRAY_BUFFER, (unsigned)previous_buffer);
  g_gl.PixelStorei(HD_GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
  g_gl.UseProgram((unsigned)previous_program);
  g_gl.BindFramebuffer(HD_GL_FRAMEBUFFER, (unsigned)previous_framebuffer);
  g_gl.Viewport(previous_viewport[0], previous_viewport[1],
                previous_viewport[2], previous_viewport[3]);
  g_gl.ClearColor(previous_clear_color[0], previous_clear_color[1],
                  previous_clear_color[2], previous_clear_color[3]);
  g_gl.ColorMask(previous_color_mask[0], previous_color_mask[1],
                 previous_color_mask[2], previous_color_mask[3]);
  if (depth) g_gl.Enable(HD_GL_DEPTH_TEST);
  if (blend) g_gl.Enable(HD_GL_BLEND);
  if (cull) g_gl.Enable(HD_GL_CULL_FACE);
  if (scissor) g_gl.Enable(HD_GL_SCISSOR_TEST);
  return 1;
}

void hd_video_gl_end(void) {
  if (!g_resolved || g_failed) return;
  if (g_vertex_array && g_gl.DeleteVertexArrays) {
    g_gl.DeleteVertexArrays(1, &g_vertex_array);
    g_vertex_array = 0;
  }
  if (g_texture) {
    g_gl.DeleteTextures(1, &g_texture);
    g_texture = 0;
  }
  if (g_buffer) {
    g_gl.DeleteBuffers(1, &g_buffer);
    g_buffer = 0;
  }
  if (g_program) {
    g_gl.DeleteProgram(g_program);
    g_program = 0;
  }
  g_width = 0;
  g_height = 0;
}
