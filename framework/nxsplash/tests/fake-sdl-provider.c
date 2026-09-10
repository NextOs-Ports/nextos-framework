/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Rect { int x, y, w, h; } Rect;

int SDL_Init(uint32_t flags) { (void)flags; return 0; }
void SDL_Quit(void) {}
const char *SDL_GetError(void) { return "provider fixture"; }
const char *SDL_GetCurrentVideoDriver(void) { return "KMSDRM"; }
int SDL_GetNumVideoDrivers(void) { return 1; }
const char *SDL_GetVideoDriver(int index) {
  return index == 0 ? "KMSDRM" : NULL;
}
void *SDL_CreateWindow(const char *title, int x, int y, int width, int height,
                       uint32_t flags) {
  const char *egl = getenv("SDL_VIDEO_EGL_DRIVER");
  const char *gles = getenv("SDL_VIDEO_GL_DRIVER");
  (void)title; (void)x; (void)y; (void)width; (void)height; (void)flags;
  return egl && gles && strcmp(egl, "libEGL.so") == 0 &&
         strcmp(gles, "libGLESv2.so") == 0 ? (void *)1 : NULL;
}
void SDL_DestroyWindow(void *window) { (void)window; }
void *SDL_CreateRenderer(void *window, int index, uint32_t flags) {
  (void)index; (void)flags; return window ? (void *)2 : NULL;
}
void SDL_DestroyRenderer(void *renderer) { (void)renderer; }
int SDL_GetRendererOutputSize(void *renderer, int *width, int *height) {
  (void)renderer; *width = 640; *height = 480; return 0;
}
int SDL_SetRenderDrawColor(void *renderer, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a) {
  (void)renderer; (void)r; (void)g; (void)b; (void)a; return 0;
}
int SDL_RenderClear(void *renderer) { (void)renderer; return 0; }
int SDL_RenderFillRect(void *renderer, const Rect *rect) {
  (void)renderer; (void)rect; return 0;
}
int SDL_RenderDrawRect(void *renderer, const Rect *rect) {
  (void)renderer; (void)rect; return 0;
}
void SDL_RenderPresent(void *renderer) { (void)renderer; }
int SDL_PollEvent(void *event) { (void)event; return 0; }
void SDL_Delay(uint32_t milliseconds) { usleep(milliseconds * 1000u); }
