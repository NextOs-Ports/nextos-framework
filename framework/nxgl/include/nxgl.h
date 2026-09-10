/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_H
#define NXGL_H

#include <SDL.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_API_VERSION 1u
#define NXGL_API_VERSION_V1 1u
#define NXGL_API_VERSION_V2 2u
#define NXGL_API_CURRENT_VERSION NXGL_API_VERSION_V2
#define NXGL_VERSION "0.3.5"

#define NXGL_NAME_MAX 64u
#define NXGL_DETAIL_MAX 192u
#define NXGL_EXTENSIONS_MAX 1024u
#define NXGL_MAX_CONFIG_CANDIDATES 16u
#define NXGL_ATTEMPT_JOURNAL_MAX 32u
#define NXGL_ATTEMPT_DETAIL_MAX 96u
#define NXGL_SURFACE_DIMENSION_MAX 32768
#define NXGL_SDL_VIDEO_DRIVER_MAX 16u
#define NXGL_SDL_VIDEO_HINT_MAX 256u
#define NXGL_SDL_VIDEO_DRIVER_LIST_MAX 1024u
#define NXGL_SDL_VIDEO_RECEIPT_TEXT_MAX 1536u

typedef enum nxgl_result_code {
  NXGL_SUCCESS = 0,
  NXGL_NO_ACTION = 1,
  NXGL_ERROR_INVALID_ARGUMENT = -1,
  NXGL_ERROR_VIDEO_UNAVAILABLE = -2,
  NXGL_ERROR_RESOLUTION_UNAVAILABLE = -3,
  NXGL_ERROR_NO_GLES_CONFIG = -4,
  NXGL_ERROR_PRESENT = -5,
  NXGL_ERROR_OUT_OF_MEMORY = -6,
  NXGL_ERROR_BUSY = -7,
  NXGL_ERROR_ROLLBACK = -8,
  NXGL_ERROR_STACK_MISMATCH = -9,
  NXGL_ERROR_INVALID_STATE = -10
} nxgl_result_code;

typedef enum nxgl_status_kind {
  NXGL_STATUS_INFO = 0,
  NXGL_STATUS_ATTEMPT,
  NXGL_STATUS_SELECTED,
  NXGL_STATUS_WARNING,
  NXGL_STATUS_ERROR
} nxgl_status_kind;

/* The callback may write to stderr, a PortMaster log, or text drawn over an
 * existing startup logo. nxgl never takes ownership of the UI.  Callback
 * code and userdata are retained by an opened context and must remain alive
 * through its successful close or final rollback cleanup.  Callbacks execute
 * while the nxgl arbiter is held and must return promptly: a reentrant
 * mutating nxgl call returns NXGL_ERROR_BUSY, except legacy void close which
 * is a no-op while busy. */
typedef void (*nxgl_status_callback)(void *userdata, nxgl_status_kind kind,
                                     const char *message);

typedef enum nxgl_resolution_source {
  NXGL_RESOLUTION_NONE = 0,
  NXGL_RESOLUTION_SDL_DESKTOP,
  NXGL_RESOLUTION_SDL_CURRENT,
  NXGL_RESOLUTION_SDL_BOUNDS,
  NXGL_RESOLUTION_DRM_FACT,
  NXGL_RESOLUTION_FBDEV_FACT
} nxgl_resolution_source;

/* SDL values are populated by nxgl at runtime. DRM/fbdev values are facts
 * supplied by a preflight probe (normally nxcompat), never reprobed here. */
typedef struct nxgl_resolution_sources {
  uint32_t api_version;
  size_t struct_size;
  int sdl_desktop_width;
  int sdl_desktop_height;
  int sdl_current_width;
  int sdl_current_height;
  int sdl_bounds_width;
  int sdl_bounds_height;
  int drm_width;
  int drm_height;
  int fbdev_width;
  int fbdev_height;
} nxgl_resolution_sources;

typedef struct nxgl_resolution {
  uint32_t api_version;
  size_t struct_size;
  nxgl_resolution_source source;
  int width;
  int height;
} nxgl_resolution;

/* A candidate is an explicit request. All RGBA/depth/stencil fields are set
 * before its SDL window is created; zero is also an exact, intentional
 * request. The delivered config is queried and validated separately. */
typedef struct nxgl_config_candidate {
  int gles_major;
  int gles_minor;
  int red_bits;
  int green_bits;
  int blue_bits;
  int alpha_bits;
  int depth_bits;
  int stencil_bits;
  int double_buffer;
} nxgl_config_candidate;

/* Engine requirements decide whether a delivered context is usable. A zero
 * maximum means no maximum, but the caller must still list every context
 * version it is willing to try in its candidate ladder. Double buffering is
 * optional by default because working KMSDRM stacks can expose a functional
 * swap path while reporting EGL_SINGLE_BUFFER; engines that truly require a
 * back buffer must set require_double_buffer explicitly. */
