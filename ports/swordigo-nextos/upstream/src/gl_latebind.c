/*
 * Resolve GLES1 only after SDL owns a live context.
 *
 * Some KMSDRM firmware stacks cannot expose their device-enumeration entry
 * points when libEGL/libGLES are loaded as startup dependencies.  SDL is the
 * lifecycle owner here, so keep the loader free of those DT_NEEDED entries and
 * resolve the exact GLES calls after SDL_GL_CreateContext succeeds.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#define EGLAPI extern __attribute__((visibility("hidden")))
#define GL_API extern __attribute__((visibility("hidden")))
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_latebind.h"

#define HIDDEN __attribute__((visibility("hidden")))

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif

static void *gl_symbol(const char *name) {
  void *symbol = SDL_GL_GetProcAddress(name);
  if (!symbol) {
    fprintf(stderr, "FATAL ERROR: GLES entry point missing: %s\n", name);
    abort();
  }
  return symbol;
}

#define GL_VOID0(name)                                                        \
  HIDDEN void name(void) {                                                    \
    typedef void (*Fn)(void);                                                 \
    static Fn fn;                                                             \
    if (!fn) fn = (Fn)gl_symbol(#name);                                       \
    fn();                                                                     \
  }
#define GL_VOID1(name, T1, A1)                                                \
  HIDDEN void name(T1 A1) {                                                   \
    typedef void (*Fn)(T1);                                                   \
    static Fn fn;                                                             \
    if (!fn) fn = (Fn)gl_symbol(#name);                                       \
    fn(A1);                                                                   \
  }
#define GL_VOID2(name, T1, A1, T2, A2)                                       \
  HIDDEN void name(T1 A1, T2 A2) {                                            \
    typedef void (*Fn)(T1, T2);                                               \
    static Fn fn;                                                             \
    if (!fn) fn = (Fn)gl_symbol(#name);                                       \
    fn(A1, A2);                                                               \
  }
#define GL_VOID3(name, T1, A1, T2, A2, T3, A3)                               \
  HIDDEN void name(T1 A1, T2 A2, T3 A3) {                                     \
    typedef void (*Fn)(T1, T2, T3);                                           \
    static Fn fn;                                                             \
    if (!fn) fn = (Fn)gl_symbol(#name);                                       \
    fn(A1, A2, A3);                                                           \
  }
#define GL_VOID4(name, T1, A1, T2, A2, T3, A3, T4, A4)                       \
  HIDDEN void name(T1 A1, T2 A2, T3 A3, T4 A4) {                              \
    typedef void (*Fn)(T1, T2, T3, T4);                                       \
    static Fn fn;                                                             \
    if (!fn) fn = (Fn)gl_symbol(#name);                                       \
    fn(A1, A2, A3, A4);                                                       \
  }

/*
 * The ROCKNIX g24p0 Wayland blob on Mali-G52 can lose the VBO association
 * captured by a GLES1 client-array descriptor.  At draw time it then treats
 * the descriptor's byte offset as a CPU address (the reported crash copied
 * from 0x3e inside libmali).  Keep this workaround exact and opt-in by the
 * detected driver tuple: mirror guest buffers in CPU memory and present them
 * to that one driver as ordinary client arrays.  Every other stack continues
 * to call GLES directly.
 */
typedef struct BufferShadow {
  GLuint id;
  unsigned char *data;
  size_t size;
} BufferShadow;

typedef struct ArrayPointerState {
  int configured;
  int enabled;
  GLuint buffer;
  const void *pointer;
  GLint size;
  GLenum type;
  GLsizei stride;
} ArrayPointerState;

static int g_client_buffer_bridge;
static GLuint g_array_buffer;
static GLuint g_element_array_buffer;
static BufferShadow *g_buffer_shadows;
static size_t g_buffer_shadow_count;
static size_t g_buffer_shadow_capacity;
static unsigned g_bridge_warning_count;
static ArrayPointerState g_vertex_array;
static ArrayPointerState g_color_array;
static ArrayPointerState g_normal_array;
static ArrayPointerState g_texcoord_array;

