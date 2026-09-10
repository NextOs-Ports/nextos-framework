#include "video.h"

#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#if defined(__i386__)
#include "capture_x86.h"
#include "x86_runtime_compat.h"

#define ASM2_VIDEO_GL_WRAPPER(return_type, wrapper, symbol, parameters, args) \
  static return_type wrapper parameters {                                    \
    typedef return_type (*Function) parameters;                              \
    static Function function;                                                \
    if (!function)                                                           \
      function = (Function)asm2_x86_gl_resolve_now(symbol);                  \
    if (!function) {                                                         \
      debugPrintf("ASM2_VIDEO missing GL symbol %s\n", symbol);              \
      abort();                                                               \
    }                                                                        \
    return function args;                                                    \
  }

ASM2_VIDEO_GL_WRAPPER(const GLubyte *, asm2_video_glGetString, "glGetString",
                      (GLenum name), (name))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glGetIntegerv, "glGetIntegerv",
                      (GLenum name, GLint *value), (name, value))
ASM2_VIDEO_GL_WRAPPER(GLboolean, asm2_video_glIsEnabled, "glIsEnabled",
                      (GLenum capability), (capability))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glGetBooleanv, "glGetBooleanv",
                      (GLenum name, GLboolean *value), (name, value))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glGetFloatv, "glGetFloatv",
                      (GLenum name, GLfloat *value), (name, value))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glDisable, "glDisable",
                      (GLenum capability), (capability))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glEnable, "glEnable",
                      (GLenum capability), (capability))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glColorMask, "glColorMask",
                      (GLboolean red, GLboolean green, GLboolean blue,
                       GLboolean alpha),
                      (red, green, blue, alpha))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glClearColor, "glClearColor",
                      (GLfloat red, GLfloat green, GLfloat blue,
                       GLfloat alpha),
                      (red, green, blue, alpha))
ASM2_VIDEO_GL_WRAPPER(void, asm2_video_glClear, "glClear",
                      (GLbitfield mask), (mask))
#else
#define asm2_video_glGetString glGetString
#define asm2_video_glGetIntegerv glGetIntegerv
#define asm2_video_glIsEnabled glIsEnabled
#define asm2_video_glGetBooleanv glGetBooleanv
#define asm2_video_glGetFloatv glGetFloatv
#define asm2_video_glDisable glDisable
#define asm2_video_glEnable glEnable
#define asm2_video_glColorMask glColorMask
#define asm2_video_glClearColor glClearColor
#define asm2_video_glClear glClear
#endif

static int enabled_environment_flag(const char *name) {
  const char *value = getenv(name);
  return value && value[0] && strcmp(value, "0") != 0;
}

int asm2_video_init(struct asm2_video *video) {
  if (!video)
    return -1;
  memset(video, 0, sizeof(*video));

  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef SDL_HINT_NO_SIGNAL_HANDLERS
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#endif
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) !=
      0) {
    debugPrintf("ASM2_VIDEO SDL_Init failed: %s\n", SDL_GetError());
    return -1;
  }

  SDL_DisplayMode mode;
  video->width = 1280;
  video->height = 720;
  if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
    video->width = mode.w;
    video->height = mode.h;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  /* InitViewSettings reports the original Android target as RGB565/D24/S8. */
#if defined(__i386__)
  /*
   * The X5M KMSDRM/EGL stack cannot create a window surface for RGB565/A0.
   * Its native RGBA8888/A8 config preserves D24/S8 and passes exact readback.
   */
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#else
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
#endif
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  video->window = SDL_CreateWindow(
      "The Amazing Spider-Man 2 (1.2.7d)", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, video->width, video->height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!video->window) {
    debugPrintf("ASM2_VIDEO SDL_CreateWindow failed: %s\n", SDL_GetError());
    asm2_video_shutdown(video);
    return -1;
  }

  video->context = SDL_GL_CreateContext(video->window);
  if (!video->context) {
    debugPrintf("ASM2_VIDEO SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    asm2_video_shutdown(video);
    return -1;
  }
  if (SDL_GL_MakeCurrent(video->window, video->context) != 0) {
    debugPrintf("ASM2_VIDEO SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
    asm2_video_shutdown(video);
    return -1;
  }
  const int swap_interval_result = SDL_GL_SetSwapInterval(1);
  if (swap_interval_result != 0)
    debugPrintf("ASM2_VIDEO vsync request failed: %s\n", SDL_GetError());
  const int swap_interval = SDL_GL_GetSwapInterval();
  SDL_GL_GetDrawableSize(video->window, &video->width, &video->height);
  video->force_opaque_alpha = enabled_environment_flag("ASM2_FORCE_OPAQUE");

  int red = 0;
  int green = 0;
  int blue = 0;
  int alpha = 0;
  int depth = 0;
  int stencil = 0;
  SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &red);
  SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &green);
  SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &blue);
  SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alpha);
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
  SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);

  debugPrintf("ASM2_VIDEO_OK driver=%s drawable=%dx%d GLES=%s "
              "rgba=%d/%d/%d/%d depth=%d stencil=%d opaque=%d "
              "vsync_set_rc=%d vsync_effective=%d\n",
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
              video->width, video->height,
              asm2_video_glGetString(GL_VERSION)
                  ? (const char *)asm2_video_glGetString(GL_VERSION)
                  : "?",
              red, green, blue, alpha, depth, stencil,
              video->force_opaque_alpha, swap_interval_result,
              swap_interval);
  return 0;
}

static void force_backbuffer_opaque(void) {
  GLint framebuffer = 0;
  asm2_video_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
  if (framebuffer != 0)
    return;

  GLboolean old_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  GLfloat old_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  GLboolean old_scissor = asm2_video_glIsEnabled(GL_SCISSOR_TEST);
  asm2_video_glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
  asm2_video_glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
  if (old_scissor)
    asm2_video_glDisable(GL_SCISSOR_TEST);
  asm2_video_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  asm2_video_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  asm2_video_glClear(GL_COLOR_BUFFER_BIT);
  asm2_video_glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
  asm2_video_glClearColor(old_clear[0], old_clear[1], old_clear[2],
                          old_clear[3]);
  if (old_scissor)
    asm2_video_glEnable(GL_SCISSOR_TEST);
}

void asm2_video_swap(struct asm2_video *video) {
  if (!video || !video->window)
    return;
  if (video->force_opaque_alpha)
    force_backbuffer_opaque();
#if defined(__i386__)
  asm2_x86_capture_backbuffer_if_requested(video->width, video->height);
#endif
  SDL_GL_SwapWindow(video->window);
}

void asm2_video_shutdown(struct asm2_video *video) {
  if (!video)
    return;
  if (video->context) {
    SDL_GL_DeleteContext(video->context);
    video->context = NULL;
  }
  if (video->window) {
    SDL_DestroyWindow(video->window);
    video->window = NULL;
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER |
                    SDL_INIT_JOYSTICK);
}