typedef struct nxgl_engine_requirements {
  uint32_t api_version;
  size_t struct_size;
  int minimum_gles_major;
  int minimum_gles_minor;
  int maximum_gles_major;
  int maximum_gles_minor;
  int minimum_red_bits;
  int minimum_green_bits;
  int minimum_blue_bits;
  int minimum_alpha_bits;
  int minimum_depth_bits;
  int minimum_stencil_bits;
  int require_double_buffer;
} nxgl_engine_requirements;

/* An architecture adapter can wrap these calls when the Android ABI needs
 * TLS preservation. NULL callbacks use the corresponding SDL function.  The
 * struct is copied into an opened context; callback code and userdata must
 * remain alive through close/final rollback because delete_context may run
 * during teardown. */
typedef SDL_GLContext (*nxgl_create_context_callback)(void *userdata,
                                                       SDL_Window *window);
typedef int (*nxgl_make_current_callback)(void *userdata, SDL_Window *window,
                                          SDL_GLContext context);
typedef void (*nxgl_delete_context_callback)(void *userdata,
                                              SDL_GLContext context);

typedef struct nxgl_context_ops {
  uint32_t api_version;
  size_t struct_size;
  nxgl_create_context_callback create_context;
  nxgl_make_current_callback make_current;
  nxgl_delete_context_callback delete_context;
  void *userdata;
} nxgl_context_ops;

enum nxgl_open_flag {
  /* Initialize only SDL_INIT_VIDEO when the caller/nxcompat has not done so. */
  NXGL_OPEN_INITIALIZE_VIDEO = 1u << 0,
  /* After an actual desktop-GL context is rejected, retry the same candidate
   * once with SDL's GLES hint (and its EGL hint when the inherited backend is
   * X11). This never selects SDL_VIDEODRIVER. */
  NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP = 1u << 1,
  /* Intended only for host tests and diagnostic programs. */
  NXGL_OPEN_ALLOW_NONDISPLAY_BACKEND = 1u << 2,
  /* If and only if a full window+GLES+drawable negotiation fails while a
   * video-driver hint is inherited, remove that hint and retry SDL automatic
   * selection once. This restarts SDL video and is therefore a startup-only
   * policy, before engine windows/threads exist. No replacement backend name
   * is selected by nxgl. */
  NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE = 1u << 3
};

typedef struct nxgl_open_options {
  uint32_t api_version;
  size_t struct_size;
  uint32_t flags;
  const char *window_title;
  Uint32 window_flags;
  int display_index;
  unsigned drawable_wait_ms;
  const nxgl_resolution_sources *fallback_facts;
  const nxgl_engine_requirements *requirements;
  const nxgl_config_candidate *candidates;
  size_t candidate_count;
  const nxgl_context_ops *context_ops;
  nxgl_status_callback status;
  void *status_userdata;
} nxgl_open_options;

typedef struct nxgl_config_actual {
  int gles_major;
  int gles_minor;
  int red_bits;
  int green_bits;
  int blue_bits;
  int alpha_bits;
  int depth_bits;
  int stencil_bits;
  int double_buffer;
  int profile_mask;
} nxgl_config_actual;

typedef struct nxgl_report {
  uint32_t api_version;
  size_t struct_size;
  nxgl_resolution resolution;
  int window_width;
  int window_height;
  int drawable_width;
  int drawable_height;
  unsigned attempt_count;
  size_t selected_candidate_index;
  int used_gles_hint_recovery;
  int used_video_autodetect_recovery;
  nxgl_config_candidate requested;
  nxgl_config_actual actual;
  char video_backend[NXGL_NAME_MAX];
  char vendor[NXGL_DETAIL_MAX];
  char renderer[NXGL_DETAIL_MAX];
  char version[NXGL_DETAIL_MAX];
  char shading_language_version[NXGL_DETAIL_MAX];
  char extensions[NXGL_EXTENSIONS_MAX];
} nxgl_report;

/* Optional target-SDL startup policy. It must run in the process and ABI that
 * will initialize video, before SDL_INIT_VIDEO. The helper never chooses a
 * backend: it preserves a supported inherited SDL_VIDEODRIVER, removes only
 * an unsupported value, and otherwise leaves SDL autodetection untouched.
 * SDL_VIDEO_DRIVER, SDL_DYNAMIC_API and provider variables are not changed. */
typedef enum nxgl_sdl_video_hint_action_v2 {
  NXGL_SDL_VIDEO_HINT_V2_DISABLED = 0,
  NXGL_SDL_VIDEO_HINT_V2_NO_HINT,
  NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED,
  NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED,
  NXGL_SDL_VIDEO_HINT_V2_REFUSED_VIDEO_ACTIVE,
  NXGL_SDL_VIDEO_HINT_V2_DRIVER_QUERY_FAILED,
  NXGL_SDL_VIDEO_HINT_V2_INVALID_HINT,
  NXGL_SDL_VIDEO_HINT_V2_ENVIRONMENT_FAILED
} nxgl_sdl_video_hint_action_v2;

typedef struct nxgl_sdl_video_hint_options_v2 {
  uint32_t api_version;
  size_t struct_size;
  /* Defaults to zero. Adapters opt in only for a target-ABI process whose
   * inherited frontend hint may describe a different SDL build. */
  int enabled;
} nxgl_sdl_video_hint_options_v2;

