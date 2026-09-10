#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform_shims.h"

#include <SDL2/SDL.h>
#if defined(__arm__)
#include <EGL/egl.h>
#endif
#include <dlfcn.h>
#include <string.h>

#include "guest_import.h"
#include "util.h"

static void *asm2_gl_extension(const char *name) {
#if defined(__i386__)
  /*
   * Box32's proven X5M path wraps SDL2 but not libGLESv2.  SDL returns an
   * x86-callable bridge for the native context's GL entry point.
   */
  void *function = SDL_GL_GetProcAddress(name);
#else
  void *function = (void *)eglGetProcAddress(name);
#endif
  if (!function)
    function = dlsym(RTLD_DEFAULT, name);
  return function;
}

void ASM2_GUEST_PCS asm2_glCompressedTexImage3DOES(
    GLenum target, GLint level, GLenum internal_format, GLsizei width,
    GLsizei height, GLsizei depth, GLint border, GLsizei image_size,
    const void *data) {
  typedef void (*Function)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei,
                           GLint, GLsizei, const void *);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glCompressedTexImage3DOES");
    looked_up = 1;
  }
  if (function)
    function(target, level, internal_format, width, height, depth, border,
             image_size, data);
}

void ASM2_GUEST_PCS asm2_glCompressedTexSubImage3DOES(
    GLenum target, GLint level, GLint x_offset, GLint y_offset, GLint z_offset,
    GLsizei width, GLsizei height, GLsizei depth, GLenum format,
    GLsizei image_size, const void *data) {
  typedef void (*Function)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei,
                           GLsizei, GLenum, GLsizei, const void *);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function =
        (Function)asm2_gl_extension("glCompressedTexSubImage3DOES");
    looked_up = 1;
  }
  if (function)
    function(target, level, x_offset, y_offset, z_offset, width, height, depth,
             format, image_size, data);
}

void ASM2_GUEST_PCS asm2_glTexImage3DOES(
    GLenum target, GLint level, GLenum internal_format, GLsizei width,
    GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
    const void *pixels) {
  typedef void (*Function)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei,
                           GLint, GLenum, GLenum, const void *);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glTexImage3DOES");
    looked_up = 1;
  }
  if (function)
    function(target, level, internal_format, width, height, depth, border,
             format, type, pixels);
}

void ASM2_GUEST_PCS asm2_glTexSubImage3DOES(
    GLenum target, GLint level, GLint x_offset, GLint y_offset, GLint z_offset,
    GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
    const void *pixels) {
  typedef void (*Function)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei,
                           GLsizei, GLenum, GLenum, const void *);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glTexSubImage3DOES");
    looked_up = 1;
  }
  if (function)
    function(target, level, x_offset, y_offset, z_offset, width, height, depth,
             format, type, pixels);
}

static GLuint asm2_next_fence = 1;
static uint64_t asm2_guest_glfinish_calls;
static uint64_t asm2_finish_fence_calls;
static uint64_t asm2_fallback_glfinish_calls;
static uint64_t asm2_test_fence_calls;
static uint64_t asm2_fallback_test_true_calls;
static int asm2_native_finish_fence = -1;
static int asm2_native_test_fence = -1;

void asm2_platform_get_stats(struct asm2_platform_stats *stats) {
  if (!stats)
    return;
  stats->guest_glfinish_calls =
      __atomic_load_n(&asm2_guest_glfinish_calls, __ATOMIC_RELAXED);
  stats->finish_fence_calls =
      __atomic_load_n(&asm2_finish_fence_calls, __ATOMIC_RELAXED);
  stats->fallback_glfinish_calls =
      __atomic_load_n(&asm2_fallback_glfinish_calls, __ATOMIC_RELAXED);
  stats->test_fence_calls =
      __atomic_load_n(&asm2_test_fence_calls, __ATOMIC_RELAXED);
  stats->fallback_test_true_calls =
      __atomic_load_n(&asm2_fallback_test_true_calls, __ATOMIC_RELAXED);
  stats->native_finish_fence =
      __atomic_load_n(&asm2_native_finish_fence, __ATOMIC_RELAXED);
  stats->native_test_fence =
      __atomic_load_n(&asm2_native_test_fence, __ATOMIC_RELAXED);
}

void ASM2_GUEST_PCS asm2_glFinish(void) {
  __atomic_add_fetch(&asm2_guest_glfinish_calls, 1u, __ATOMIC_RELAXED);
#if defined(__i386__)
  typedef void (*Function)(void);
  static Function function;
  if (!function)
    function = (Function)asm2_gl_extension("glFinish");
  if (function)
    function();
#else
  glFinish();
#endif
}

void ASM2_GUEST_PCS asm2_glDeleteFencesNV(GLsizei count,
                                           const GLuint *fences) {
  typedef void (*Function)(GLsizei, const GLuint *);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glDeleteFencesNV");
    looked_up = 1;
  }
  if (function)
    function(count, fences);
}

void ASM2_GUEST_PCS asm2_glGenFencesNV(GLsizei count, GLuint *fences) {
  typedef void (*Function)(GLsizei, GLuint *);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glGenFencesNV");
    looked_up = 1;
  }
  if (function) {
    function(count, fences);
    return;
  }
  for (GLsizei index = 0; index < count; ++index)
    fences[index] = asm2_next_fence++;
}

void ASM2_GUEST_PCS asm2_glSetFenceNV(GLuint fence, GLenum condition) {
  typedef void (*Function)(GLuint, GLenum);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glSetFenceNV");
    looked_up = 1;
  }
  if (function)
    function(fence, condition);
}