typedef void (*BindBufferFn)(GLenum, GLuint);
typedef void (*BufferDataFn)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*BufferSubDataFn)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef void (*DeleteBuffersFn)(GLsizei, const GLuint *);
typedef void (*GenBuffersFn)(GLsizei, GLuint *);
typedef void (*ArrayPointerFn)(GLint, GLenum, GLsizei, const void *);
typedef void (*NormalPointerFn)(GLenum, GLsizei, const void *);
typedef void (*DrawArraysFn)(GLenum, GLint, GLsizei);
typedef void (*DrawElementsFn)(GLenum, GLsizei, GLenum, const void *);
typedef void (*ClientStateFn)(GLenum);
typedef void (*GetIntegervFn)(GLenum, GLint *);

static BindBufferFn real_gl_bind_buffer(void) {
  static BindBufferFn fn;
  if (!fn)
    fn = (BindBufferFn)gl_symbol("glBindBuffer");
  return fn;
}

static int bridge_target(GLenum target) {
  return target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER;
}

static GLuint *bridge_binding(GLenum target) {
  if (target == GL_ARRAY_BUFFER)
    return &g_array_buffer;
  if (target == GL_ELEMENT_ARRAY_BUFFER)
    return &g_element_array_buffer;
  return NULL;
}

static BufferShadow *bridge_find_buffer(GLuint id, int create) {
  size_t i;
  if (!id)
    return NULL;
  for (i = 0; i < g_buffer_shadow_count; ++i) {
    if (g_buffer_shadows[i].id == id)
      return &g_buffer_shadows[i];
  }
  if (!create)
    return NULL;
  if (g_buffer_shadow_count == g_buffer_shadow_capacity) {
    size_t capacity = g_buffer_shadow_capacity ?
                          g_buffer_shadow_capacity * 2u : 32u;
    BufferShadow *grown = realloc(g_buffer_shadows,
                                  capacity * sizeof(*g_buffer_shadows));
    if (!grown) {
      fprintf(stderr, "gl: g24p0 VBO bridge metadata allocation failed\n");
      return NULL;
    }
    g_buffer_shadows = grown;
    g_buffer_shadow_capacity = capacity;
  }
  BufferShadow *shadow = &g_buffer_shadows[g_buffer_shadow_count++];
  memset(shadow, 0, sizeof(*shadow));
  shadow->id = id;
  return shadow;
}

static void bridge_forget_buffer(GLuint id) {
  size_t i;
  for (i = 0; i < g_buffer_shadow_count; ++i) {
    if (g_buffer_shadows[i].id != id)
      continue;
    free(g_buffer_shadows[i].data);
    --g_buffer_shadow_count;
    if (i != g_buffer_shadow_count)
      g_buffer_shadows[i] = g_buffer_shadows[g_buffer_shadow_count];
    return;
  }
}

static const void *bridge_array_pointer(const ArrayPointerState *state,
                                        const char *name, int *valid) {
  BufferShadow *shadow;
  uintptr_t offset;
  if (!state->buffer)
    return state->pointer;
  shadow = bridge_find_buffer(state->buffer, 0);
  offset = (uintptr_t)state->pointer;
  if (!shadow || !shadow->data || offset >= shadow->size) {
    if (g_bridge_warning_count++ < 12u)
      fprintf(stderr,
              "gl: g24p0 VBO bridge rejected %s buffer=%u offset=0x%lx\n",
              name, state->buffer, (unsigned long)offset);
    *valid = 0;
    return NULL;
  }
  return shadow->data + offset;
}

static int bridge_refresh_arrays(void) {
  int valid = 1;
  const void *pointer;
  real_gl_bind_buffer()(GL_ARRAY_BUFFER, 0);

  if (g_vertex_array.enabled && g_vertex_array.configured) {
    ArrayPointerFn fn = (ArrayPointerFn)gl_symbol("glVertexPointer");
    pointer = bridge_array_pointer(&g_vertex_array, "vertex", &valid);
    fn(g_vertex_array.size, g_vertex_array.type, g_vertex_array.stride,
       pointer);
  }
  if (g_color_array.enabled && g_color_array.configured) {
    ArrayPointerFn fn = (ArrayPointerFn)gl_symbol("glColorPointer");
    pointer = bridge_array_pointer(&g_color_array, "color", &valid);
    fn(g_color_array.size, g_color_array.type, g_color_array.stride, pointer);
  }
  if (g_normal_array.enabled && g_normal_array.configured) {
    NormalPointerFn fn = (NormalPointerFn)gl_symbol("glNormalPointer");
    pointer = bridge_array_pointer(&g_normal_array, "normal", &valid);
    fn(g_normal_array.type, g_normal_array.stride, pointer);
  }
  if (g_texcoord_array.enabled && g_texcoord_array.configured) {
    ArrayPointerFn fn = (ArrayPointerFn)gl_symbol("glTexCoordPointer");
    pointer = bridge_array_pointer(&g_texcoord_array, "texcoord", &valid);
    fn(g_texcoord_array.size, g_texcoord_array.type, g_texcoord_array.stride,
       pointer);
  }
  return valid;
}