typedef struct nxgl_sdl_video_hint_receipt_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_sdl_video_hint_action_v2 action;
  int inherited_hint_present;
  int inherited_hint_supported;
  int hint_removed;
  int video_driver_count;
  int selected_recorded;
  char inherited_hint[NXGL_SDL_VIDEO_HINT_MAX];
  char compiled_video_drivers[NXGL_SDL_VIDEO_DRIVER_LIST_MAX];
  char selected_video_driver[NXGL_NAME_MAX];
} nxgl_sdl_video_hint_receipt_v2;

void nxgl_sdl_video_hint_options_v2_init(
    nxgl_sdl_video_hint_options_v2 *options);
void nxgl_sdl_video_hint_receipt_v2_init(
    nxgl_sdl_video_hint_receipt_v2 *receipt);
/* Invalid/BUSY calls are byte-atomic. Operational failures publish a receipt
 * explaining why no backend decision was applied. */
int nxgl_sanitize_sdl_video_hint_v2(
    const nxgl_sdl_video_hint_options_v2 *options,
    nxgl_sdl_video_hint_receipt_v2 *receipt);
/* Call only after the adapter's SDL_Init succeeds. It records the driver
 * selected by that same target SDL and never changes SDL or the environment. */
int nxgl_complete_sdl_video_hint_receipt_v2(
    nxgl_sdl_video_hint_receipt_v2 *receipt);
const char *nxgl_sdl_video_hint_action_name_v2(
    nxgl_sdl_video_hint_action_v2 action);
/* Produces one bounded log line with inherited, compiled, action and selected
 * fields. output_size should be NXGL_SDL_VIDEO_RECEIPT_TEXT_MAX. */
int nxgl_format_sdl_video_hint_receipt_v2(
    const nxgl_sdl_video_hint_receipt_v2 *receipt, char *output,
    size_t output_size);

typedef struct nxgl_context nxgl_context;

/* Populates a conservative, Mali-450/GLES2-friendly set of non-engine
 * defaults. The engine requirements and candidate ladder remain mandatory. */
void nxgl_open_options_init(nxgl_open_options *options);
void nxgl_resolution_sources_init(nxgl_resolution_sources *sources);
void nxgl_engine_requirements_init(nxgl_engine_requirements *requirements);

/* Pure helpers are public so the same policy can be tested without a GPU. */
int nxgl_choose_resolution(const nxgl_resolution_sources *sources,
                           nxgl_resolution *resolution);
const char *nxgl_resolution_source_name(nxgl_resolution_source source);
int nxgl_parse_gles_version(const char *version, int *major, int *minor);
int nxgl_candidate_compatible(const nxgl_engine_requirements *requirements,
                              const nxgl_config_candidate *candidate);

/* The video subsystem must already be negotiated by nxcompat/PortMaster or
 * NXGL_OPEN_INITIALIZE_VIDEO must be set. nxgl never assigns a backend name
 * to SDL_VIDEODRIVER; it may remove an inherited hint after real failure. */
int nxgl_open(const nxgl_open_options *options, nxgl_context **context,
              nxgl_report *report);
/* All mutating entry points share a process-global non-blocking arbiter.
 * Under contention open/make_current return NXGL_ERROR_BUSY before touching
 * caller outputs or graphics state.  Legacy close has no result channel and
 * is therefore a no-op while busy; callers must serialize and retry it only
 * while they still own a live context. */
void nxgl_close(nxgl_context *context);
SDL_Window *nxgl_window(nxgl_context *context);
SDL_GLContext nxgl_sdl_context(nxgl_context *context);
int nxgl_make_current(nxgl_context *context);
const nxgl_report *nxgl_get_report(const nxgl_context *context);
/* The four legacy access/use entry points above accept API-v1 contexts only:
 * v2 contexts use their frozen stack and v2 report, so legacy getters return
 * NULL and legacy make_current returns NXGL_ERROR_INVALID_ARGUMENT. */

/* API v2 is additive.  The API-v1 constants and structures retain their
 * original layouts, and its uncontended behavior is unchanged.  A v2 context
 * freezes one complete graphics stack so window/context/proc lookup/current
 * checks and presentation cannot be mixed between SDL/EGL providers. */
typedef enum nxgl_stack_owner_v2 {
  NXGL_STACK_OWNER_V2_NONE = 0,
  NXGL_STACK_OWNER_V2_SDL_EGL,
  NXGL_STACK_OWNER_V2_RAW_EGL
} nxgl_stack_owner_v2;

typedef struct nxgl_stack_handles_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_stack_owner_v2 owner;
  SDL_Window *sdl_window;
  SDL_GLContext sdl_context;
  uintptr_t native_display;
  uintptr_t native_window;
  uintptr_t egl_display;
  uintptr_t egl_context;
  uintptr_t egl_surface;
  uintptr_t egl_config;
} nxgl_stack_handles_v2;

