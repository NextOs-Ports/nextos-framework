/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_nxcompat.h"

#include <SDL_egl.h>
#include <SDL_opengles2.h>

#include <stdio.h>
#include <string.h>

static int nxgl_nxcompat_bounded_string(const char *value, size_t size) {
  return value && size > 0u && memchr(value, '\0', size) != NULL;
}

int nxgl_nxcompat_resolution_sources(const nxcompat_host *host,
                                     nxgl_resolution_sources *sources) {
  if (!host || host->api_version != NXCOMPAT_API_VERSION ||
      host->struct_size < sizeof(*host) ||
      !nxgl_nxcompat_bounded_string(host->display_source,
                                    sizeof(host->display_source)) ||
      !sources)
    return NXGL_ERROR_INVALID_ARGUMENT;
  nxgl_resolution_sources_init(sources);
  if (host->display_width <= 0 || host->display_height <= 0)
    return NXGL_SUCCESS;
  if (strstr(host->display_source, "/drm/") ||
      ((host->capabilities & NXCOMPAT_CAP_DRM_CONNECTED) != 0 &&
       !strstr(host->display_source, "/graphics/fb"))) {
    sources->drm_width = host->display_width;
    sources->drm_height = host->display_height;
  } else if ((host->capabilities & NXCOMPAT_CAP_FBDEV) != 0) {
    sources->fbdev_width = host->display_width;
    sources->fbdev_height = host->display_height;
  }
  return NXGL_SUCCESS;
}

int nxgl_nxcompat_capture_report(const nxgl_report *report,
                                 nxcompat_graphics *graphics,
                                 nxcompat_status_callback status,
                                 void *status_userdata) {
  nxcompat_graphics_options options;
  char line[NXCOMPAT_DETAIL_MAX];
  int result;
  if (!report || report->api_version != NXGL_API_VERSION ||
      report->struct_size < sizeof(*report) ||
      !nxgl_nxcompat_bounded_string(report->video_backend,
                                    sizeof(report->video_backend)) ||
      !nxgl_nxcompat_bounded_string(report->vendor,
                                    sizeof(report->vendor)) ||
      !nxgl_nxcompat_bounded_string(report->renderer,
                                    sizeof(report->renderer)) ||
      !nxgl_nxcompat_bounded_string(report->version,
                                    sizeof(report->version)) ||
      !nxgl_nxcompat_bounded_string(
          report->shading_language_version,
          sizeof(report->shading_language_version)) ||
      !nxgl_nxcompat_bounded_string(report->extensions,
                                    sizeof(report->extensions)) ||
      !graphics)
    return NXGL_ERROR_INVALID_ARGUMENT;
  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION;
  options.struct_size = sizeof(options);
  options.video_driver = report->video_backend;
  options.vendor = report->vendor;
  options.renderer = report->renderer;
  options.version = report->version;
  options.shading_language_version = report->shading_language_version;
  options.extensions = report->extensions;
  options.drawable_width = report->drawable_width;
  options.drawable_height = report->drawable_height;
  options.red_bits = report->actual.red_bits;
  options.green_bits = report->actual.green_bits;
  options.blue_bits = report->actual.blue_bits;
  options.alpha_bits = report->actual.alpha_bits;
  options.depth_bits = report->actual.depth_bits;
  options.stencil_bits = report->actual.stencil_bits;
  result = nxcompat_capture_graphics(&options, graphics);
  if (result != 0)
    return result;
  if (status && nxcompat_format_graphics_line(graphics, line, sizeof(line)) >=
                    0)
    status(status_userdata, NXCOMPAT_STATUS_BACKEND, line);
  return NXGL_SUCCESS;
}

/* Several GLES2-era firmware headers expose the EGL entry points but omit the
 * optional PFN* typedef aliases present in newer Khronos headers. Keep the
 * exact public signatures locally so the strong receipt bridge remains
 * source-compatible with those low-glibc/old-header toolchains. */
