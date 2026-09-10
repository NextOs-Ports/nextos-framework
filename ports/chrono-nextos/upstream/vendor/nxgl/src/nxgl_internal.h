/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_INTERNAL_H
#define NXGL_INTERNAL_H

#include "nxgl.h"

#include <stdarg.h>

/* String constants keep the source compatible with older SDL2 headers that
 * already support these runtime hints but do not define the newer macros. */
#define NXGL_HINT_OPENGL_ES_DRIVER "SDL_OPENGL_ES_DRIVER"
#define NXGL_HINT_VIDEO_X11_FORCE_EGL "SDL_VIDEO_X11_FORCE_EGL"
#define NXGL_VIDEO_ENV_VALUE_MAX 4096u
#define NXGL_HINT_VALUE_MAX 4096u

#if defined(__GNUC__) || defined(__clang__)
#define NXGL_PRINTF_FORMAT(format_index, first_argument)                     \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define NXGL_PRINTF_FORMAT(format_index, first_argument)
#endif

typedef const unsigned char *(*nxgl_gl_get_string_fn)(unsigned int name);
typedef unsigned int (*nxgl_gl_get_error_fn)(void);
typedef void (*nxgl_gl_finish_fn)(void);
typedef unsigned char (*nxgl_gl_is_enabled_fn)(unsigned int capability);
typedef void (*nxgl_gl_get_booleanv_fn)(unsigned int name,
                                        unsigned char *values);
typedef void (*nxgl_gl_get_floatv_fn)(unsigned int name, float *values);
typedef void (*nxgl_gl_get_integerv_fn)(unsigned int name, int *values);
typedef void (*nxgl_gl_disable_fn)(unsigned int capability);
typedef void (*nxgl_gl_enable_fn)(unsigned int capability);
typedef void (*nxgl_gl_color_mask_fn)(unsigned char red, unsigned char green,
                                      unsigned char blue,
                                      unsigned char alpha);
typedef void (*nxgl_gl_clear_color_fn)(float red, float green, float blue,
                                       float alpha);
typedef void (*nxgl_gl_clear_fn)(unsigned int mask);

typedef struct nxgl_gl_functions {
  nxgl_gl_get_string_fn get_string;
  nxgl_gl_get_error_fn get_error;
  nxgl_gl_finish_fn finish;
  nxgl_gl_is_enabled_fn is_enabled;
  nxgl_gl_get_booleanv_fn get_booleanv;
  nxgl_gl_get_floatv_fn get_floatv;
  nxgl_gl_get_integerv_fn get_integerv;
  nxgl_gl_disable_fn disable;
  nxgl_gl_enable_fn enable;
  nxgl_gl_color_mask_fn color_mask;
  nxgl_gl_clear_color_fn clear_color;
  nxgl_gl_clear_fn clear;
} nxgl_gl_functions;

struct nxgl_context {
  uint32_t open_api_version;
  SDL_Window *window;
  SDL_GLContext gl_context;
  nxgl_context_ops context_ops;
  int initialized_video;
  int video_was_initialized_on_entry;
  int video_environment_cleared;
  int video_environment_preservable;
  int inherited_video_driver_existed;
  int inherited_video_driver_alias_existed;
  char *inherited_video_driver;
  char *inherited_video_driver_alias;
  int changed_gles_hint;
  int previous_gles_hint_existed;
  int previous_gles_hint_preservable;
  char *previous_gles_hint;
  int changed_x11_egl_hint;
  int previous_x11_egl_hint_existed;
  int previous_x11_egl_hint_preservable;
  char *previous_x11_egl_hint;
  nxgl_status_callback status;
  void *status_userdata;
  nxgl_gl_functions gl;
  nxgl_report report;
  nxgl_stack_owner_v2 stack_owner;
  nxgl_stack_ops_v2 stack_ops;
  void *stack_userdata;
  int stack_is_default_sdl;
  nxgl_stack_handles_v2 stack_handles;
  int stack_cleanup_pending;
  int stack_video_initialized;
  int stack_video_active;
  volatile int present_in_progress;
  nxgl_report_v2 report_v2;
};

void nxgl_copy_string(char *destination, size_t destination_size,
                      const char *source);
void nxgl_emit(nxgl_status_callback callback, void *userdata,
               nxgl_status_kind kind, const char *format, ...)
    NXGL_PRINTF_FORMAT(4, 5);
int nxgl_validate_actual(const nxgl_engine_requirements *requirements,
                         const nxgl_config_actual *actual, char *error,
                         size_t error_size);
int nxgl_resolve_gl_functions(nxgl_context *context);
void nxgl_delete_context(nxgl_context *context);
void nxgl_pump_events_without_consuming(void);
int nxgl_arbiter_try_acquire(void);
void nxgl_arbiter_release(void);

#if defined(NXGL_CORE_TESTING)
/* Test-only seam compiled into a private target.  It exercises the exact
 * production snapshot/clear/callback/restore transaction without SDL video. */
int nxgl_test_video_environment_retry(int callback_result,
                                      int create_alias_in_callback,
                                      int *callback_calls,
                                      int *environment_was_cleared);
#endif

#if defined(NXGL_M13_TESTING)
/* Test-only one-shot failpoint; production allocation remains plain calloc. */
void nxgl_test_fail_next_v2_allocation(void);
int nxgl_test_v2_hint_recovery_transaction(const char *backend,
                                            int *attempt_permitted,
                                            nxgl_report_v2 *report);
#endif

#endif /* NXGL_INTERNAL_H */