typedef struct nxgl_egl_actual_v2 {
  uint32_t api_version;
  size_t struct_size;
  int observed;
  uintptr_t display;
  uintptr_t context;
  uintptr_t surface;
  uintptr_t config;
  int config_id;
  int surface_width;
  int surface_height;
  int red_bits;
  int green_bits;
  int blue_bits;
  int alpha_bits;
  int depth_bits;
  int stencil_bits;
  int renderable_type;
  int surface_type;
  char vendor[NXGL_DETAIL_MAX];
  char version[NXGL_DETAIL_MAX];
  char client_apis[NXGL_DETAIL_MAX];
} nxgl_egl_actual_v2;

typedef enum nxgl_open_stage_v2 {
  NXGL_OPEN_STAGE_V2_NONE = 0,
  NXGL_OPEN_STAGE_V2_ARBITER,
  NXGL_OPEN_STAGE_V2_ENVIRONMENT_SNAPSHOT,
  NXGL_OPEN_STAGE_V2_HINT_SNAPSHOT,
  NXGL_OPEN_STAGE_V2_VIDEO_START,
  NXGL_OPEN_STAGE_V2_RESOLUTION,
  NXGL_OPEN_STAGE_V2_CONFIG_ATTRIBUTES,
  NXGL_OPEN_STAGE_V2_WINDOW_CREATE,
  NXGL_OPEN_STAGE_V2_CONTEXT_CREATE,
  NXGL_OPEN_STAGE_V2_MAKE_CURRENT,
  NXGL_OPEN_STAGE_V2_PROC_RESOLVE,
  NXGL_OPEN_STAGE_V2_CONFIG_QUERY,
  NXGL_OPEN_STAGE_V2_DRAWABLE_QUERY,
  NXGL_OPEN_STAGE_V2_CURRENT_VALIDATE,
  NXGL_OPEN_STAGE_V2_ATTEMPT_CLEANUP,
  NXGL_OPEN_STAGE_V2_VIDEO_STOP,
  NXGL_OPEN_STAGE_V2_HINT_RESTORE,
  NXGL_OPEN_STAGE_V2_ENVIRONMENT_RESTORE,
  NXGL_OPEN_STAGE_V2_HINT_APPLY
} nxgl_open_stage_v2;

typedef enum nxgl_open_reason_v2 {
  NXGL_OPEN_REASON_V2_NONE = 0,
  NXGL_OPEN_REASON_V2_SELECTED,
  NXGL_OPEN_REASON_V2_ARBITER_BUSY,
  NXGL_OPEN_REASON_V2_INVALID_OPTIONS,
  NXGL_OPEN_REASON_V2_SNAPSHOT_TOO_LARGE,
  NXGL_OPEN_REASON_V2_SNAPSHOT_FAILED,
  NXGL_OPEN_REASON_V2_VIDEO_UNAVAILABLE,
  NXGL_OPEN_REASON_V2_RESOLUTION_UNAVAILABLE,
  NXGL_OPEN_REASON_V2_ATTRIBUTES_REJECTED,
  NXGL_OPEN_REASON_V2_WINDOW_FAILED,
  NXGL_OPEN_REASON_V2_CONTEXT_FAILED,
  NXGL_OPEN_REASON_V2_CURRENT_FAILED,
  NXGL_OPEN_REASON_V2_PROC_UNAVAILABLE,
  NXGL_OPEN_REASON_V2_DESKTOP_GL_REJECTED,
  NXGL_OPEN_REASON_V2_CONFIG_QUERY_FAILED,
  NXGL_OPEN_REASON_V2_CONFIG_REJECTED,
  NXGL_OPEN_REASON_V2_EGL_CONFIG_UNAVAILABLE,
  NXGL_OPEN_REASON_V2_DRAWABLE_INVALID,
  NXGL_OPEN_REASON_V2_CURRENT_MISMATCH,
  NXGL_OPEN_REASON_V2_CLEANUP_FAILED,
  NXGL_OPEN_REASON_V2_RESTORE_FAILED,
  NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT,
  NXGL_OPEN_REASON_V2_OUT_OF_MEMORY,
  NXGL_OPEN_REASON_V2_HINT_APPLY_FAILED
} nxgl_open_reason_v2;

typedef struct nxgl_attempt_entry_v2 {
  uint32_t api_version;
  size_t struct_size;
  unsigned round_index;
  unsigned attempt_index;
  size_t candidate_index;
  nxgl_stack_owner_v2 owner;
  nxgl_open_stage_v2 stage;
  nxgl_open_reason_v2 reason;
  int result;
  char detail[NXGL_ATTEMPT_DETAIL_MAX];
} nxgl_attempt_entry_v2;

typedef struct nxgl_stack_attempt_request_v2 {
  uint32_t api_version;
  size_t struct_size;
  unsigned round_index;
  unsigned attempt_index;
  size_t candidate_index;
  const char *window_title;
  Uint32 window_flags;
  int display_index;
  nxgl_resolution resolution;
  nxgl_config_candidate candidate;
} nxgl_stack_attempt_request_v2;