typedef EGLDisplay(EGLAPIENTRY *nxgl_egl_get_current_display_fn)(void);
typedef EGLContext(EGLAPIENTRY *nxgl_egl_get_current_context_fn)(void);
typedef EGLSurface(EGLAPIENTRY *nxgl_egl_get_current_surface_fn)(EGLint);
typedef const char *(EGLAPIENTRY *nxgl_egl_query_string_fn)(EGLDisplay,
                                                            EGLint);
typedef EGLBoolean(EGLAPIENTRY *nxgl_egl_query_surface_fn)(
    EGLDisplay, EGLSurface, EGLint, EGLint *);
typedef EGLBoolean(EGLAPIENTRY *nxgl_egl_query_context_fn)(
    EGLDisplay, EGLContext, EGLint, EGLint *);
typedef EGLBoolean(EGLAPIENTRY *nxgl_egl_choose_config_fn)(
    EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean(EGLAPIENTRY *nxgl_egl_get_config_attrib_fn)(
    EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef const GLubyte *(GL_APIENTRY *nxgl_gl_get_string_fn)(GLenum);

typedef struct nxgl_nxcompat_egl_api {
  nxgl_egl_get_current_display_fn get_current_display;
  nxgl_egl_get_current_context_fn get_current_context;
  nxgl_egl_get_current_surface_fn get_current_surface;
  nxgl_egl_query_string_fn query_string;
  nxgl_egl_query_surface_fn query_surface;
  nxgl_egl_query_context_fn query_context;
  nxgl_egl_choose_config_fn choose_config;
  nxgl_egl_get_config_attrib_fn get_config_attrib;
} nxgl_nxcompat_egl_api;

static int nxgl_nxcompat_load_proc(void *destination, size_t destination_size,
                                  const char *name) {
  void *symbol;
  if (!destination || destination_size != sizeof(symbol) || !name)
    return 0;
  symbol = SDL_GL_GetProcAddress(name);
  if (!symbol)
    return 0;
  memcpy(destination, &symbol, destination_size);
  return 1;
}

static int nxgl_nxcompat_copy(char *destination, size_t destination_size,
                              const char *source) {
  const char *terminator;
  size_t length;
  if (!destination || destination_size == 0u)
    return 0;
  destination[0] = '\0';
  if (!source)
    return 1;
  terminator = (const char *)memchr(source, '\0', destination_size);
  if (!terminator)
    return 0;
  length = (size_t)(terminator - source);
  memcpy(destination, source, length + 1u);
  return 1;
}

static int nxgl_nxcompat_query_sdl_config(
    nxcompat_graphics_receipt *receipt) {
  receipt->double_buffer = -1;
  if (SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &receipt->double_buffer) != 0)
    receipt->double_buffer = -1;
  return SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &receipt->red_bits) == 0 &&
         SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &receipt->green_bits) == 0 &&
         SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &receipt->blue_bits) == 0 &&
         SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &receipt->alpha_bits) == 0 &&
         SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &receipt->depth_bits) == 0 &&
         SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &receipt->stencil_bits) ==
             0 &&
         SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                             &receipt->profile_mask) == 0;
}

static int nxgl_nxcompat_load_egl(nxgl_nxcompat_egl_api *api) {
  memset(api, 0, sizeof(*api));
  return nxgl_nxcompat_load_proc(&api->get_current_display,
                                 sizeof(api->get_current_display),
                                 "eglGetCurrentDisplay") &&
         nxgl_nxcompat_load_proc(&api->get_current_context,
                                 sizeof(api->get_current_context),
                                 "eglGetCurrentContext") &&
         nxgl_nxcompat_load_proc(&api->get_current_surface,
                                 sizeof(api->get_current_surface),
                                 "eglGetCurrentSurface") &&
         nxgl_nxcompat_load_proc(&api->query_string,
                                 sizeof(api->query_string),
                                 "eglQueryString") &&
         nxgl_nxcompat_load_proc(&api->query_surface,
                                 sizeof(api->query_surface),
                                 "eglQuerySurface") &&
         nxgl_nxcompat_load_proc(&api->query_context,
                                 sizeof(api->query_context),
                                 "eglQueryContext") &&
         nxgl_nxcompat_load_proc(&api->choose_config,
                                 sizeof(api->choose_config),
                                 "eglChooseConfig") &&
         nxgl_nxcompat_load_proc(&api->get_config_attrib,
                                 sizeof(api->get_config_attrib),
                                 "eglGetConfigAttrib");
}

