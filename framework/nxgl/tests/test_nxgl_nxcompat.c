/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_nxcompat.h"

#include <SDL_egl.h>
#include <SDL_opengles2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int callback_count;
static int fake_context_token;
static int fake_window_token;
static int fake_sdl_context_token;
static int fake_egl_display_token;
static int fake_egl_context_token;
static int fake_egl_surface_token;
static int fake_egl_config_token;
static int fake_identity_valid = 1;
static int fake_drawable_valid = 1;
static int fake_disable_egl;
static int fake_sdl_doublebuffer_unavailable;
static EGLint fake_egl_render_buffer = EGL_BACK_BUFFER;
static const char *fake_backend = "kmsdrm";
static const char *fake_renderer = "Mali-450 MP fixture";
static int fake_use_v2;
static nxgl_report_v2 fake_report_v2;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static void status_callback(void *userdata, nxcompat_status_kind kind,
                            const char *message) {
  int *count = (int *)userdata;
  CHECK(kind == NXCOMPAT_STATUS_BACKEND);
  CHECK(message && strstr(message, "GLES 2.0"));
  ++*count;
}

void nxgl_resolution_sources_init(nxgl_resolution_sources *sources) {
  memset(sources, 0, sizeof(*sources));
  sources->api_version = NXGL_API_VERSION;
  sources->struct_size = sizeof(*sources);
}

SDL_Window *nxgl_window(nxgl_context *context) {
  CHECK(context == (nxgl_context *)(void *)&fake_context_token);
  return (SDL_Window *)(void *)&fake_window_token;
}

SDL_GLContext nxgl_sdl_context(nxgl_context *context) {
  CHECK(context == (nxgl_context *)(void *)&fake_context_token);
  return (SDL_GLContext)(void *)&fake_sdl_context_token;
}

int nxgl_make_current(nxgl_context *context) {
  CHECK(context == (nxgl_context *)(void *)&fake_context_token);
  return NXGL_SUCCESS;
}

const nxgl_report_v2 *nxgl_get_report_v2(const nxgl_context *context) {
  CHECK(context == (const nxgl_context *)(const void *)&fake_context_token);
  return fake_use_v2 ? &fake_report_v2 : NULL;
}

int nxgl_parse_gles_version(const char *version, int *major, int *minor) {
  if (!version || !major || !minor ||
      sscanf(version, "OpenGL ES %d.%d", major, minor) != 2)
    return NXGL_ERROR_NO_GLES_CONFIG;
  return NXGL_SUCCESS;
}

DECLSPEC SDL_Window *SDLCALL SDL_GL_GetCurrentWindow(void) {
  if (!fake_identity_valid)
    return NULL;
  return (SDL_Window *)(void *)&fake_window_token;
}

DECLSPEC SDL_GLContext SDLCALL SDL_GL_GetCurrentContext(void) {
  if (!fake_identity_valid)
    return NULL;
  return (SDL_GLContext)(void *)&fake_sdl_context_token;
}

DECLSPEC void SDLCALL SDL_GetWindowSize(SDL_Window *window, int *width,
                                        int *height) {
  CHECK(window == (SDL_Window *)(void *)&fake_window_token);
  *width = 640;
  *height = 480;
}

DECLSPEC void SDLCALL SDL_GL_GetDrawableSize(SDL_Window *window, int *width,
                                             int *height) {
  CHECK(window == (SDL_Window *)(void *)&fake_window_token);
  *width = fake_drawable_valid ? 640 : 0;
  *height = fake_drawable_valid ? 480 : 0;
}

DECLSPEC const char *SDLCALL SDL_GetCurrentVideoDriver(void) {
  return fake_backend;
}

