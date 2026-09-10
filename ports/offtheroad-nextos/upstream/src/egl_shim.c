/* egl_shim.c (DEVICE) -- contexto GL via SDL2 (driver "mali" do device faz o
 * EGL fbdev CERTO -> render visível na TV; o raw eglCreateWindowSurface(fbdev_
 * window) dava BAD_ALLOC 0x3003). Expomos os objetos EGL que o SDL2-mali criou
 * (eglGetCurrent*) p/ o libGame (que importa egl* direto) usar o MESMO contexto.
 * API bully_* idêntica -> jni_shim não muda. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdint.h>

static SDL_Window *g_win = NULL;
static SDL_GLContext g_ctx = NULL;
static int g_w = 1280, g_h = 720;

int bully_screen_w(void) { return g_w; }
int bully_screen_h(void) { return g_h; }

int bully_init_gl(void) {
  if (g_ctx) return 1;
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    fprintf(stderr, "[sdl] InitVideo: %s\n", SDL_GetError());

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w; g_h = dm.h;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  g_win = SDL_CreateWindow("Bully", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                           g_w, g_h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!g_win) { fprintf(stderr, "[sdl] CreateWindow: %s\n", SDL_GetError()); return 0; }
  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) { fprintf(stderr, "[sdl] GL_CreateContext: %s\n", SDL_GetError()); return 0; }
  SDL_GL_MakeCurrent(g_win, g_ctx);

  const GLubyte *r = glGetString(GL_RENDERER), *v = glGetString(GL_VERSION);
  fprintf(stderr, "[gl] SDL2 GLES2 %dx%d | EGL dpy=%p surf=%p ctx=%p | %s / %s\n",
          g_w, g_h, (void*)eglGetCurrentDisplay(), (void*)eglGetCurrentSurface(EGL_DRAW),
          (void*)eglGetCurrentContext(), r ? (const char*)r : "?", v ? (const char*)v : "?");
  return 1;
}

/* os objetos EGL REAIS que o SDL2-mali criou (o jogo seeda nos OS_EGL globals e
 * chama eglMakeCurrent direto neles) */
void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c) {
  if (d) *d = (uintptr_t)eglGetCurrentDisplay();
  if (s) *s = (uintptr_t)eglGetCurrentSurface(EGL_DRAW);
  if (c) *c = (uintptr_t)eglGetCurrentContext();
}
int  bully_make_current(void) { return SDL_GL_MakeCurrent(g_win, g_ctx) == 0 ? 1 : 0; }
void bully_release_current(void) { SDL_GL_MakeCurrent(g_win, NULL); }
void bully_swap_buffers(void) { if (g_win) SDL_GL_SwapWindow(g_win); }