void ASM2_GUEST_PCS asm2_glFinishFenceNV(GLuint fence) {
  typedef void (*Function)(GLuint);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glFinishFenceNV");
    __atomic_store_n(&asm2_native_finish_fence, function ? 1 : 0,
                     __ATOMIC_RELAXED);
    debugPrintf("ASM2_GL_FENCE finish=%s\n",
                function ? "GL_NV_fence" : "glFinish fallback");
    looked_up = 1;
  }
  __atomic_add_fetch(&asm2_finish_fence_calls, 1u, __ATOMIC_RELAXED);
  if (function)
    function(fence);
  else {
    __atomic_add_fetch(&asm2_fallback_glfinish_calls, 1u, __ATOMIC_RELAXED);
#if defined(__i386__)
    asm2_glFinish();
#else
    glFinish();
#endif
  }
}

GLboolean ASM2_GUEST_PCS asm2_glTestFenceNV(GLuint fence) {
  typedef GLboolean (*Function)(GLuint);
  static Function function;
  static int looked_up;
  if (!looked_up) {
    function = (Function)asm2_gl_extension("glTestFenceNV");
    __atomic_store_n(&asm2_native_test_fence, function ? 1 : 0,
                     __ATOMIC_RELAXED);
    debugPrintf("ASM2_GL_FENCE test=%s\n",
                function ? "GL_NV_fence" : "immediate-true fallback");
    looked_up = 1;
  }
  __atomic_add_fetch(&asm2_test_fence_calls, 1u, __ATOMIC_RELAXED);
  if (function)
    return function(fence);
  __atomic_add_fetch(&asm2_fallback_test_true_calls, 1u, __ATOMIC_RELAXED);
  return GL_TRUE;
}

static __thread int asm2_fake_looper;
static int asm2_fake_sensor_manager;
static int asm2_fake_sensor_queue;

void *ASM2_GUEST_PCS asm2_ALooper_forThread(void) {
  return &asm2_fake_looper;
}
void *ASM2_GUEST_PCS asm2_ALooper_prepare(int options) {
  (void)options;
  return &asm2_fake_looper;
}
void *ASM2_GUEST_PCS asm2_ASensorManager_getInstance(void) {
  return &asm2_fake_sensor_manager;
}
void *ASM2_GUEST_PCS asm2_ASensorManager_getDefaultSensor(void *manager,
                                                           int sensor_type) {
  (void)manager;
  (void)sensor_type;
  return NULL;
}
void *ASM2_GUEST_PCS asm2_ASensorManager_createEventQueue(
    void *manager, void *looper, int identifier, void *callback, void *data) {
  (void)manager;
  (void)looper;
  (void)identifier;
  (void)callback;
  (void)data;
  return &asm2_fake_sensor_queue;
}
int ASM2_GUEST_PCS asm2_ASensorEventQueue_disableSensor(void *queue,
                                                        void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}
int ASM2_GUEST_PCS asm2_ASensorEventQueue_enableSensor(void *queue,
                                                       void *sensor) {
  (void)queue;
  (void)sensor;
  return sensor ? 0 : -1;
}
ssize_t ASM2_GUEST_PCS asm2_ASensorEventQueue_getEvents(void *queue,
                                                         void *events,
                                                         size_t count) {
  (void)queue;
  (void)events;
  (void)count;
  return 0;
}
int ASM2_GUEST_PCS asm2_ASensorEventQueue_setEventRate(void *queue,
                                                       void *sensor,
                                                       int32_t usec) {
  (void)queue;
  (void)usec;
  return sensor ? 0 : -1;
}

/* Registro EXTRA de exidx: o so_util guarda o estado de UM modulo, e este
 * port carrega DOIS (libfmodex.so antes da engine). Sem isto, uma excecao
 * C++ levantada dentro do FMOD nao acharia a tabela de unwind do modulo dele
 * e viraria std::terminate. Registrado pelo main.c logo apos cada carga. */
#define TS_EXIDX_MAX 4
static struct { uintptr_t lo, hi, table; int count; } ts_exidx[TS_EXIDX_MAX];
static int ts_exidx_n;

void ts_exidx_register(uintptr_t lo, uintptr_t hi, uintptr_t table, int count) {
  if (ts_exidx_n >= TS_EXIDX_MAX || !table)
    return;
  ts_exidx[ts_exidx_n].lo = lo;
  ts_exidx[ts_exidx_n].hi = hi;
  ts_exidx[ts_exidx_n].table = table;
  ts_exidx[ts_exidx_n].count = count;
  ts_exidx_n++;
}

uintptr_t ASM2_GUEST_PCS asm2_unwind_find_exidx(uintptr_t pc, int *count) {
  uintptr_t address = so_find_exidx(pc, count);
  if (address)
    return address;

  for (int i = 0; i < ts_exidx_n; i++)
    if (pc >= ts_exidx[i].lo && pc < ts_exidx[i].hi) {
      if (count)
        *count = ts_exidx[i].count;
      return ts_exidx[i].table;
    }

  typedef uintptr_t (*HostFunction)(uintptr_t, int *);
  static HostFunction host_function;
  static int looked_up;
  if (!looked_up) {
    host_function =
        (HostFunction)dlsym(RTLD_NEXT, "__gnu_Unwind_Find_exidx");
    looked_up = 1;
  }
  return host_function ? host_function(pc, count) : 0;
}