typedef void (*nxgl_proc_v2)(void);
typedef int (*nxgl_stack_start_video_callback_v2)(
    void *userdata, int allow_initialize, int display_index,
    const nxgl_resolution_sources *fallback_facts,
    nxgl_resolution_sources *sources, char *backend, size_t backend_size,
    int *initialized_by_stack, char *error, size_t error_size);
typedef int (*nxgl_stack_stop_video_callback_v2)(void *userdata,
                                                 int initialized_by_stack,
                                                 char *error,
                                                 size_t error_size);
typedef int (*nxgl_stack_set_config_callback_v2)(
    void *userdata, const nxgl_config_candidate *candidate, char *error,
    size_t error_size);
typedef int (*nxgl_stack_create_window_callback_v2)(
    void *userdata, const nxgl_stack_attempt_request_v2 *request,
    nxgl_stack_handles_v2 *handles, char *error, size_t error_size);
typedef int (*nxgl_stack_create_context_callback_v2)(
    void *userdata, nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size);
typedef int (*nxgl_stack_make_current_callback_v2)(
    void *userdata, const nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size);
typedef nxgl_proc_v2 (*nxgl_stack_get_proc_callback_v2)(void *userdata,
                                                        const char *name);
typedef int (*nxgl_stack_query_actual_callback_v2)(
    void *userdata, nxgl_stack_handles_v2 *handles,
    nxgl_config_actual *actual, nxgl_egl_actual_v2 *egl, char *error,
    size_t error_size);
typedef int (*nxgl_stack_query_surface_callback_v2)(
    void *userdata, const nxgl_stack_handles_v2 *handles, int *window_width,
    int *window_height, int *drawable_width, int *drawable_height, char *error,
    size_t error_size);
typedef int (*nxgl_stack_validate_current_callback_v2)(
    void *userdata, const nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size);
typedef int (*nxgl_stack_release_attempt_callback_v2)(
    void *userdata, nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size);
typedef int (*nxgl_stack_present_callback_v2)(
    void *userdata, const nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size);

typedef struct nxgl_stack_ops_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_stack_owner_v2 owner;
  nxgl_stack_start_video_callback_v2 start_video;
  nxgl_stack_stop_video_callback_v2 stop_video;
  nxgl_stack_set_config_callback_v2 set_config;
  nxgl_stack_create_window_callback_v2 create_window;
  nxgl_stack_create_context_callback_v2 create_context;
  nxgl_stack_make_current_callback_v2 make_current;
  nxgl_stack_get_proc_callback_v2 get_proc_address;
  nxgl_stack_query_actual_callback_v2 query_actual;
  nxgl_stack_query_surface_callback_v2 query_surface;
  nxgl_stack_validate_current_callback_v2 validate_current;
  nxgl_stack_release_attempt_callback_v2 release_attempt;
  nxgl_stack_present_callback_v2 present;
} nxgl_stack_ops_v2;

/* Callback success is exactly NXGL_SUCCESS (zero).  nxgl freezes stack_ops by
 * value; its callback code and stack_userdata must remain alive and valid
 * until nxgl_close_v2(); the same applies to status/status_userdata.  Every
 * attempt handle is owned by that same stack and is released only through
 * release_attempt; shutdown order is release_attempt then stop_video on both
 * success-close and failure.  Each error buffer is bounded by its supplied
 * size and must be NUL-terminated. */

typedef struct nxgl_open_options_v2 {
  uint32_t api_version;
  size_t struct_size;
  uint32_t flags;
  const char *window_title;
  Uint32 window_flags;
  int display_index;
  unsigned drawable_wait_ms;
  const nxgl_resolution_sources *fallback_facts;
  const nxgl_engine_requirements *requirements;
  const nxgl_config_candidate *candidates;
  size_t candidate_count;
  /* NULL selects built-in SDL/EGL.  Custom stacks must clear both SDL-only
   * retry flags and own their single explicit negotiation round. */
  const nxgl_stack_ops_v2 *stack_ops;
  void *stack_userdata;
  nxgl_status_callback status;
  void *status_userdata;
} nxgl_open_options_v2;

typedef struct nxgl_report_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_report legacy;
  nxgl_stack_owner_v2 stack_owner;
  nxgl_stack_handles_v2 handles;
  nxgl_egl_actual_v2 egl;
  nxgl_open_stage_v2 final_stage;
  nxgl_open_reason_v2 final_reason;
  int environment_restored;
  int hints_restored;
  unsigned journal_count;
  unsigned journal_dropped;
  nxgl_attempt_entry_v2 journal[NXGL_ATTEMPT_JOURNAL_MAX];
} nxgl_report_v2;

void nxgl_open_options_v2_init(nxgl_open_options_v2 *options);
void nxgl_stack_ops_v2_init_sdl(nxgl_stack_ops_v2 *ops);
int nxgl_open_v2(const nxgl_open_options_v2 *options, nxgl_context **context,
                 nxgl_report_v2 *report);