HIDDEN void gl_latebind_configure(const char *video_driver,
                                  const char *renderer,
                                  const char *version) {
  const char *override = getenv("SWORDIGO_G24_VBO_BRIDGE");
  int detected = video_driver && strcmp(video_driver, "wayland") == 0 &&
                 renderer && strstr(renderer, "Mali-G52") && version &&
                 strstr(version, "g24p0");
  if (override && strcmp(override, "0") == 0)
    detected = 0;
  else if (override && strcmp(override, "1") == 0)
    detected = 1;
  g_client_buffer_bridge = detected;
  if (g_client_buffer_bridge) {
    real_gl_bind_buffer()(GL_ARRAY_BUFFER, 0);
    real_gl_bind_buffer()(GL_ELEMENT_ARRAY_BUFFER, 0);
    fprintf(stderr,
            "gl: g24p0 VBO client-array bridge enabled for %s / %s\n",
            video_driver ? video_driver : "?", renderer ? renderer : "?");
  }
}

HIDDEN void glBindBuffer(GLenum target, GLuint buffer) {
  if (!g_client_buffer_bridge || !bridge_target(target)) {
    real_gl_bind_buffer()(target, buffer);
    return;
  }
  *bridge_binding(target) = buffer;
  if (buffer)
    (void)bridge_find_buffer(buffer, 1);
  real_gl_bind_buffer()(target, 0);
}

HIDDEN void glBufferData(GLenum target, GLsizeiptr size, const void *data,
                         GLenum usage) {
  static BufferDataFn fn;
  GLuint *binding;
  BufferShadow *shadow;
  unsigned char *replacement = NULL;
  (void)usage;
  if (!fn)
    fn = (BufferDataFn)gl_symbol("glBufferData");
  if (!g_client_buffer_bridge || !bridge_target(target)) {
    fn(target, size, data, usage);
    return;
  }
  binding = bridge_binding(target);
  shadow = binding ? bridge_find_buffer(*binding, 1) : NULL;
  if (!shadow || size < (GLsizeiptr)0) {
    if (g_bridge_warning_count++ < 12u)
      fprintf(stderr, "gl: g24p0 VBO bridge rejected glBufferData\n");
    return;
  }
  if (size) {
    replacement = malloc((size_t)size);
    if (!replacement) {
      fprintf(stderr, "gl: g24p0 VBO bridge data allocation failed (%lu)\n",
              (unsigned long)size);
      return;
    }
    if (data)
      memcpy(replacement, data, (size_t)size);
    else
      memset(replacement, 0, (size_t)size);
  }
  free(shadow->data);
  shadow->data = replacement;
  shadow->size = (size_t)size;
}

HIDDEN void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                            const void *data) {
  static BufferSubDataFn fn;
  GLuint *binding;
  BufferShadow *shadow;
  size_t start, length;
  if (!fn)
    fn = (BufferSubDataFn)gl_symbol("glBufferSubData");
  if (!g_client_buffer_bridge || !bridge_target(target)) {
    fn(target, offset, size, data);
    return;
  }
  binding = bridge_binding(target);
  shadow = binding ? bridge_find_buffer(*binding, 0) : NULL;
  if (!shadow || offset < (GLintptr)0 || size < (GLsizeiptr)0 ||
      (!data && size)) {
    if (g_bridge_warning_count++ < 12u)
      fprintf(stderr, "gl: g24p0 VBO bridge rejected glBufferSubData\n");
    return;
  }
  start = (size_t)offset;
  length = (size_t)size;
  if (start > shadow->size || length > shadow->size - start) {
    if (g_bridge_warning_count++ < 12u)
      fprintf(stderr,
              "gl: g24p0 VBO bridge subdata outside buffer=%u\n",
              shadow->id);
    return;
  }
  if (length)
    memcpy(shadow->data + start, data, length);
}

