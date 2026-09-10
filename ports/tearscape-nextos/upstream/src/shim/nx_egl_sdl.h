#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

int nx_sdl_egl_enabled(void);
EGLDisplay nx_sdl_egl_get_display(EGLNativeDisplayType native_display);
EGLBoolean nx_sdl_egl_initialize(EGLDisplay display, EGLint *major, EGLint *minor);
EGLBoolean nx_sdl_egl_terminate(EGLDisplay display);
EGLBoolean nx_sdl_egl_bind_api(EGLenum api);
EGLenum nx_sdl_egl_query_api(void);
EGLBoolean nx_sdl_egl_get_configs(EGLDisplay display, EGLConfig *configs,
                                  EGLint config_size, EGLint *num_config);
EGLBoolean nx_sdl_egl_get_config_attrib(EGLDisplay display, EGLConfig config,
                                        EGLint attribute, EGLint *value);
EGLBoolean nx_sdl_egl_choose_config(EGLDisplay display, const EGLint *attributes,
                                    EGLConfig *configs, EGLint config_size,
                                    EGLint *num_config);
EGLContext nx_sdl_egl_create_context(EGLDisplay display, EGLConfig config,
                                     EGLContext share_context,
                                     const EGLint *attributes);
EGLSurface nx_sdl_egl_create_window_surface(EGLDisplay display, EGLConfig config,
                                            EGLNativeWindowType native_window,
                                            const EGLint *attributes);
EGLSurface nx_sdl_egl_create_pbuffer_surface(EGLDisplay display, EGLConfig config,
                                             const EGLint *attributes);
EGLBoolean nx_sdl_egl_destroy_surface(EGLDisplay display, EGLSurface surface);
EGLBoolean nx_sdl_egl_destroy_context(EGLDisplay display, EGLContext context);
EGLBoolean nx_sdl_egl_make_current(EGLDisplay display, EGLSurface draw,
                                   EGLSurface read, EGLContext context);
EGLBoolean nx_sdl_egl_swap_buffers(EGLDisplay display, EGLSurface surface);
EGLBoolean nx_sdl_egl_swap_interval(EGLDisplay display, EGLint interval);
EGLint nx_sdl_egl_get_error(void);
EGLBoolean nx_sdl_egl_wait_gl(void);
EGLBoolean nx_sdl_egl_wait_native(EGLint engine);
EGLBoolean nx_sdl_egl_release_thread(void);
EGLContext nx_sdl_egl_get_current_context(void);
EGLSurface nx_sdl_egl_get_current_surface(EGLint readdraw);
EGLDisplay nx_sdl_egl_get_current_display(void);
EGLBoolean nx_sdl_egl_query_surface(EGLDisplay display, EGLSurface surface,
                                    EGLint attribute, EGLint *value);
EGLBoolean nx_sdl_egl_query_context(EGLDisplay display, EGLContext context,
                                    EGLint attribute, EGLint *value);
EGLBoolean nx_sdl_egl_surface_attrib(EGLDisplay display, EGLSurface surface,
                                    EGLint attribute, EGLint value);
const char *nx_sdl_egl_query_string(EGLDisplay display, EGLint name);
void *nx_sdl_egl_get_proc_address(const char *name);
