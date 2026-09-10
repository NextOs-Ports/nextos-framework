#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_shims.h"

enum resolver_mode {
  MODE_SDL_CORE,
  MODE_EGL_OES,
  MODE_DLSYM_CORE,
};

static enum resolver_mode mode;
static GLenum map_target;
static GLenum map_access;
static GLenum unmap_target;
static unsigned sdl_hits;
static unsigned egl_hits;
static unsigned dlsym_hits;
static int mapped_storage;

static void *host_map_buffer(GLenum target, GLenum access) {
  map_target = target;
  map_access = access;
  return &mapped_storage;
}

static GLboolean host_unmap_buffer(GLenum target) {
  unmap_target = target;
  return GL_TRUE;
}

static void *resolved_function(const char *name, int source) {
  const int wants_oes = mode == MODE_EGL_OES && source == 2;
  const int wants_core =
      (mode == MODE_SDL_CORE && source == 1) ||
      (mode == MODE_DLSYM_CORE && source == 3);

  if ((wants_oes && strcmp(name, "glMapBufferOES") == 0) ||
      (wants_core && strcmp(name, "glMapBuffer") == 0))
    return (void *)(uintptr_t)host_map_buffer;
  if ((wants_oes && strcmp(name, "glUnmapBufferOES") == 0) ||
      (wants_core && strcmp(name, "glUnmapBuffer") == 0))
    return (void *)(uintptr_t)host_unmap_buffer;
  return NULL;
}

void *SDLCALL SDL_GL_GetProcAddress(const char *name) {
  ++sdl_hits;
  return resolved_function(name, 1);
}

__eglMustCastToProperFunctionPointerType EGLAPIENTRY
eglGetProcAddress(const char *name) {
  ++egl_hits;
  return (__eglMustCastToProperFunctionPointerType)(uintptr_t)
      resolved_function(name, 2);
}

void *dlsym(void *handle, const char *name) {
  (void)handle;
  ++dlsym_hits;
  return resolved_function(name, 3);
}

void debugPrintf(const char *format, ...) {
  (void)format;
}

static void fail(const char *message) {
  fprintf(stderr, "gles_oes_alias_test: FAIL: %s\n", message);
  exit(1);
}

int main(int argc, char **argv) {
  if (argc != 2)
    fail("expected resolver mode");
  if (strcmp(argv[1], "sdl-core") == 0)
    mode = MODE_SDL_CORE;
  else if (strcmp(argv[1], "egl-oes") == 0)
    mode = MODE_EGL_OES;
  else if (strcmp(argv[1], "dlsym-core") == 0)
    mode = MODE_DLSYM_CORE;
  else
    fail("unknown resolver mode");

  const GLenum expected_target = 0x8892;
  const GLenum expected_access = 0x88b9;
  if (asm2_glMapBufferOES(expected_target, expected_access) != &mapped_storage)
    fail("map wrapper did not return the selected host result");
  if (asm2_glUnmapBufferOES(expected_target) != GL_TRUE)
    fail("unmap wrapper did not return the selected host result");
  if (map_target != expected_target || map_access != expected_access ||
      unmap_target != expected_target)
    fail("wrapper changed GLES integer arguments");

  if (mode == MODE_SDL_CORE && (sdl_hits == 0 || egl_hits != 0 ||
                               dlsym_hits != 0))
    fail("SDL core alias was not preferred");
  if (mode == MODE_EGL_OES && (sdl_hits == 0 || egl_hits == 0 ||
                              dlsym_hits != 0))
    fail("EGL OES fallback was not used after SDL");
  if (mode == MODE_DLSYM_CORE &&
      (sdl_hits == 0 || egl_hits == 0 || dlsym_hits == 0))
    fail("link-time core alias fallback was not reached");

  printf("gles_oes_alias_test: PASS mode=%s\n", argv[1]);
  return 0;
}