HIDDEN void glDeleteBuffers(GLsizei count, const GLuint *buffers) {
  static DeleteBuffersFn fn;
  GLsizei i;
  if (!fn)
    fn = (DeleteBuffersFn)gl_symbol("glDeleteBuffers");
  if (g_client_buffer_bridge && buffers) {
    for (i = 0; i < count; ++i) {
      if (g_array_buffer == buffers[i])
        g_array_buffer = 0;
      if (g_element_array_buffer == buffers[i])
        g_element_array_buffer = 0;
      bridge_forget_buffer(buffers[i]);
    }
  }
  fn(count, buffers);
}

HIDDEN void glGenBuffers(GLsizei count, GLuint *buffers) {
  static GenBuffersFn fn;
  GLsizei i;
  if (!fn)
    fn = (GenBuffersFn)gl_symbol("glGenBuffers");
  fn(count, buffers);
  if (g_client_buffer_bridge && buffers) {
    for (i = 0; i < count; ++i)
      (void)bridge_find_buffer(buffers[i], 1);
  }
}

static void bridge_set_array_pointer(ArrayPointerState *state, GLint size,
                                     GLenum type, GLsizei stride,
                                     const void *pointer) {
  state->configured = 1;
  state->buffer = g_array_buffer;
  state->pointer = pointer;
  state->size = size;
  state->type = type;
  state->stride = stride;
}

HIDDEN void glVertexPointer(GLint size, GLenum type, GLsizei stride,
                            const void *pointer) {
  static ArrayPointerFn fn;
  if (!fn)
    fn = (ArrayPointerFn)gl_symbol("glVertexPointer");
  if (!g_client_buffer_bridge) {
    fn(size, type, stride, pointer);
    return;
  }
  bridge_set_array_pointer(&g_vertex_array, size, type, stride, pointer);
  real_gl_bind_buffer()(GL_ARRAY_BUFFER, 0);
  int valid = 1;
  fn(size, type, stride,
     bridge_array_pointer(&g_vertex_array, "vertex", &valid));
}

HIDDEN void glColorPointer(GLint size, GLenum type, GLsizei stride,
                           const void *pointer) {
  static ArrayPointerFn fn;
  if (!fn)
    fn = (ArrayPointerFn)gl_symbol("glColorPointer");
  if (!g_client_buffer_bridge) {
    fn(size, type, stride, pointer);
    return;
  }
  bridge_set_array_pointer(&g_color_array, size, type, stride, pointer);
  real_gl_bind_buffer()(GL_ARRAY_BUFFER, 0);
  int valid = 1;
  fn(size, type, stride,
     bridge_array_pointer(&g_color_array, "color", &valid));
}

HIDDEN void glNormalPointer(GLenum type, GLsizei stride, const void *pointer) {
  static NormalPointerFn fn;
  if (!fn)
    fn = (NormalPointerFn)gl_symbol("glNormalPointer");
  if (!g_client_buffer_bridge) {
    fn(type, stride, pointer);
    return;
  }
  bridge_set_array_pointer(&g_normal_array, 3, type, stride, pointer);
  real_gl_bind_buffer()(GL_ARRAY_BUFFER, 0);
  int valid = 1;
  fn(type, stride,
     bridge_array_pointer(&g_normal_array, "normal", &valid));
}

HIDDEN void glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                              const void *pointer) {
  static ArrayPointerFn fn;
  if (!fn)
    fn = (ArrayPointerFn)gl_symbol("glTexCoordPointer");
  if (!g_client_buffer_bridge) {
    fn(size, type, stride, pointer);
    return;
  }
  bridge_set_array_pointer(&g_texcoord_array, size, type, stride, pointer);
  real_gl_bind_buffer()(GL_ARRAY_BUFFER, 0);
  int valid = 1;
  fn(size, type, stride,
     bridge_array_pointer(&g_texcoord_array, "texcoord", &valid));
}

static ArrayPointerState *bridge_client_state(GLenum array) {
  if (array == GL_VERTEX_ARRAY)
    return &g_vertex_array;
  if (array == GL_COLOR_ARRAY)
    return &g_color_array;
  if (array == GL_NORMAL_ARRAY)
    return &g_normal_array;
  if (array == GL_TEXTURE_COORD_ARRAY)
    return &g_texcoord_array;
  return NULL;
}

HIDDEN void glEnableClientState(GLenum array) {
  static ClientStateFn fn;
  ArrayPointerState *state;
  if (!fn)
    fn = (ClientStateFn)gl_symbol("glEnableClientState");
  if (g_client_buffer_bridge && (state = bridge_client_state(array)))
    state->enabled = 1;
  fn(array);
}