DECLSPEC int SDLCALL SDL_GL_GetAttribute(SDL_GLattr attribute, int *value) {
  if (!value)
    return -1;
  switch (attribute) {
  case SDL_GL_RED_SIZE:
  case SDL_GL_GREEN_SIZE:
  case SDL_GL_BLUE_SIZE:
  case SDL_GL_ALPHA_SIZE:
    *value = 8;
    return 0;
  case SDL_GL_DEPTH_SIZE:
    *value = 24;
    return 0;
  case SDL_GL_STENCIL_SIZE:
    *value = 8;
    return 0;
  case SDL_GL_DOUBLEBUFFER:
    if (fake_sdl_doublebuffer_unavailable)
      return -1;
    *value = 1;
    return 0;
  case SDL_GL_CONTEXT_PROFILE_MASK:
    *value = SDL_GL_CONTEXT_PROFILE_ES;
    return 0;
  default:
    return -1;
  }
}

static const GLubyte *GL_APIENTRY fake_gl_get_string(GLenum name) {
  switch (name) {
  case GL_VENDOR:
    return (const GLubyte *)"ARM";
  case GL_RENDERER:
    return (const GLubyte *)fake_renderer;
  case GL_VERSION:
    return (const GLubyte *)"OpenGL ES 2.0 fixture";
  case GL_SHADING_LANGUAGE_VERSION:
    return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
  case GL_EXTENSIONS:
    return (const GLubyte *)"GL_OES_compressed_ETC1_RGB8_texture";
  default:
    return NULL;
  }
}

static EGLDisplay EGLAPIENTRY fake_egl_get_current_display(void) {
  return (EGLDisplay)(void *)&fake_egl_display_token;
}

static EGLContext EGLAPIENTRY fake_egl_get_current_context(void) {
  return (EGLContext)(void *)&fake_egl_context_token;
}

static EGLSurface EGLAPIENTRY fake_egl_get_current_surface(EGLint which) {
  CHECK(which == EGL_DRAW);
  return (EGLSurface)(void *)&fake_egl_surface_token;
}

static EGLBoolean EGLAPIENTRY fake_egl_query_surface(
    EGLDisplay display, EGLSurface surface, EGLint attribute,
    EGLint *value) {
  CHECK(display == (EGLDisplay)(void *)&fake_egl_display_token);
  CHECK(surface == (EGLSurface)(void *)&fake_egl_surface_token);
  if (attribute != EGL_RENDER_BUFFER || !value)
    return EGL_FALSE;
  *value = fake_egl_render_buffer;
  return EGL_TRUE;
}

static const char *EGLAPIENTRY fake_egl_query_string(EGLDisplay display,
                                                      EGLint name) {
  CHECK(display == (EGLDisplay)(void *)&fake_egl_display_token);
  switch (name) {
  case EGL_VENDOR:
    return "Fixture EGL";
  case EGL_VERSION:
    return "1.4";
  case EGL_CLIENT_APIS:
    return "OpenGL_ES";
  default:
    return NULL;
  }
}

static EGLBoolean EGLAPIENTRY fake_egl_query_context(EGLDisplay display,
                                                      EGLContext context,
                                                      EGLint attribute,
                                                      EGLint *value) {
  CHECK(display == (EGLDisplay)(void *)&fake_egl_display_token);
  CHECK(context == (EGLContext)(void *)&fake_egl_context_token);
  if (attribute != EGL_CONFIG_ID || !value)
    return EGL_FALSE;
  *value = 7;
  return EGL_TRUE;
}

static EGLBoolean EGLAPIENTRY fake_egl_choose_config(
    EGLDisplay display, const EGLint *attributes, EGLConfig *configs,
    EGLint config_size, EGLint *config_count) {
  CHECK(display == (EGLDisplay)(void *)&fake_egl_display_token);
  if (!attributes || attributes[0] != EGL_CONFIG_ID || attributes[1] != 7 ||
      attributes[2] != EGL_NONE || !configs || config_size != 1 ||
      !config_count)
    return EGL_FALSE;
  *configs = (EGLConfig)(void *)&fake_egl_config_token;
  *config_count = 1;
  return EGL_TRUE;
}