/* NXGL_ERROR_BUSY is byte-atomic: a busy call does not touch either output.
 * A callback that re-enters nxgl_open_v2 while negotiation owns the arbiter
 * therefore receives BUSY without deadlock.  The terminal status callback is
 * invoked while the arbiter is still held and before either output is
 * published; it must return promptly, and any reentrant mutating nxgl call
 * receives BUSY.  Report handle copies are observations only: they transfer
 * no ownership and become stale at close. */
int nxgl_close_v2(nxgl_context *context);
/* close_v2 also returns BUSY without touching provider/context state.  On
 * NXGL_ERROR_ROLLBACK from open, a non-NULL context is a cleanup handle and
 * must be passed to close_v2 again; release_attempt always precedes
 * stop_video.  The arbiter retains no caller pointer and cannot make a stale
 * pointer safe: the caller must keep each context alive and must not race a
 * successful close against any query, present, close, or other use of that
 * context.  Queries likewise require a live, already-published context. */
nxgl_stack_owner_v2 nxgl_stack_owner(const nxgl_context *context);
const nxgl_report_v2 *nxgl_get_report_v2(const nxgl_context *context);

typedef enum nxgl_present_owner {
  /* Default: the engine/its EGL adapter owns presentation; nxgl is a no-op. */
  NXGL_PRESENT_ENGINE_OWNED = 0,
  /* Explicitly delegate presentation to SDL_GL_SwapWindow. */
  NXGL_PRESENT_SDL,
  /* Explicit raw-EGL, firmware blitter, or other port-specific adapter. */
  NXGL_PRESENT_ADAPTER
} nxgl_present_owner;

enum nxgl_present_flag {
  NXGL_PRESENT_FINISH_BEFORE_SWAP = 1u << 0,
  /* A-only clear to 1, preserving scissor, color mask, and clear color. */
  NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE = 1u << 1
};

typedef int (*nxgl_present_callback)(void *userdata, SDL_Window *window,
                                     SDL_GLContext context, char *error,
                                     size_t error_size);

typedef struct nxgl_present_policy {
  uint32_t api_version;
  size_t struct_size;
  nxgl_present_owner owner;
  uint32_t flags;
  nxgl_present_callback adapter;
  void *userdata;
} nxgl_present_policy;

typedef enum nxgl_present_quirk_v2 {
  NXGL_PRESENT_QUIRK_V2_NONE = 0,
  NXGL_PRESENT_QUIRK_V2_AMLOGIC_OSD_ALPHA_ONE
} nxgl_present_quirk_v2;

typedef enum nxgl_present_reason_v2 {
  NXGL_PRESENT_REASON_V2_NONE = 0,
  NXGL_PRESENT_REASON_V2_OBSERVED_OSD_ZERO_ALPHA
} nxgl_present_reason_v2;

typedef enum nxgl_present_stage_v2 {
  NXGL_PRESENT_STAGE_V2_NONE = 0,
  NXGL_PRESENT_STAGE_V2_ARBITER,
  NXGL_PRESENT_STAGE_V2_CURRENT,
  NXGL_PRESENT_STAGE_V2_SNAPSHOT,
  NXGL_PRESENT_STAGE_V2_ALPHA_ONE,
  NXGL_PRESENT_STAGE_V2_FINISH,
  NXGL_PRESENT_STAGE_V2_PRESENT,
  NXGL_PRESENT_STAGE_V2_RESTORE
} nxgl_present_stage_v2;

typedef struct nxgl_present_policy_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_present_owner owner;
  uint32_t flags;
  nxgl_present_quirk_v2 quirk;
  nxgl_present_reason_v2 reason;
} nxgl_present_policy_v2;

typedef struct nxgl_present_result_v2 {
  uint32_t api_version;
  size_t struct_size;
  int result;
  nxgl_present_stage_v2 failed_stage;
  int expected_context_current;
  int state_snapshotted;
  int state_restored;
} nxgl_present_result_v2;

void nxgl_present_policy_init(nxgl_present_policy *policy);
int nxgl_present(nxgl_context *context, const nxgl_present_policy *policy);
void nxgl_present_policy_v2_init(nxgl_present_policy_v2 *policy);
void nxgl_present_result_v2_init(nxgl_present_result_v2 *result);
/* context must remain live for the entire call; the arbiter serializes a
 * valid operation but does not retain or resurrect caller-owned pointers. */
int nxgl_present_v2(nxgl_context *context,
                    const nxgl_present_policy_v2 *policy,
                    nxgl_present_result_v2 *result);

typedef enum nxgl_surface_event_v2 {
  NXGL_SURFACE_EVENT_V2_FOCUS_GAINED = 0,
  NXGL_SURFACE_EVENT_V2_FOCUS_LOST,
  NXGL_SURFACE_EVENT_V2_MINIMIZED,
  NXGL_SURFACE_EVENT_V2_RESTORED,
  NXGL_SURFACE_EVENT_V2_RESIZED,
  NXGL_SURFACE_EVENT_V2_CONTEXT_LOST,
  NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED
} nxgl_surface_event_v2;