HIDDEN void glDisableClientState(GLenum array) {
  static ClientStateFn fn;
  ArrayPointerState *state;
  if (!fn)
    fn = (ClientStateFn)gl_symbol("glDisableClientState");
  if (g_client_buffer_bridge && (state = bridge_client_state(array)))
    state->enabled = 0;
  fn(array);
}

HIDDEN void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  static DrawArraysFn fn;
  if (!fn)
    fn = (DrawArraysFn)gl_symbol("glDrawArrays");
  if (g_client_buffer_bridge && !bridge_refresh_arrays())
    return;
  fn(mode, first, count);
}

HIDDEN void glDrawElements(GLenum mode, GLsizei count, GLenum type,
                           const void *indices) {
  static DrawElementsFn fn;
  const void *resolved = indices;
  int valid = 1;
  if (!fn)
    fn = (DrawElementsFn)gl_symbol("glDrawElements");
  if (g_client_buffer_bridge) {
    BufferShadow *shadow;
    uintptr_t offset;
    if (!bridge_refresh_arrays())
      return;
    real_gl_bind_buffer()(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (g_element_array_buffer) {
      shadow = bridge_find_buffer(g_element_array_buffer, 0);
      offset = (uintptr_t)indices;
      if (!shadow || !shadow->data || offset >= shadow->size) {
        if (g_bridge_warning_count++ < 12u)
          fprintf(stderr,
                  "gl: g24p0 VBO bridge rejected index buffer=%u "
                  "offset=0x%lx\n",
                  g_element_array_buffer, (unsigned long)offset);
        valid = 0;
      } else {
        resolved = shadow->data + offset;
      }
    }
  }
  if (valid)
    fn(mode, count, type, resolved);
}

HIDDEN void glGetIntegerv(GLenum pname, GLint *params) {
  static GetIntegervFn fn;
  if (!fn)
    fn = (GetIntegervFn)gl_symbol("glGetIntegerv");
  if (g_client_buffer_bridge && params && pname == GL_ARRAY_BUFFER_BINDING) {
    *params = (GLint)g_array_buffer;
    return;
  }
  if (g_client_buffer_bridge && params &&
      pname == GL_ELEMENT_ARRAY_BUFFER_BINDING) {
    *params = (GLint)g_element_array_buffer;
    return;
  }
  fn(pname, params);
}

GL_VOID1(glActiveTexture, GLenum, texture)
GL_VOID2(glBindTexture, GLenum, target, GLuint, texture)
GL_VOID2(glBlendFunc, GLenum, sfactor, GLenum, dfactor)
GL_VOID1(glClear, GLbitfield, mask)
GL_VOID4(glClearColor, GLclampf, red, GLclampf, green, GLclampf, blue, GLclampf,
         alpha)
GL_VOID1(glClearDepthf, GLclampf, depth)
GL_VOID1(glClearStencil, GLint, s)
GL_VOID4(glColor4f, GLfloat, red, GLfloat, green, GLfloat, blue, GLfloat, alpha)
GL_VOID4(glColor4ub, GLubyte, red, GLubyte, green, GLubyte, blue, GLubyte,
         alpha)
GL_VOID4(glColorMask, GLboolean, red, GLboolean, green, GLboolean, blue,
         GLboolean, alpha)

HIDDEN void glCompressedTexImage2D(GLenum target, GLint level,
                                   GLenum internalformat, GLsizei width,
                                   GLsizei height, GLint border,
                                   GLsizei image_size, const void *data) {
  typedef void (*Fn)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                     const void *);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glCompressedTexImage2D");
  fn(target, level, internalformat, width, height, border, image_size, data);
}

GL_VOID1(glCullFace, GLenum, mode)
GL_VOID2(glDeleteTextures, GLsizei, count, const GLuint *, textures)
GL_VOID1(glDepthFunc, GLenum, func)
GL_VOID1(glDepthMask, GLboolean, flag)
GL_VOID1(glDisable, GLenum, cap)
GL_VOID1(glEnable, GLenum, cap)
GL_VOID0(glFinish)
GL_VOID0(glFlush)
GL_VOID2(glGenTextures, GLsizei, count, GLuint *, textures)
GL_VOID2(glGetBooleanv, GLenum, pname, GLboolean *, params)

HIDDEN GLenum glGetError(void) {
  typedef GLenum (*Fn)(void);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glGetError");
  return fn();
}