static EGLBoolean EGLAPIENTRY fake_egl_get_config_attrib(
    EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value) {
  CHECK(display == (EGLDisplay)(void *)&fake_egl_display_token);
  CHECK(config == (EGLConfig)(void *)&fake_egl_config_token);
  if (!value)
    return EGL_FALSE;
  switch (attribute) {
  case EGL_RED_SIZE:
  case EGL_GREEN_SIZE:
  case EGL_BLUE_SIZE:
  case EGL_ALPHA_SIZE:
  case EGL_STENCIL_SIZE:
    *value = 8;
    return EGL_TRUE;
  case EGL_DEPTH_SIZE:
    *value = 24;
    return EGL_TRUE;
  case EGL_RENDERABLE_TYPE:
    *value = EGL_OPENGL_ES2_BIT;
    return EGL_TRUE;
  case EGL_SURFACE_TYPE:
    *value = EGL_WINDOW_BIT;
    return EGL_TRUE;
  default:
    return EGL_FALSE;
  }
}

#define RETURN_FAKE_PROC(type, function_value)                              \
  do {                                                                       \
    type typed_function = (function_value);                                  \
    void *object_pointer = NULL;                                             \
    if (sizeof(object_pointer) != sizeof(typed_function))                     \
      return NULL;                                                           \
    memcpy(&object_pointer, &typed_function, sizeof(object_pointer));         \
    return object_pointer;                                                   \
  } while (0)

DECLSPEC void *SDLCALL SDL_GL_GetProcAddress(const char *name) {
  if (!name)
    return NULL;
  if (strcmp(name, "glGetString") == 0)
    RETURN_FAKE_PROC(PFNGLGETSTRINGPROC, fake_gl_get_string);
  if (fake_disable_egl)
    return NULL;
  if (strcmp(name, "eglGetCurrentDisplay") == 0)
    RETURN_FAKE_PROC(PFNEGLGETCURRENTDISPLAYPROC,
                     fake_egl_get_current_display);
  if (strcmp(name, "eglGetCurrentContext") == 0)
    RETURN_FAKE_PROC(PFNEGLGETCURRENTCONTEXTPROC,
                     fake_egl_get_current_context);
  if (strcmp(name, "eglGetCurrentSurface") == 0)
    RETURN_FAKE_PROC(PFNEGLGETCURRENTSURFACEPROC,
                     fake_egl_get_current_surface);
  if (strcmp(name, "eglQueryString") == 0)
    RETURN_FAKE_PROC(PFNEGLQUERYSTRINGPROC, fake_egl_query_string);
  if (strcmp(name, "eglQuerySurface") == 0)
    RETURN_FAKE_PROC(PFNEGLQUERYSURFACEPROC, fake_egl_query_surface);
  if (strcmp(name, "eglQueryContext") == 0)
    RETURN_FAKE_PROC(PFNEGLQUERYCONTEXTPROC, fake_egl_query_context);
  if (strcmp(name, "eglChooseConfig") == 0)
    RETURN_FAKE_PROC(PFNEGLCHOOSECONFIGPROC, fake_egl_choose_config);
  if (strcmp(name, "eglGetConfigAttrib") == 0)
    RETURN_FAKE_PROC(PFNEGLGETCONFIGATTRIBPROC, fake_egl_get_config_attrib);
  return NULL;
}

#undef RETURN_FAKE_PROC

static nxcompat_capability_evidence get_evidence(nxcompat_registry *registry,
                                                 uint32_t id) {
  nxcompat_capability_evidence evidence;
  memset(&evidence, 0, sizeof(evidence));
  CHECK(nxcompat_registry_get(registry, id, &evidence) == NXCOMPAT_OK);
  return evidence;
}

