/* SOR4 graphics capability probe for PortMaster/aarch64.
 *
 * The display backend is deliberately left to SDL. We only negotiate an OpenGL ES
 * context (ES3 first, ES2 fallback) and print a shell-safe capability contract used
 * by the launcher and first-run bake. No GPU/CFW name is used for the decision.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void SDL_Window;
typedef void *SDL_GLContext;

static int (*SDL_Init)(uint32_t flags);
static void (*SDL_Quit)(void);
static const char *(*SDL_GetError)(void);
static int (*SDL_GL_SetAttribute)(int attr, int value);
static SDL_Window *(*SDL_CreateWindow)(const char *title, int x, int y, int w, int h,
                                       uint32_t flags);
static void (*SDL_DestroyWindow)(SDL_Window *window);
static SDL_GLContext (*SDL_GL_CreateContext)(SDL_Window *window);
static void (*SDL_GL_DeleteContext)(SDL_GLContext context);
static void *(*SDL_GL_GetProcAddress)(const char *name);

static int load_sdl(void) {
  void *library = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!library) library = dlopen("libSDL2.so", RTLD_NOW | RTLD_GLOBAL);
  if (!library) {
    fprintf(stderr, "sor4probe: SDL2 load failed: %s\n", dlerror());
    return 0;
  }
#define SDL_LOAD(name) do { \
  *(void **)&name = dlsym(library, #name); \
  if (!name) { fprintf(stderr, "sor4probe: missing %s\n", #name); return 0; } \
} while (0)
  SDL_LOAD(SDL_Init); SDL_LOAD(SDL_Quit); SDL_LOAD(SDL_GetError);
  SDL_LOAD(SDL_GL_SetAttribute); SDL_LOAD(SDL_CreateWindow);
  SDL_LOAD(SDL_DestroyWindow); SDL_LOAD(SDL_GL_CreateContext);
  SDL_LOAD(SDL_GL_DeleteContext); SDL_LOAD(SDL_GL_GetProcAddress);
#undef SDL_LOAD
  return 1;
}

enum {
  SDL_INIT_VIDEO = 0x20,
  SDL_WINDOW_OPENGL = 0x2,
  SDL_WINDOW_HIDDEN = 0x8,
  SDL_GL_DOUBLEBUFFER = 5,
  SDL_GL_CONTEXT_MAJOR_VERSION = 17,
  SDL_GL_CONTEXT_MINOR_VERSION = 18,
  SDL_GL_CONTEXT_PROFILE_MASK = 21,
  SDL_GL_CONTEXT_PROFILE_ES = 4,
  GL_VENDOR = 0x1f00,
  GL_RENDERER = 0x1f01,
  GL_VERSION = 0x1f02,
  GL_EXTENSIONS = 0x1f03
};

typedef const unsigned char *(*gl_get_string_fn)(unsigned int name);

static void safe_value(const char *key, const char *value) {
  char clean[512];
  size_t out = 0;
  if (!value) value = "unknown";
  while (*value && out + 1 < sizeof(clean)) {
    unsigned char c = (unsigned char)*value++;
    clean[out++] = (c >= 32 && c < 127 && c != '\'' && c != '\\') ? (char)c : '_';
  }
  clean[out] = 0;
  printf("%s='%s'\n", key, clean);
}

static int try_context(int requested, int *actual, int *astc) {
  SDL_Window *window = NULL;
  SDL_GLContext context = NULL;
  gl_get_string_fn get_string;
  const char *version;
  const char *extensions;
  int ok = 0;

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, requested);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  window = SDL_CreateWindow("SOR4 probe", 0, 0, 64, 64,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) goto done;
  context = SDL_GL_CreateContext(window);
  if (!context) goto done;
  get_string = (gl_get_string_fn)SDL_GL_GetProcAddress("glGetString");
  if (!get_string) goto done;
  version = (const char *)get_string(GL_VERSION);
  if (!version || !strstr(version, "OpenGL ES")) goto done;
  *actual = strstr(version, "OpenGL ES 3") ? 3 : 2;
  extensions = (const char *)get_string(GL_EXTENSIONS);
  *astc = extensions &&
      (strstr(extensions, "GL_KHR_texture_compression_astc_ldr") ||
       strstr(extensions, "GL_OES_texture_compression_astc"));
  safe_value("SOR4_GL_VENDOR", (const char *)get_string(GL_VENDOR));
  safe_value("SOR4_GL_RENDERER", (const char *)get_string(GL_RENDERER));
  safe_value("SOR4_GL_VERSION", version);
  ok = 1;

done:
  if (context) SDL_GL_DeleteContext(context);
  if (window) SDL_DestroyWindow(window);
  return ok;
}

int main(void) {
  int major = 0;
  int astc = 0;
  if (!load_sdl()) {
    /* A final safe contract is still printed below. */
  } else if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "sor4probe: SDL video init failed: %s\n", SDL_GetError());
  } else if (!try_context(3, &major, &astc) &&
             !try_context(2, &major, &astc)) {
    fprintf(stderr, "sor4probe: no OpenGL ES context: %s\n", SDL_GetError());
  }
  printf("SOR4_PROBE_OK=%d\n", major >= 2);
  printf("SOR4_GLES=%d\n", major >= 2 ? major : 2);
  printf("SOR4_ASTC=%d\n", astc ? 1 : 0);
  printf("SOR4_ETC2=%d\n", major >= 3 ? 1 : 0);
  if (SDL_Quit) SDL_Quit();
  return major >= 2 ? 0 : 1;
}