GL_VOID2(glGetFloatv, GLenum, pname, GLfloat *, params)

HIDDEN const GLubyte *glGetString(GLenum name) {
  typedef const GLubyte *(*Fn)(GLenum);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glGetString");
  return fn(name);
}

HIDDEN GLboolean glIsEnabled(GLenum cap) {
  typedef GLboolean (*Fn)(GLenum);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glIsEnabled");
  return fn(cap);
}

GL_VOID3(glLightf, GLenum, light, GLenum, pname, GLfloat, param)
GL_VOID3(glLightfv, GLenum, light, GLenum, pname, const GLfloat *, params)
GL_VOID2(glLightModelfv, GLenum, pname, const GLfloat *, params)
GL_VOID0(glLoadIdentity)
GL_VOID1(glLoadMatrixf, const GLfloat *, matrix)
GL_VOID3(glMaterialfv, GLenum, face, GLenum, pname, const GLfloat *, params)
GL_VOID1(glMatrixMode, GLenum, mode)

HIDDEN void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top,
                     GLfloat near_value, GLfloat far_value) {
  typedef void (*Fn)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glOrthof");
  fn(left, right, bottom, top, near_value, far_value);
}

GL_VOID2(glPixelStorei, GLenum, pname, GLint, param)
GL_VOID0(glPopMatrix)
GL_VOID0(glPushMatrix)

HIDDEN void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels) {
  typedef void (*Fn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glReadPixels");
  fn(x, y, width, height, format, type, pixels);
}

GL_VOID3(glScalef, GLfloat, x, GLfloat, y, GLfloat, z)
GL_VOID4(glScissor, GLint, x, GLint, y, GLsizei, width, GLsizei, height)
GL_VOID3(glStencilFunc, GLenum, func, GLint, ref, GLuint, mask)
GL_VOID3(glStencilOp, GLenum, sfail, GLenum, dpfail, GLenum, dppass)
GL_VOID3(glTexEnvfv, GLenum, target, GLenum, pname, const GLfloat *, params)
GL_VOID3(glTexEnvi, GLenum, target, GLenum, pname, GLint, param)

HIDDEN void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border,
                         GLenum format, GLenum type, const void *pixels) {
  typedef void (*Fn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                     GLenum, const void *);
  static Fn fn;
  if (!fn) fn = (Fn)gl_symbol("glTexImage2D");
  fn(target, level, internalformat, width, height, border, format, type, pixels);
}

GL_VOID3(glTexParameterf, GLenum, target, GLenum, pname, GLfloat, param)
GL_VOID3(glTexParameteri, GLenum, target, GLenum, pname, GLint, param)
GL_VOID3(glTranslatef, GLfloat, x, GLfloat, y, GLfloat, z)
GL_VOID4(glViewport, GLint, x, GLint, y, GLsizei, width, GLsizei, height)

HIDDEN __eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *name) {
  if (name) {
#define BRIDGE_PROC(proc)                                                     \
    if (strcmp(name, #proc) == 0)                                            \
      return (__eglMustCastToProperFunctionPointerType)(proc)
    BRIDGE_PROC(glBindBuffer);
    BRIDGE_PROC(glBufferData);
    BRIDGE_PROC(glBufferSubData);
    BRIDGE_PROC(glDeleteBuffers);
    BRIDGE_PROC(glGenBuffers);
    BRIDGE_PROC(glVertexPointer);
    BRIDGE_PROC(glColorPointer);
    BRIDGE_PROC(glNormalPointer);
    BRIDGE_PROC(glTexCoordPointer);
    BRIDGE_PROC(glEnableClientState);
    BRIDGE_PROC(glDisableClientState);
    BRIDGE_PROC(glDrawArrays);
    BRIDGE_PROC(glDrawElements);
    BRIDGE_PROC(glGetIntegerv);
#undef BRIDGE_PROC
  }
  void *symbol = SDL_GL_GetProcAddress(name);
  if (symbol)
    return (__eglMustCastToProperFunctionPointerType)symbol;

  static void *egl;
  static __eglMustCastToProperFunctionPointerType (*real_get_proc)(
      const char *);
  if (!egl)
    egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (egl && !real_get_proc)
    real_get_proc = (__eglMustCastToProperFunctionPointerType (*)(const char *))
        dlsym(egl, "eglGetProcAddress");
  return real_get_proc ? real_get_proc(name) : NULL;
}