typedef struct nxgl_surface_observation_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxgl_surface_event_v2 event;
  int window_width;
  int window_height;
  int drawable_width;
  int drawable_height;
} nxgl_surface_observation_v2;

typedef struct nxgl_surface_state_v2 {
  uint32_t api_version;
  size_t struct_size;
  uint64_t generation;
  uint64_t context_generation;
  int focused;
  int minimized;
  int context_lost;
  int window_width;
  int window_height;
  int drawable_width;
  int drawable_height;
} nxgl_surface_state_v2;

void nxgl_surface_state_v2_init(nxgl_surface_state_v2 *state);
int nxgl_surface_observe_v2(nxgl_surface_state_v2 *state,
                            const nxgl_surface_observation_v2 *observation);

typedef struct nxgl_surface_metrics_input_v2 {
  uint32_t api_version;
  size_t struct_size;
  int display_width;
  int display_height;
  int drawable_width;
  int drawable_height;
  int viewport_x;
  int viewport_y;
  int viewport_width;
  int viewport_height;
  int render_target_width;
  int render_target_height;
} nxgl_surface_metrics_input_v2;

typedef struct nxgl_surface_metrics_v2 {
  uint32_t api_version;
  size_t struct_size;
  int display_width;
  int display_height;
  int drawable_width;
  int drawable_height;
  int viewport_x;
  int viewport_y;
  int viewport_width;
  int viewport_height;
  int render_target_width;
  int render_target_height;
  double drawable_per_display_scale_x;
  double drawable_per_display_scale_y;
  double render_target_per_viewport_scale_x;
  double render_target_per_viewport_scale_y;
} nxgl_surface_metrics_v2;

int nxgl_calculate_surface_metrics_v2(
    const nxgl_surface_metrics_input_v2 *input,
    nxgl_surface_metrics_v2 *metrics);

typedef enum nxgl_silhouette_diagnosis_v2 {
  NXGL_SILHOUETTE_V2_NOT_APPLICABLE = 0,
  NXGL_SILHOUETTE_V2_AUDIT_SAMPLER_WRAP_ATLAS,
  NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE
} nxgl_silhouette_diagnosis_v2;

typedef struct nxgl_silhouette_observation_v2 {
  uint32_t api_version;
  size_t struct_size;
  int pixels_are_black;
  int silhouette_is_intact;
  int uses_texture_atlas;
  int repeating_or_mirrored_uv;
  int forced_clamp_to_edge;
  int sampler_override_active;
} nxgl_silhouette_observation_v2;

int nxgl_classify_black_silhouette_v2(
    const nxgl_silhouette_observation_v2 *observation,
    nxgl_silhouette_diagnosis_v2 *diagnosis);

/* Frame proof: did anything the engine drew actually reach the presented
 * frame?  A run that draws nothing looks healthy in every other signal a
 * launcher has -- the loop ticks, audio plays, input arrives and the process
 * exits 0 -- so "black screen" is only reportable if someone measures the
 * frame.  The caller reads pixels back (it owns the GL context; nxgl never
 * touches GL state) and reports the share of non-black pixels of the best
 * sample it saw.  Sampling more than one frame is required: a title card can
 * legitimately be black at the first sample, and a single reading turns that
 * into a false verdict.
 *
 * UNKNOWN is not a pass. It means the run ended before any sample existed and
 * nothing about the image was proven either way. */
#define NXGL_FRAME_PROOF_DEFAULT_MIN_NON_BLACK 0.5 /* percent of pixels */

typedef enum nxgl_frame_proof_verdict_v2 {
  NXGL_FRAME_PROOF_V2_UNKNOWN = 0,
  NXGL_FRAME_PROOF_V2_BLACK,
  NXGL_FRAME_PROOF_V2_OK
} nxgl_frame_proof_verdict_v2;

typedef struct nxgl_frame_proof_observation_v2 {
  uint32_t api_version;
  size_t struct_size;
  int samples;                      /* frames actually read back */
  double best_non_black_percent;    /* best sample, 0..100 */
  double minimum_non_black_percent; /* 0 selects the default above */
} nxgl_frame_proof_observation_v2;

int nxgl_classify_frame_proof_v2(
    const nxgl_frame_proof_observation_v2 *observation,
    nxgl_frame_proof_verdict_v2 *verdict);

/* Launch context: was this run even able to produce an image?
 *
 * A port launched from a remote shell can fail to open a window for reasons
 * that have nothing to do with the port -- on RK3326/ArkOS the SDL window
 * never opens over SSH, the provider repair takes its pre-context branch and
 * every probe fails, while the exact same build launched from the device
 * frontend repairs itself and renders. A black frame collected that way is
 * evidence about the harness, not about the game, and reporting it as a defect
 * sends everyone hunting a regression that does not exist.
 *
 * So the verdict is asymmetric: a drawn frame proves the port draws no matter
 * how it was launched, but an empty frame only accuses the port when the
 * launch could have produced an image in the first place. */