static int nxgl_nxcompat_query_egl_attribute(
    const nxgl_nxcompat_egl_api *api, EGLDisplay display, EGLConfig config,
    EGLint attribute, int *destination) {
  EGLint value = 0;
  if (api->get_config_attrib(display, config, attribute, &value) != EGL_TRUE)
    return 0;
  *destination = (int)value;
  return 1;
}

static void nxgl_nxcompat_capture_egl(nxcompat_graphics_receipt *receipt) {
  nxgl_nxcompat_egl_api api;
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
  EGLConfig config = (EGLConfig)0;
  EGLint config_id = 0;
  EGLint config_count = 0;
  EGLint render_buffer = 0;
  EGLint attributes[3];
  const char *vendor;
  const char *version;
  const char *client_apis;
  if (!nxgl_nxcompat_load_egl(&api))
    return;
  display = api.get_current_display();
  context = api.get_current_context();
  surface = api.get_current_surface(EGL_DRAW);
  if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT)
    return;
  if (receipt->double_buffer < 0 && surface != EGL_NO_SURFACE &&
      api.query_surface(display, surface, EGL_RENDER_BUFFER,
                        &render_buffer) == EGL_TRUE &&
      (render_buffer == EGL_BACK_BUFFER ||
       render_buffer == EGL_SINGLE_BUFFER))
    receipt->double_buffer = render_buffer == EGL_BACK_BUFFER ? 1 : 0;
  vendor = api.query_string(display, EGL_VENDOR);
  version = api.query_string(display, EGL_VERSION);
  client_apis = api.query_string(display, EGL_CLIENT_APIS);
  if (!vendor || !vendor[0] || !version || !version[0] || !client_apis ||
      !client_apis[0])
    return;
  if (!nxgl_nxcompat_copy(receipt->egl_vendor,
                          sizeof(receipt->egl_vendor), vendor) ||
      !nxgl_nxcompat_copy(receipt->egl_version,
                          sizeof(receipt->egl_version), version) ||
      !nxgl_nxcompat_copy(receipt->egl_client_apis,
                          sizeof(receipt->egl_client_apis), client_apis)) {
    receipt->egl_vendor[0] = '\0';
    receipt->egl_version[0] = '\0';
    receipt->egl_client_apis[0] = '\0';
    return;
  }
  receipt->proof_flags |= NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                          NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT;
  if (api.query_context(display, context, EGL_CONFIG_ID, &config_id) !=
          EGL_TRUE ||
      config_id <= 0)
    return;
  attributes[0] = EGL_CONFIG_ID;
  attributes[1] = config_id;
  attributes[2] = EGL_NONE;
  if (api.choose_config(display, attributes, &config, 1, &config_count) !=
          EGL_TRUE ||
      config_count != 1 || !config)
    return;
  receipt->egl_config_id = (int)config_id;
  if (!nxgl_nxcompat_query_egl_attribute(&api, display, config, EGL_RED_SIZE,
                                         &receipt->egl_red_bits) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config, EGL_GREEN_SIZE,
                                         &receipt->egl_green_bits) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config, EGL_BLUE_SIZE,
                                         &receipt->egl_blue_bits) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config, EGL_ALPHA_SIZE,
                                         &receipt->egl_alpha_bits) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config, EGL_DEPTH_SIZE,
                                         &receipt->egl_depth_bits) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config,
                                         EGL_STENCIL_SIZE,
                                         &receipt->egl_stencil_bits) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config,
                                         EGL_RENDERABLE_TYPE,
                                         &receipt->egl_renderable_type) ||
      !nxgl_nxcompat_query_egl_attribute(&api, display, config,
                                         EGL_SURFACE_TYPE,
                                         &receipt->egl_surface_type)) {
    receipt->egl_config_id = 0;
    receipt->egl_red_bits = 0;
    receipt->egl_green_bits = 0;
    receipt->egl_blue_bits = 0;
    receipt->egl_alpha_bits = 0;
    receipt->egl_depth_bits = 0;
    receipt->egl_stencil_bits = 0;
    receipt->egl_renderable_type = 0;
    receipt->egl_surface_type = 0;
    return;
  }
  receipt->proof_flags |= NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED;
}