int main(void) {
  nxcompat_host host;
  nxgl_resolution_sources sources;
  nxgl_report report;
  nxcompat_graphics graphics;
  nxcompat_graphics_receipt receipt;
  nxcompat_registry *registry = NULL;
  unsigned char sources_snapshot[sizeof(sources)];
  unsigned char graphics_snapshot[sizeof(graphics)];
  nxgl_context *fake_context =
      (nxgl_context *)(void *)&fake_context_token;
  nxcompat_capability_evidence evidence;
  memset(&host, 0, sizeof(host));
  host.api_version = NXCOMPAT_API_VERSION;
  host.struct_size = sizeof(host);
  host.capabilities = NXCOMPAT_CAP_DRM_CONNECTED | NXCOMPAT_CAP_FBDEV;
  host.display_width = 640;
  host.display_height = 480;
  (void)snprintf(host.display_source, sizeof(host.display_source), "%s",
                 "/sys/class/drm/card0-HDMI-A-1/modes");
  memset(&sources, 0x5a, sizeof(sources));
  memcpy(sources_snapshot, &sources, sizeof(sources_snapshot));
  memset(host.display_source, 'D', sizeof(host.display_source));
  CHECK(nxgl_nxcompat_resolution_sources(&host, &sources) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(sources_snapshot, &sources, sizeof(sources_snapshot)) == 0);
  (void)snprintf(host.display_source, sizeof(host.display_source), "%s",
                 "/sys/class/drm/card0-HDMI-A-1/modes");
  CHECK(nxgl_nxcompat_resolution_sources(&host, &sources) == NXGL_SUCCESS);
  CHECK(sources.drm_width == 640 && sources.drm_height == 480);
  CHECK(sources.fbdev_width == 0 && sources.fbdev_height == 0);

  memset(&report, 0, sizeof(report));
  report.api_version = NXGL_API_VERSION;
  report.struct_size = sizeof(report);
  report.drawable_width = 640;
  report.drawable_height = 480;
  report.actual.red_bits = 8;
  report.actual.green_bits = 8;
  report.actual.blue_bits = 8;
  report.actual.alpha_bits = 8;
  report.actual.depth_bits = 24;
  report.actual.stencil_bits = 8;
  (void)snprintf(report.video_backend, sizeof(report.video_backend), "%s",
                 "kmsdrm");
  (void)snprintf(report.vendor, sizeof(report.vendor), "%s", "ARM");
  (void)snprintf(report.renderer, sizeof(report.renderer), "%s",
                 "Mali-450 MP");
  (void)snprintf(report.version, sizeof(report.version), "%s",
                 "OpenGL ES 2.0");
  (void)snprintf(report.shading_language_version,
                 sizeof(report.shading_language_version), "%s",
                 "OpenGL ES GLSL ES 1.00");
  (void)snprintf(report.extensions, sizeof(report.extensions), "%s",
                 "GL_OES_compressed_ETC1_RGB8_texture");
  CHECK(nxgl_nxcompat_capture_report(&report, &graphics, status_callback,
                                     &callback_count) == NXGL_SUCCESS);
  CHECK(callback_count == 1);
  CHECK(graphics.gpu_family == NXCOMPAT_GPU_MALI_4XX);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_GLES2) != 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_ETC1) != 0);
  memcpy(graphics_snapshot, &graphics, sizeof(graphics_snapshot));
  memset(report.extensions, 'E', sizeof(report.extensions));
  CHECK(nxgl_nxcompat_capture_report(&report, &graphics, status_callback,
                                     &callback_count) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(graphics_snapshot, &graphics, sizeof(graphics_snapshot)) == 0);
  CHECK(callback_count == 1);
  (void)snprintf(report.extensions, sizeof(report.extensions), "%s",
                 "GL_OES_compressed_ETC1_RGB8_texture");

  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 1u, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.source == NXCOMPAT_SOURCE_NXGL);
  CHECK((receipt.proof_flags & NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED) != 0u);
  CHECK((receipt.proof_flags & NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT) != 0u);
  CHECK((receipt.proof_flags & NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL) != 0u);
  CHECK((receipt.proof_flags &
         NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED) != 0u);
  CHECK((receipt.proof_flags &
         NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE) != 0u);
  CHECK(receipt.egl_config_id == 7);
  CHECK(receipt.egl_renderable_type == EGL_OPENGL_ES2_BIT);
  CHECK(receipt.double_buffer == 1);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_WINDOW).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_GLES2).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE).state ==
        NXCOMPAT_EVIDENCE_OPENED);

  /* API v2 deliberately makes the v1 accessors return NULL. The bridge must
   * consume the frozen SDL handles from the v2 report and must not call the
   * v1 make-current entry point. */
  memset(&fake_report_v2, 0, sizeof(fake_report_v2));
  fake_report_v2.api_version = NXGL_API_VERSION_V2;
  fake_report_v2.struct_size = sizeof(fake_report_v2);
  fake_report_v2.stack_owner = NXGL_STACK_OWNER_V2_SDL_EGL;
  fake_report_v2.handles.api_version = NXGL_API_VERSION_V2;
  fake_report_v2.handles.struct_size = sizeof(fake_report_v2.handles);
  fake_report_v2.handles.owner = NXGL_STACK_OWNER_V2_SDL_EGL;
  fake_report_v2.handles.sdl_window =
      (SDL_Window *)(void *)&fake_window_token;
  fake_report_v2.handles.sdl_context =
      (SDL_GLContext)(void *)&fake_sdl_context_token;
  fake_use_v2 = 1;
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 2u, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.generation == 2u);
  CHECK((receipt.proof_flags & NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED) !=
        0u);
  fake_report_v2.handles.owner = NXGL_STACK_OWNER_V2_RAW_EGL;
  memset(&receipt, 0x5a, sizeof(receipt));
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 3u, &receipt) ==
        NXCOMPAT_FAILED);
  CHECK(receipt.api_version == 0u);
  fake_report_v2.handles.owner = NXGL_STACK_OWNER_V2_SDL_EGL;
  fake_use_v2 = 0;

  memset(&receipt, 0x5a, sizeof(receipt));
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 2u, &receipt) ==
        NXCOMPAT_FAILED);
  CHECK(receipt.api_version == 0u);
  fake_sdl_doublebuffer_unavailable = 1;
  fake_egl_render_buffer = EGL_SINGLE_BUFFER;
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 3u, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.double_buffer == 0);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG)
            .generation == 3u);
  fake_sdl_doublebuffer_unavailable = 0;
  fake_egl_render_buffer = EGL_BACK_BUFFER;
  fake_identity_valid = 0;
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 4u, &receipt) ==
        NXCOMPAT_FAILED);
  CHECK(receipt.api_version == 0u);
  evidence = get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG);
  CHECK(evidence.state == NXCOMPAT_EVIDENCE_OPENED &&
        evidence.generation == 3u);
  fake_identity_valid = 1;

  fake_disable_egl = 1;
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 4u, &receipt) ==
        NXCOMPAT_OK);
  CHECK((receipt.proof_flags &
         NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT) == 0u);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG).state ==
        NXCOMPAT_EVIDENCE_LOST);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_GLES2).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  fake_disable_egl = 0;

  fake_drawable_valid = 0;
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 5u, &receipt) ==
        NXCOMPAT_OK);
  CHECK((receipt.proof_flags &
         NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE) == 0u);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE).state ==
        NXCOMPAT_EVIDENCE_LOST);
  fake_drawable_valid = 1;

  {
    char oversized_renderer[NXCOMPAT_DETAIL_MAX + 1u];
    memset(oversized_renderer, 'R', sizeof(oversized_renderer));
    oversized_renderer[sizeof(oversized_renderer) - 1u] = '\0';
    fake_renderer = oversized_renderer;
    memset(&receipt, 0x5a, sizeof(receipt));
    CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 6u,
                                        &receipt) == NXCOMPAT_FAILED);
    CHECK(receipt.api_version == 0u);
    evidence = get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE);
    CHECK(evidence.state == NXCOMPAT_EVIDENCE_LOST &&
          evidence.generation == 5u);
    fake_renderer = "Mali-450 MP fixture";
  }

  fake_backend = "dummy";
  CHECK(nxgl_nxcompat_publish_context(registry, fake_context, 6u, &receipt) ==
        NXCOMPAT_INVALID);
  evidence = get_evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE);
  CHECK(evidence.state == NXCOMPAT_EVIDENCE_LOST &&
        evidence.generation == 5u);
  fake_backend = "kmsdrm";
  CHECK(nxgl_nxcompat_publish_context(NULL, fake_context, 6u, NULL) ==
        NXCOMPAT_INVALID);
  CHECK(nxgl_nxcompat_publish_context(registry, NULL, 6u, NULL) ==
        NXCOMPAT_INVALID);
  nxcompat_registry_destroy(registry);
  if (failures)
    return 1;
  (void)fprintf(stdout, "nxgl nxcompat adapter tests passed\n");
  return 0;
}