typedef enum nxgl_launch_context_v2 {
  NXGL_LAUNCH_CONTEXT_V2_UNKNOWN = 0,
  NXGL_LAUNCH_CONTEXT_V2_FRONTEND, /* started by the device frontend */
  NXGL_LAUNCH_CONTEXT_V2_CONSOLE,  /* local VT with the display attached */
  NXGL_LAUNCH_CONTEXT_V2_REMOTE    /* remote shell: cannot prove an image */
} nxgl_launch_context_v2;

typedef struct nxgl_launch_observation_v2 {
  uint32_t api_version;
  size_t struct_size;
  int frontend_launched; /* launcher was invoked by the frontend */
  int remote_session;    /* SSH_CONNECTION or SSH_TTY present */
  int seat_vt;           /* stdin is a real VT, not a pseudo-terminal */
} nxgl_launch_observation_v2;

int nxgl_classify_launch_context_v2(
    const nxgl_launch_observation_v2 *observation,
    nxgl_launch_context_v2 *context);

/* GLES1 client-array bridge: the ROCKNIX g24p0 Wayland blob on Mali-G52 loses
 * the VBO association held by a GLES1 client-array descriptor and then reads
 * the descriptor's byte offset as a CPU address. The observed result is a
 * SIGSEGV in the first drawn frame with fault=0x3e inside libmali, so the game
 * dies at frame 1 with status 139 after a completely healthy boot.
 *
 * The workaround -- mirroring buffer objects in CPU memory and handing that one
 * driver ordinary client arrays -- is expensive and must never reach a healthy
 * stack, so the decision is gated on the full driver tuple rather than on the
 * vendor alone: Mali-G31, Mali-G310 and Mali-450 all run this same engine
 * correctly and must stay on the direct path. */
typedef struct nxgl_client_array_observation_v2 {
  uint32_t api_version;
  size_t struct_size;
  const char *video_driver; /* SDL video driver, e.g. "wayland", "KMSDRM" */
  const char *renderer;     /* GL_RENDERER */
  const char *version;      /* GL_VERSION */
} nxgl_client_array_observation_v2;

int nxgl_classify_client_array_bridge_v2(
    const nxgl_client_array_observation_v2 *observation, int *bridge_required);

/* Whether a BLACK frame proof from this context may be reported as a port
 * defect. Conclusive contexts accuse the port; the rest demand a re-test. */
int nxgl_frame_proof_is_conclusive_v2(nxgl_launch_context_v2 context,
                                      int *conclusive);

/* Pure, conservative prefilter for adapter-owned GL-provider recovery.
 * A filename that passes is only a candidate: callers must still validate the
 * required symbols and open the real EGL/window stack.  In particular,
 * "wayland-gbm" is compatible with a direct KMSDRM backend because the
 * explicit GBM transport is stronger evidence than the auxiliary Wayland
 * token.  Dummy/stub/headless providers are never compatible. */
int nxgl_provider_name_compatible(const char *video_backend,
                                  const char *provider_name);

/* Pure authorization plan for the narrow crossed-provider recovery used by
 * SDL adapters. A positive plan does not mutate the environment or load a
 * provider: the adapter still owns candidate scanning, symbol validation,
 * teardown and a one-shot re-exec. Both SDL_VIDEO_EGL_DRIVER and
 * SDL_VIDEO_GL_DRIVER must be bound to the exact same validated object. */
typedef enum nxgl_sdl_provider_pair_plan {
  NXGL_SDL_PROVIDER_PAIR_INVALID = -1,
  NXGL_SDL_PROVIDER_PAIR_NO_ACTION = 0,
  NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT = 1
} nxgl_sdl_provider_pair_plan;

nxgl_sdl_provider_pair_plan nxgl_plan_sdl_provider_pair(
    const char *video_backend, const char *renderer,
    const char *provider_name, int window_opened, int context_current,
    int drawable_positive, int exports_egl, int exports_engine_gles);

/* Pure authorization plan for a second, narrower crossed-provider symptom:
 * SDL video starts on a real direct-display backend, but every window or
 * context create fails because its resolved EGL and GLES sonames name
 * different providers.
 * The adapter remains responsible for finding exactly one candidate object,
 * checking that both SDL variables resolve to that same object, validating
 * symbols, changing/restoring the environment, and retrying nxgl once. */
typedef struct nxgl_sdl_precontext_recovery_v2 {
  uint32_t api_version;
  size_t struct_size;
  const char *video_backend;
  const char *provider_name;
  nxgl_open_stage_v2 failed_stage;
  nxgl_open_reason_v2 final_reason;
  int attempts_exhausted;
  int inherited_provider_hint;
  int same_object;
  int exports_egl;
  int exports_engine_gles;
} nxgl_sdl_precontext_recovery_v2;

void nxgl_sdl_precontext_recovery_v2_init(
    nxgl_sdl_precontext_recovery_v2 *recovery);
nxgl_sdl_provider_pair_plan nxgl_plan_sdl_precontext_recovery_v2(
    const nxgl_sdl_precontext_recovery_v2 *recovery);

int nxgl_format_profile_line(const nxgl_report *report, char *output,
                             size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_H */