nxcompat_result_code nxgl_nxcompat_publish_context(
    nxcompat_registry *registry, nxgl_context *context, uint64_t generation,
    nxcompat_graphics_receipt *published_receipt) {
  nxcompat_graphics_receipt receipt;
  SDL_Window *window;
  SDL_GLContext sdl_context;
  nxgl_gl_get_string_fn get_string = NULL;
  const GLubyte *vendor;
  const GLubyte *renderer;
  const GLubyte *version;
  const GLubyte *glsl_version;
  const GLubyte *extensions;
  const char *backend;
  nxcompat_result_code result;
  if (published_receipt)
    memset(published_receipt, 0, sizeof(*published_receipt));
  if (!registry || !context || generation == 0u)
    return NXCOMPAT_INVALID;
  window = nxgl_window(context);
  sdl_context = nxgl_sdl_context(context);
  if (!window || !sdl_context || nxgl_make_current(context) != NXGL_SUCCESS ||
      SDL_GL_GetCurrentWindow() != window ||
      SDL_GL_GetCurrentContext() != sdl_context)
    return NXCOMPAT_FAILED;
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.source = NXCOMPAT_SOURCE_NXGL;
  receipt.generation = generation;
  receipt.proof_flags = NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                        NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT;
  SDL_GetWindowSize(window, &receipt.window_width, &receipt.window_height);
  SDL_GL_GetDrawableSize(window, &receipt.drawable_width,
                         &receipt.drawable_height);
  backend = SDL_GetCurrentVideoDriver();
  if (!nxgl_nxcompat_copy(receipt.video_backend,
                          sizeof(receipt.video_backend), backend))
    return NXCOMPAT_FAILED;
  if (receipt.drawable_width > 0 && receipt.drawable_height > 0)
    receipt.proof_flags |= NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE;
  if (!nxgl_nxcompat_query_sdl_config(&receipt) ||
      !nxgl_nxcompat_load_proc(&get_string, sizeof(get_string),
                               "glGetString"))
    return NXCOMPAT_FAILED;
  vendor = get_string(GL_VENDOR);
  renderer = get_string(GL_RENDERER);
  version = get_string(GL_VERSION);
  glsl_version = get_string(GL_SHADING_LANGUAGE_VERSION);
  extensions = get_string(GL_EXTENSIONS);
  if (!vendor || !vendor[0] || !renderer || !renderer[0] || !version ||
      !version[0] || !glsl_version || !glsl_version[0] ||
      nxgl_parse_gles_version((const char *)version, &receipt.gles_major,
                              &receipt.gles_minor) != NXGL_SUCCESS)
    return NXCOMPAT_FAILED;
  if (!nxgl_nxcompat_copy(receipt.gl_vendor, sizeof(receipt.gl_vendor),
                          (const char *)vendor) ||
      !nxgl_nxcompat_copy(receipt.gl_renderer, sizeof(receipt.gl_renderer),
                          (const char *)renderer) ||
      !nxgl_nxcompat_copy(receipt.gl_version, sizeof(receipt.gl_version),
                          (const char *)version) ||
      !nxgl_nxcompat_copy(receipt.glsl_version,
                          sizeof(receipt.glsl_version),
                          (const char *)glsl_version))
    return NXCOMPAT_FAILED;
  /* Extension lists can legitimately exceed the bounded receipt. Discard an
   * oversized list so optional capabilities remain absent instead of deriving
   * evidence from a truncated token. Baseline GLES proof remains usable. */
  if (!nxgl_nxcompat_copy(receipt.gl_extensions,
                          sizeof(receipt.gl_extensions),
                          (const char *)extensions))
    receipt.gl_extensions[0] = '\0';
  receipt.proof_flags |= NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL;
  nxgl_nxcompat_capture_egl(&receipt);
  result = nxcompat_registry_publish_graphics(registry, &receipt);
  if (result == NXCOMPAT_OK && published_receipt)
    *published_receipt = receipt;
  return result;
}
