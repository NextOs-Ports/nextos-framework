/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXCOMPAT_H
#define NXCOMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXCOMPAT_API_VERSION_V1 1u
#define NXCOMPAT_API_VERSION_V2 2u
#define NXCOMPAT_API_VERSION NXCOMPAT_API_VERSION_V2
#define NXCOMPAT_VERSION "0.2.0"

#define NXCOMPAT_NAME_MAX 64u
#define NXCOMPAT_PATH_MAX 512u
#define NXCOMPAT_DETAIL_MAX 192u
#define NXCOMPAT_MAX_ACTIONS 16u
#define NXCOMPAT_BACKEND_ENV_MAX 4u
#define NXCOMPAT_MAX_OBSERVATIONS 12u

/* Public return values are stable and append-only.  Existing API-v1 entry
 * points still treat every non-zero value as failure. */
typedef enum nxcompat_result_code {
  NXCOMPAT_OK = 0,
  NXCOMPAT_INVALID = -1,
  NXCOMPAT_BUSY = -2,
  NXCOMPAT_FAILED = -3,
  NXCOMPAT_ROLLBACK_FAILED = -4
} nxcompat_result_code;

/* Machine-readable reasons.  Numeric values are part of the public contract;
 * new values may be appended, never renumbered or inferred from prose. */
typedef enum nxcompat_reason_code {
  NXCOMPAT_REASON_NONE = 0,
  NXCOMPAT_REASON_PROBE_COMPLETE = 1,

  NXCOMPAT_REASON_INVALID_ARGUMENT = 100,
  NXCOMPAT_REASON_UNSUPPORTED_API = 101,
  NXCOMPAT_REASON_STRUCT_TOO_SMALL = 102,
  NXCOMPAT_REASON_ARBITER_BUSY = 103,
  NXCOMPAT_REASON_PROVIDER_CONTRACT = 104,

  NXCOMPAT_REASON_OBSERVATION_ABSENT = 200,
  NXCOMPAT_REASON_OBSERVATION_MALFORMED = 201,
  NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE = 202,
  NXCOMPAT_REASON_PROCESS_ARCH_VERIFIED = 210,
  NXCOMPAT_REASON_KERNEL_ARCH_VERIFIED = 211,
  NXCOMPAT_REASON_USERLAND_ARCH_VERIFIED = 212,
  NXCOMPAT_REASON_MEMAVAILABLE_VERIFIED = 220,
  NXCOMPAT_REASON_CGROUP_V1_LIMIT_VERIFIED = 221,
  NXCOMPAT_REASON_CGROUP_V2_LIMIT_VERIFIED = 222,
  NXCOMPAT_REASON_RLIMIT_AS_VERIFIED = 223,
  NXCOMPAT_REASON_EFFECTIVE_MEMORY_MINIMUM = 224,
  NXCOMPAT_REASON_EFFECTIVE_MEMORY_UNKNOWN = 225,
  NXCOMPAT_REASON_LIMIT_UNBOUNDED = 226,

  NXCOMPAT_REASON_BACKEND_INHERITED_OK = 300,
  NXCOMPAT_REASON_BACKEND_AUTODETECT_OK = 301,
  NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE = 302,
  NXCOMPAT_REASON_BACKEND_ATTEMPT_BUSY = 303,
  NXCOMPAT_REASON_BACKEND_ATTEMPT_FATAL = 304,
  NXCOMPAT_REASON_BACKEND_CLEANUP_FAILED = 305,
  NXCOMPAT_REASON_ENV_CLEAR_FAILED = 306,
  NXCOMPAT_REASON_ENV_RESTORE_FAILED = 307,
  NXCOMPAT_REASON_BACKEND_NAME_REJECTED = 308,
  NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT = 309,
  NXCOMPAT_REASON_VIDEO_OPEN_FAILED = 310,
  NXCOMPAT_REASON_AUDIO_OPEN_FAILED = 311,
  NXCOMPAT_REASON_BACKEND_FOREIGN_OWNED = 312,
  NXCOMPAT_REASON_AUDIO_DEVICE_INVALID = 313,

  NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE = 400,
  NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED = 401,
  NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED = 402,
  NXCOMPAT_REASON_PLAN_POLICY_NOT_NEEDED = 403,
  NXCOMPAT_REASON_ENV_LATE_VALUE_PRESERVED = 404,
  NXCOMPAT_REASON_ENV_APPLIED = 405,
  NXCOMPAT_REASON_ENV_APPLY_FAILED = 406,
  NXCOMPAT_REASON_ENV_ROLLED_BACK = 407,
  NXCOMPAT_REASON_ENV_ROLLBACK_FAILED = 408,
  NXCOMPAT_REASON_PLAN_COMPLETE = 409,
  NXCOMPAT_REASON_ENV_APPLY_COMPLETE = 410,

  NXCOMPAT_REASON_CAPABILITY_PUBLISHED = 500,
  NXCOMPAT_REASON_CAPABILITY_LOST = 501,
  NXCOMPAT_REASON_CAPABILITY_STALE = 502,
  NXCOMPAT_REASON_REQUIREMENT_UNKNOWN = 510,
  NXCOMPAT_REASON_REQUIREMENT_DUPLICATE = 511,
  NXCOMPAT_REASON_REQUIREMENT_PENDING = 512,
  NXCOMPAT_REASON_REQUIREMENT_MISSING = 513,
  NXCOMPAT_REASON_REQUIREMENT_SATISFIED = 514,
  NXCOMPAT_REASON_GRAPHICS_WINDOW_OPENED = 520,
  NXCOMPAT_REASON_GRAPHICS_GLES_OPENED = 521,
  NXCOMPAT_REASON_GRAPHICS_EGL_OPENED = 522,
  NXCOMPAT_REASON_GRAPHICS_EGL_CONFIG_OPENED = 523,
  NXCOMPAT_REASON_GRAPHICS_DRAWABLE_OPENED = 524,
  NXCOMPAT_REASON_AUDIO_OUTPUT_OPENED = 530,
  NXCOMPAT_REASON_INPUT_CONTROLLER_API_ACTIVE = 540,
  NXCOMPAT_REASON_INPUT_CONTROLLER_CONNECTED = 541,
  NXCOMPAT_REASON_INPUT_HOTPLUG_ACTIVE = 542,
  NXCOMPAT_REASON_INPUT_CONTROLLER_LOST = 543,
  NXCOMPAT_REASON_REPORT_SANITIZED = 550
} nxcompat_reason_code;

typedef enum nxcompat_arch {
  NXCOMPAT_ARCH_UNKNOWN = 0,
  NXCOMPAT_ARCH_ARMV7,
  NXCOMPAT_ARCH_AARCH64,
  NXCOMPAT_ARCH_I386,
  NXCOMPAT_ARCH_X86_64
} nxcompat_arch;

typedef enum nxcompat_observation_id {
  NXCOMPAT_OBSERVATION_PROCESS_MACHINE = 1,
  NXCOMPAT_OBSERVATION_KERNEL_MACHINE = 2,
  NXCOMPAT_OBSERVATION_USERLAND_MACHINE = 3,
  NXCOMPAT_OBSERVATION_MEM_TOTAL_KIB = 10,
  NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB = 11,
  NXCOMPAT_OBSERVATION_SWAP_TOTAL_KIB = 12,
  NXCOMPAT_OBSERVATION_VM_SIZE_KIB = 13,
  NXCOMPAT_OBSERVATION_CGROUP_MODE = 20,
  NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES = 21,
  NXCOMPAT_OBSERVATION_CGROUP_CURRENT_BYTES = 22,
  NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES = 30
} nxcompat_observation_id;

typedef enum nxcompat_observation_state {
  NXCOMPAT_OBSERVATION_ABSENT = 0,
  NXCOMPAT_OBSERVATION_PRESENT = 1,
  NXCOMPAT_OBSERVATION_REJECTED = 2
} nxcompat_observation_state;

/* Providers return only fixed observation IDs.  A custom provider is an
 * exclusive source: nxcompat never falls back to the host, environment or
 * filesystem for a missing custom value. */
typedef enum nxcompat_provider_result {
  NXCOMPAT_PROVIDER_VALUE = 0,
  NXCOMPAT_PROVIDER_ABSENT = 1,
  NXCOMPAT_PROVIDER_ERROR = 2
} nxcompat_provider_result;

typedef nxcompat_provider_result (*nxcompat_probe_read_observation)(
    void *userdata, nxcompat_observation_id id, char *value,
    size_t value_size);

typedef struct nxcompat_probe_provider {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_probe_read_observation read;
  void *userdata;
} nxcompat_probe_provider;

typedef struct nxcompat_observation {
  nxcompat_observation_id id;
  nxcompat_observation_state state;
  nxcompat_reason_code reason;
  uint64_t numeric_value;
  nxcompat_arch arch_value;
} nxcompat_observation;

/* The registry source-of-truth owns the finite capability IDs and names.
 * Probe providers cannot mint capabilities or free-form reason strings. */
typedef uint32_t nxcompat_capability_id;

typedef enum nxcompat_evidence_state {
  NXCOMPAT_EVIDENCE_ABSENT = 0,
  NXCOMPAT_EVIDENCE_OBSERVED = 1,
  NXCOMPAT_EVIDENCE_OPENED = 2,
  NXCOMPAT_EVIDENCE_ACTIVE = 3,
  NXCOMPAT_EVIDENCE_LOST = 4
} nxcompat_evidence_state;

typedef enum nxcompat_phase {
  NXCOMPAT_PHASE_PREFLIGHT = 0,
  NXCOMPAT_PHASE_GRAPHICS = 1,
  NXCOMPAT_PHASE_AUDIO = 2,
  NXCOMPAT_PHASE_INPUT = 3,
  NXCOMPAT_PHASE_READY = 4
} nxcompat_phase;

typedef enum nxcompat_source {
  NXCOMPAT_SOURCE_PROBE = 0,
  NXCOMPAT_SOURCE_NXGL = 1,
  NXCOMPAT_SOURCE_SDL2_AUDIO = 2,
  NXCOMPAT_SOURCE_NXINPUT = 3,
  NXCOMPAT_SOURCE_ENGINE_ADAPTER = 4
} nxcompat_source;

typedef struct nxcompat_capability_evidence {
  nxcompat_capability_id capability_id;
  nxcompat_evidence_state state;
  nxcompat_phase phase;
  nxcompat_source source;
  nxcompat_reason_code reason;
  /* Monotonic per-context generation used to publish hotplug/LOST changes. */
  uint64_t generation;
} nxcompat_capability_evidence;

#define NXCOMPAT_CAPABILITY_COUNT 31u
#define NXCOMPAT_MAX_REQUIREMENTS 32u
#define NXCOMPAT_REQUIREMENTS_TEXT_MAX                                     \
  (NXCOMPAT_MAX_REQUIREMENTS * NXCOMPAT_NAME_MAX)
#define NXCOMPAT_RECEIPT_EXTENSIONS_MAX 1024u

/* Numeric values follow capabilities-v1.json exactly.  They are stable and
 * append-only; free-form strings can never create another capability. */
typedef enum nxcompat_capability_id_v1 {
  NXCOMPAT_CAPABILITY_HOST_PORTMASTER = 0,
  NXCOMPAT_CAPABILITY_HOST_ARMHF_LIBS = 1,
  NXCOMPAT_CAPABILITY_HOST_AARCH64_LIBS = 2,
  NXCOMPAT_CAPABILITY_HOST_I386_LIBS = 3,
  NXCOMPAT_CAPABILITY_HOST_SESSION_RUNTIME = 4,
  NXCOMPAT_CAPABILITY_HOST_SHORT_MEMORY = 5,
  NXCOMPAT_CAPABILITY_HOST_FUSE_LIKE_FILESYSTEM = 6,
  NXCOMPAT_CAPABILITY_HOST_NETWORK_FILESYSTEM = 7,
  NXCOMPAT_CAPABILITY_HOST_FBDEV = 8,
  NXCOMPAT_CAPABILITY_HOST_DRM = 9,
  NXCOMPAT_CAPABILITY_HOST_DRM_CONNECTED = 10,
  NXCOMPAT_CAPABILITY_HOST_WAYLAND = 11,
  NXCOMPAT_CAPABILITY_HOST_X11 = 12,
  NXCOMPAT_CAPABILITY_AUDIO_PULSE_SOCKET = 13,
  NXCOMPAT_CAPABILITY_AUDIO_PIPEWIRE_SOCKET = 14,
  NXCOMPAT_CAPABILITY_AUDIO_ALSA = 15,
  NXCOMPAT_CAPABILITY_GRAPHICS_WINDOW = 16,
  NXCOMPAT_CAPABILITY_GRAPHICS_GLES2 = 17,
  NXCOMPAT_CAPABILITY_GRAPHICS_GLES3 = 18,
  NXCOMPAT_CAPABILITY_GRAPHICS_EGL = 19,
  NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG = 20,
  NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE = 21,
  NXCOMPAT_CAPABILITY_GRAPHICS_ETC1 = 22,
  NXCOMPAT_CAPABILITY_GRAPHICS_ETC2 = 23,
  NXCOMPAT_CAPABILITY_GRAPHICS_ASTC = 24,
  NXCOMPAT_CAPABILITY_GRAPHICS_NPOT_FULL = 25,
  NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN = 26,
  NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING = 27,
  NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API = 28,
  NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED = 29,
  NXCOMPAT_CAPABILITY_INPUT_HOTPLUG = 30
} nxcompat_capability_id_v1;

typedef enum nxcompat_capability_role {
  NXCOMPAT_ROLE_OBSERVATION = 0,
  NXCOMPAT_ROLE_BASELINE_GRAPHICS = 1,
  NXCOMPAT_ROLE_PORT_DECLARED = 2,
  NXCOMPAT_ROLE_OPTIONAL_ENHANCEMENT = 3,
  NXCOMPAT_ROLE_OPTIONAL_RUNTIME = 4
} nxcompat_capability_role;

typedef struct nxcompat_capability_definition {
  nxcompat_capability_id capability_id;
  const char *name;
  nxcompat_phase phase;
  nxcompat_source source;
  nxcompat_evidence_state minimum_evidence;
  nxcompat_capability_role role;
} nxcompat_capability_definition;

typedef struct nxcompat_registry nxcompat_registry;

typedef struct nxcompat_requirements {
  uint32_t api_version;
  size_t struct_size;
  size_t count;
  nxcompat_capability_id capability_ids[NXCOMPAT_MAX_REQUIREMENTS];
} nxcompat_requirements;

typedef enum nxcompat_requirement_state {
  NXCOMPAT_REQUIREMENT_PENDING = 0,
  NXCOMPAT_REQUIREMENT_SATISFIED = 1,
  NXCOMPAT_REQUIREMENT_MISSING = 2
} nxcompat_requirement_state;

typedef struct nxcompat_requirement_result {
  nxcompat_capability_id capability_id;
  nxcompat_requirement_state state;
  nxcompat_reason_code reason;
} nxcompat_requirement_result;

typedef struct nxcompat_requirement_report {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_phase phase;
  size_t count;
  size_t satisfied_count;
  size_t pending_count;
  size_t missing_count;
  nxcompat_reason_code final_reason;
  nxcompat_requirement_result results[NXCOMPAT_MAX_REQUIREMENTS];
} nxcompat_requirement_report;

typedef enum nxcompat_memory_class {
  NXCOMPAT_MEMORY_UNKNOWN = 0,
  NXCOMPAT_MEMORY_SHORT,
  NXCOMPAT_MEMORY_MEDIUM,
  NXCOMPAT_MEMORY_HIGH
} nxcompat_memory_class;

typedef enum nxcompat_filesystem_class {
  NXCOMPAT_FILESYSTEM_UNKNOWN = 0,
  NXCOMPAT_FILESYSTEM_POSIX,
  NXCOMPAT_FILESYSTEM_FUSE_LIKE,
  NXCOMPAT_FILESYSTEM_NETWORK
} nxcompat_filesystem_class;

enum nxcompat_capability {
  NXCOMPAT_CAP_FBDEV = UINT64_C(1) << 0,
  NXCOMPAT_CAP_DRM = UINT64_C(1) << 1,
  NXCOMPAT_CAP_DRM_CONNECTED = UINT64_C(1) << 2,
  NXCOMPAT_CAP_WAYLAND = UINT64_C(1) << 3,
  NXCOMPAT_CAP_X11 = UINT64_C(1) << 4,
  NXCOMPAT_CAP_PULSE_SOCKET = UINT64_C(1) << 5,
  NXCOMPAT_CAP_PIPEWIRE_SOCKET = UINT64_C(1) << 6,
  NXCOMPAT_CAP_ALSA = UINT64_C(1) << 7,
  NXCOMPAT_CAP_PORTMASTER = UINT64_C(1) << 8,
  NXCOMPAT_CAP_ARMHF_LIBS = UINT64_C(1) << 9,
  NXCOMPAT_CAP_AARCH64_LIBS = UINT64_C(1) << 10,
  NXCOMPAT_CAP_I386_LIBS = UINT64_C(1) << 11,
  NXCOMPAT_CAP_SHORT_MEMORY = UINT64_C(1) << 12,
  NXCOMPAT_CAP_FUSE_LIKE_FILESYSTEM = UINT64_C(1) << 13,
  NXCOMPAT_CAP_SDL_VIDEO_INHERITED = UINT64_C(1) << 14,
  NXCOMPAT_CAP_SDL_AUDIO_INHERITED = UINT64_C(1) << 15,
  NXCOMPAT_CAP_CONTROLLER_MAPPING = UINT64_C(1) << 16
};

enum nxcompat_memory_source {
  NXCOMPAT_MEMORY_SOURCE_MEMAVAILABLE = 1u << 0,
  NXCOMPAT_MEMORY_SOURCE_CGROUP = 1u << 1,
  NXCOMPAT_MEMORY_SOURCE_RLIMIT_AS = 1u << 2
};

typedef struct nxcompat_probe_result {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_reason_code final_reason;
  size_t observation_count;
  nxcompat_observation observations[NXCOMPAT_MAX_OBSERVATIONS];

  nxcompat_arch process_arch;
  nxcompat_arch kernel_arch;
  nxcompat_arch userland_arch;
  uint32_t cgroup_version;
  uint32_t memory_sources;

  uint64_t memory_total_kib;
  uint64_t memory_available_kib;
  uint64_t swap_total_kib;
  uint64_t process_vm_kib;
  uint64_t cgroup_limit_kib;
  uint64_t cgroup_current_kib;
  uint64_t rlimit_as_kib;
  uint64_t memory_effective_kib;
} nxcompat_probe_result;

typedef struct nxcompat_probe_options {
  uint32_t api_version;
  size_t struct_size;
  const char *port_id;
  const char *game_dir;
  const char *portmaster_dir;

  /* NULL means the live root.  A bounded absolute root without . or ..
   * components redirects only filesystem discovery for test fixtures; the
   * default provider still observes this process/kernel/environment and must
   * never be presented as an authoritative offline-device report.  Use an
   * exclusive API-v2 custom provider for a hermetic offline report. */
  const char *probe_root;

  /* API v2 tail.  provider != NULL makes the entire probe hermetic: only the
   * callback is consulted and legacy host/env/filesystem discovery is not
   * used.  result is optional, but is the authoritative observations record. */
  const nxcompat_probe_provider *provider;
  nxcompat_probe_result *result;
} nxcompat_probe_options;

#define NXCOMPAT_PROBE_OPTIONS_V1_SIZE                                      \
  offsetof(nxcompat_probe_options, provider)

typedef struct nxcompat_host {
  uint32_t api_version;
  size_t struct_size;
  /* Legacy API-v1 observations only.  These bits are never verified runtime
   * requirements; API-v2 decisions consume registry evidence instead. */
  uint64_t capabilities;

  nxcompat_arch process_arch;
  nxcompat_arch kernel_arch;
  nxcompat_memory_class memory_class;
  nxcompat_filesystem_class filesystem_class;
  uint64_t memory_total_kib;
  uint64_t swap_total_kib;
  int display_width;
  int display_height;

  char port_id[NXCOMPAT_NAME_MAX];
  char device_model[NXCOMPAT_DETAIL_MAX];
  char kernel_machine[NXCOMPAT_NAME_MAX];
  char libc_version[NXCOMPAT_NAME_MAX];
  char os_id[NXCOMPAT_NAME_MAX];
  char os_version[NXCOMPAT_NAME_MAX];
  char filesystem_type[NXCOMPAT_NAME_MAX];
  char inherited_video_driver[NXCOMPAT_NAME_MAX];
  char inherited_audio_driver[NXCOMPAT_NAME_MAX];

  char game_dir[NXCOMPAT_PATH_MAX];
  char portmaster_dir[NXCOMPAT_PATH_MAX];
  char session_runtime_dir[NXCOMPAT_PATH_MAX];
  char wayland_socket[NXCOMPAT_PATH_MAX];
  char pulse_socket[NXCOMPAT_PATH_MAX];
  char pipewire_socket[NXCOMPAT_PATH_MAX];
  char drm_connector[NXCOMPAT_PATH_MAX];
  char display_source[NXCOMPAT_PATH_MAX];
  char armhf_pipewire_modules[NXCOMPAT_PATH_MAX];
  char armhf_spa_plugins[NXCOMPAT_PATH_MAX];
  char armhf_alsa_plugins[NXCOMPAT_PATH_MAX];
  char controller_db[NXCOMPAT_PATH_MAX];
} nxcompat_host;

enum nxcompat_policy_flag {
  /* These policies only plan capability-gated, process-local environment
   * changes. No SDL display/audio backend is ever selected by name. */
  NXCOMPAT_POLICY_SESSION_RUNTIME = 1u << 0,
  NXCOMPAT_POLICY_PULSE_SERVER = 1u << 1,
  NXCOMPAT_POLICY_ARMHF_AUDIO_MODULES = 1u << 2,
  NXCOMPAT_POLICY_CONTROLLER_DB = 1u << 3,
  NXCOMPAT_POLICY_XBOX_BUTTON_LABELS = 1u << 4,
  NXCOMPAT_POLICY_LOW_MEMORY_ARENAS = 1u << 5
};

#define NXCOMPAT_POLICY_AUTOMATIC_SAFE                                      \
  (NXCOMPAT_POLICY_SESSION_RUNTIME | NXCOMPAT_POLICY_PULSE_SERVER |         \
   NXCOMPAT_POLICY_ARMHF_AUDIO_MODULES | NXCOMPAT_POLICY_CONTROLLER_DB |    \
   NXCOMPAT_POLICY_XBOX_BUTTON_LABELS)

typedef struct nxcompat_plan_options {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_arch runtime_arch;
  uint32_t policy_flags;
  unsigned low_memory_arena_max;
} nxcompat_plan_options;

typedef enum nxcompat_action_id {
  NXCOMPAT_ACTION_NONE = 0,
  NXCOMPAT_ACTION_SESSION_RUNTIME,
  NXCOMPAT_ACTION_PULSE_SERVER,
  NXCOMPAT_ACTION_PIPEWIRE_MODULE_DIR,
  NXCOMPAT_ACTION_SPA_PLUGIN_DIR,
  NXCOMPAT_ACTION_ALSA_PLUGIN_DIR,
  NXCOMPAT_ACTION_CONTROLLER_DB,
  NXCOMPAT_ACTION_XBOX_BUTTON_LABELS,
  NXCOMPAT_ACTION_MALLOC_ARENAS
} nxcompat_action_id;

typedef enum nxcompat_action_state {
  NXCOMPAT_ACTION_UNAVAILABLE = 0,
  NXCOMPAT_ACTION_NOT_NEEDED,
  NXCOMPAT_ACTION_PLANNED,
  NXCOMPAT_ACTION_APPLIED,
  NXCOMPAT_ACTION_FAILED
} nxcompat_action_state;

typedef struct nxcompat_action {
  nxcompat_action_id id;
  nxcompat_action_state state;
  char variable[NXCOMPAT_NAME_MAX];
  char value[NXCOMPAT_PATH_MAX];
  char reason[NXCOMPAT_DETAIL_MAX];
} nxcompat_action;

typedef struct nxcompat_plan {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_arch runtime_arch;
  size_t action_count;
  nxcompat_action actions[NXCOMPAT_MAX_ACTIONS];
} nxcompat_plan;

typedef enum nxcompat_action_state_v2 {
  NXCOMPAT_ACTION_V2_UNAVAILABLE = 0,
  NXCOMPAT_ACTION_V2_NOT_NEEDED = 1,
  NXCOMPAT_ACTION_V2_PLANNED = 2,
  NXCOMPAT_ACTION_V2_APPLIED = 3,
  NXCOMPAT_ACTION_V2_FAILED = 4,
  NXCOMPAT_ACTION_V2_ROLLED_BACK = 5,
  NXCOMPAT_ACTION_V2_ROLLBACK_FAILED = 6
} nxcompat_action_state_v2;

typedef struct nxcompat_action_v2 {
  nxcompat_action_id id;
  nxcompat_action_state_v2 state;
  nxcompat_reason_code reason_code;
  char variable[NXCOMPAT_NAME_MAX];
  char value[NXCOMPAT_PATH_MAX];
  char reason[NXCOMPAT_DETAIL_MAX];
} nxcompat_action_v2;

typedef struct nxcompat_plan_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_arch runtime_arch;
  size_t action_count;
  nxcompat_action_v2 actions[NXCOMPAT_MAX_ACTIONS];
  nxcompat_reason_code final_reason;
  size_t apply_count;
  size_t rollback_count;
  /* True iff every environment entry is byte-identical to the pre-apply
   * snapshot, including BUSY paths on which no mutation occurred. */
  int env_restored;
} nxcompat_plan_v2;

typedef enum nxcompat_status_kind {
  NXCOMPAT_STATUS_DEVICE = 0,
  NXCOMPAT_STATUS_FIX,
  NXCOMPAT_STATUS_BACKEND,
  NXCOMPAT_STATUS_WARNING
} nxcompat_status_kind;

typedef void (*nxcompat_status_callback)(void *userdata,
                                         nxcompat_status_kind kind,
                                         const char *message);

typedef enum nxcompat_backend_kind {
  NXCOMPAT_BACKEND_VIDEO = 0,
  NXCOMPAT_BACKEND_AUDIO
} nxcompat_backend_kind;

typedef enum nxcompat_backend_state {
  NXCOMPAT_BACKEND_UNTESTED = 0,
  NXCOMPAT_BACKEND_INHERITED_OK,
  NXCOMPAT_BACKEND_AUTODETECT_OK,
  NXCOMPAT_BACKEND_RECOVERED_BY_AUTODETECT,
  NXCOMPAT_BACKEND_RECOVERED_BY_DISCOVERY,
  NXCOMPAT_BACKEND_FAILED
} nxcompat_backend_state;

/* attempt() returns zero only after the backend is actually usable. reset()
 * is required and must undo a partial first attempt before nxcompat retries.
 * current_name()
 * reports the backend that really opened, never the requested environment. */
typedef int (*nxcompat_backend_attempt)(void *userdata, char *error,
                                        size_t error_size);
typedef void (*nxcompat_backend_reset)(void *userdata);
typedef const char *(*nxcompat_backend_current_name)(void *userdata);
typedef int (*nxcompat_backend_accept_name)(void *userdata,
                                            const char *backend_name);
/* Optional capability scan after normal selection really failed. The callback
 * may test only backends enumerated by the runtime itself, must reject fake
 * outputs, and reports how many real opens it attempted. */
typedef int (*nxcompat_backend_discover)(void *userdata,
                                         unsigned *attempt_count, char *error,
                                         size_t error_size);

typedef struct nxcompat_backend_options {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_backend_kind kind;
  const char *environment_names[NXCOMPAT_BACKEND_ENV_MAX];
  nxcompat_backend_attempt attempt;
  nxcompat_backend_reset reset;
  nxcompat_backend_current_name current_name;
  nxcompat_backend_accept_name accept_name;
  nxcompat_backend_discover discover;
  nxcompat_status_callback status;
  void *userdata;
  void *status_userdata;
} nxcompat_backend_options;

typedef struct nxcompat_backend_result {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_backend_kind kind;
  nxcompat_backend_state state;
  unsigned attempt_count;
  unsigned inherited_count;
  char selected[NXCOMPAT_NAME_MAX];
  char first_error[NXCOMPAT_DETAIL_MAX];
  char final_error[NXCOMPAT_DETAIL_MAX];
} nxcompat_backend_result;

/* API-v1 discovery remains layout-compatible for old callers, but is
 * deprecated.  API v2 deliberately has no discover callback: after a failed
 * inherited attempt it permits exactly one normal autodetect attempt. */
typedef enum nxcompat_backend_attempt_outcome {
  NXCOMPAT_BACKEND_ATTEMPT_OK = 0,
  NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE = 1,
  NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY = 2,
  NXCOMPAT_BACKEND_ATTEMPT_FATAL_FAILURE = 3
} nxcompat_backend_attempt_outcome;

typedef struct nxcompat_backend_attempt_report {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_reason_code reason;
  char selected[NXCOMPAT_NAME_MAX];
  char error[NXCOMPAT_DETAIL_MAX];
} nxcompat_backend_attempt_report;

typedef nxcompat_backend_attempt_outcome (*nxcompat_backend_attempt_v2)(
    void *userdata, nxcompat_backend_attempt_report *report);
/* Called exactly once after every attempt, including a successful attempt.
 * The selected name is captured in the attempt report before cleanup. */
typedef nxcompat_result_code (*nxcompat_backend_cleanup_v2)(
    void *userdata, char *error, size_t error_size);

typedef struct nxcompat_backend_options_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_backend_kind kind;
  const char *environment_names[NXCOMPAT_BACKEND_ENV_MAX];
  nxcompat_backend_attempt_v2 attempt;
  nxcompat_backend_cleanup_v2 cleanup;
  nxcompat_backend_accept_name accept_name;
  nxcompat_status_callback status;
  void *userdata;
  void *status_userdata;
} nxcompat_backend_options_v2;

typedef enum nxcompat_backend_state_v2 {
  NXCOMPAT_BACKEND_V2_UNTESTED = 0,
  NXCOMPAT_BACKEND_V2_INHERITED_OK = 1,
  NXCOMPAT_BACKEND_V2_AUTODETECT_OK = 2,
  NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT = 3,
  NXCOMPAT_BACKEND_V2_FAILED = 4,
  NXCOMPAT_BACKEND_V2_BUSY = 5,
  NXCOMPAT_BACKEND_V2_ROLLBACK_FAILED = 6
} nxcompat_backend_state_v2;

typedef struct nxcompat_backend_result_v2 {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_backend_kind kind;
  nxcompat_backend_state_v2 state;
  unsigned attempt_count;
  unsigned cleanup_count;
  unsigned inherited_count;
  int hints_removed;
  /* True iff every declared environment value is byte-identical to the
   * initial snapshot.  It is also true for BUSY/invalid paths with no write. */
  int env_restored;
  nxcompat_reason_code first_reason;
  nxcompat_reason_code final_reason;
  char selected[NXCOMPAT_NAME_MAX];
  char first_error[NXCOMPAT_DETAIL_MAX];
  char final_error[NXCOMPAT_DETAIL_MAX];
} nxcompat_backend_result_v2;

typedef enum nxcompat_gpu_family {
  NXCOMPAT_GPU_UNKNOWN = 0,
  NXCOMPAT_GPU_MALI_4XX,
  NXCOMPAT_GPU_MALI_MODERN,
  NXCOMPAT_GPU_PANFROST,
  NXCOMPAT_GPU_OTHER
} nxcompat_gpu_family;

enum nxcompat_graphics_capability {
  NXCOMPAT_GRAPHICS_GLES = UINT64_C(1) << 0,
  NXCOMPAT_GRAPHICS_GLES2 = UINT64_C(1) << 1,
  NXCOMPAT_GRAPHICS_GLES3 = UINT64_C(1) << 2,
  NXCOMPAT_GRAPHICS_ETC1 = UINT64_C(1) << 3,
  NXCOMPAT_GRAPHICS_ETC2 = UINT64_C(1) << 4,
  NXCOMPAT_GRAPHICS_ASTC = UINT64_C(1) << 5,
  NXCOMPAT_GRAPHICS_NPOT_FULL = UINT64_C(1) << 6
};

typedef struct nxcompat_graphics_options {
  uint32_t api_version;
  size_t struct_size;
  const char *video_driver;
  const char *vendor;
  const char *renderer;
  const char *version;
  const char *shading_language_version;
  const char *extensions;
  int drawable_width;
  int drawable_height;
  int red_bits;
  int green_bits;
  int blue_bits;
  int alpha_bits;
  int depth_bits;
  int stencil_bits;
} nxcompat_graphics_options;

typedef struct nxcompat_graphics {
  uint32_t api_version;
  size_t struct_size;
  uint64_t capabilities;
  nxcompat_gpu_family gpu_family;
  int gles_major;
  int gles_minor;
  int drawable_width;
  int drawable_height;
  int red_bits;
  int green_bits;
  int blue_bits;
  int alpha_bits;
  int depth_bits;
  int stencil_bits;
  char video_driver[NXCOMPAT_NAME_MAX];
  char vendor[NXCOMPAT_DETAIL_MAX];
  char renderer[NXCOMPAT_DETAIL_MAX];
  char version[NXCOMPAT_DETAIL_MAX];
  char shading_language_version[NXCOMPAT_DETAIL_MAX];
} nxcompat_graphics;

/* Unlike nxcompat_graphics above, receipts are authoritative inputs to the
 * finite registry.  A bridge must derive their proof flags from resources it
 * actually opened; diagnostic/free-form reports never satisfy requirements. */
enum nxcompat_graphics_proof_flag {
  NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED = 1u << 0,
  NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT = 1u << 1,
  NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL = 1u << 2,
  NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT = 1u << 3,
  NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT = 1u << 4,
  NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED = 1u << 5,
  NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE = 1u << 6
};

typedef struct nxcompat_graphics_receipt {
  uint32_t api_version;
  size_t struct_size;
  uint32_t proof_flags;
  nxcompat_source source;
  uint64_t generation;
  int window_width;
  int window_height;
  int drawable_width;
  int drawable_height;
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
  int egl_config_id;
  int egl_red_bits;
  int egl_green_bits;
  int egl_blue_bits;
  int egl_alpha_bits;
  int egl_depth_bits;
  int egl_stencil_bits;
  int egl_renderable_type;
  int egl_surface_type;
  char video_backend[NXCOMPAT_NAME_MAX];
  char gl_vendor[NXCOMPAT_DETAIL_MAX];
  char gl_renderer[NXCOMPAT_DETAIL_MAX];
  char gl_version[NXCOMPAT_DETAIL_MAX];
  char glsl_version[NXCOMPAT_DETAIL_MAX];
  char gl_extensions[NXCOMPAT_RECEIPT_EXTENSIONS_MAX];
  char egl_vendor[NXCOMPAT_DETAIL_MAX];
  char egl_version[NXCOMPAT_DETAIL_MAX];
  char egl_client_apis[NXCOMPAT_DETAIL_MAX];
} nxcompat_graphics_receipt;

enum nxcompat_audio_proof_flag {
  NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED = 1u << 0,
  NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED = 1u << 1,
  NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED = 1u << 2
};

typedef enum nxcompat_audio_lifetime {
  NXCOMPAT_AUDIO_OPENED_THEN_CLOSED = 0,
  NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED = 1
} nxcompat_audio_lifetime;

typedef struct nxcompat_audio_receipt {
  uint32_t api_version;
  size_t struct_size;
  uint32_t proof_flags;
  nxcompat_source source;
  uint64_t generation;
  nxcompat_audio_lifetime lifetime;
  int frequency;
  uint32_t format;
  unsigned channels;
  unsigned samples;
  int device_id_was_nonzero;
  char backend[NXCOMPAT_NAME_MAX];
} nxcompat_audio_receipt;

enum nxcompat_input_proof_flag {
  NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE = 1u << 0,
  NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE = 1u << 1,
  NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE = 1u << 2,
  NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE = 1u << 3,
  NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE = 1u << 4,
  NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED = 1u << 5
};

typedef enum nxcompat_input_mapping_source {
  NXCOMPAT_INPUT_MAPPING_NONE = 0,
  NXCOMPAT_INPUT_MAPPING_PORTMASTER_ENV = 1,
  NXCOMPAT_INPUT_MAPPING_DATABASE = 2,
  NXCOMPAT_INPUT_MAPPING_RUNTIME = 3,
  NXCOMPAT_INPUT_MAPPING_ENGINE = 4
} nxcompat_input_mapping_source;

typedef enum nxcompat_input_change {
  NXCOMPAT_INPUT_CHANGE_NONE = 0,
  NXCOMPAT_INPUT_CHANGE_ADD = 1,
  NXCOMPAT_INPUT_CHANGE_REMOVE = 2,
  NXCOMPAT_INPUT_CHANGE_REMAP = 3,
  NXCOMPAT_INPUT_CHANGE_RESCAN = 4
} nxcompat_input_change;

typedef struct nxcompat_input_receipt {
  uint32_t api_version;
  size_t struct_size;
  uint32_t proof_flags;
  nxcompat_source source;
  uint64_t topology_generation;
  unsigned connected_count;
  nxcompat_input_mapping_source mapping_source;
  uint64_t hotplug_event_count;
  uint64_t rescan_count;
  nxcompat_input_change last_change;
} nxcompat_input_receipt;

typedef struct nxcompat_runtime_report {
  uint32_t api_version;
  size_t struct_size;
  nxcompat_phase phase;
  size_t evidence_count;
  nxcompat_capability_evidence
      evidence[NXCOMPAT_CAPABILITY_COUNT];
  nxcompat_requirement_report requirements;
  int has_graphics;
  int has_audio;
  int has_input;
  nxcompat_graphics_receipt graphics;
  nxcompat_audio_receipt audio;
  nxcompat_input_receipt input;
} nxcompat_runtime_report;

/* Capability discovery does not initialize SDL, EGL, GL, audio or input. */
int nxcompat_probe(const nxcompat_probe_options *options, nxcompat_host *host);

/* A plan is inspectable before it changes the process environment. */
int nxcompat_plan_environment(const nxcompat_host *host,
                              const nxcompat_plan_options *options,
                              nxcompat_plan *plan);
int nxcompat_apply_environment(nxcompat_plan *plan);
nxcompat_result_code nxcompat_plan_environment_v2(
    const nxcompat_host *host, const nxcompat_plan_options *options,
    nxcompat_plan_v2 *plan);
nxcompat_result_code nxcompat_apply_environment_v2(nxcompat_plan_v2 *plan);

/* Emit the two bounded lines that a loader can draw over its own startup logo.
 * The callback is deliberately UI-agnostic; stderr, SDL and engine text all
 * remain valid frontends. */
int nxcompat_emit_startup_status(const nxcompat_host *host,
                                 const nxcompat_plan *plan,
                                 nxcompat_status_callback callback,
                                 void *userdata);

/* Preserve firmware/PortMaster environment on the first real open. Only an
 * actual failure permits one retry with the inherited hints removed. No
 * backend name is selected by nxcompat. */
int nxcompat_negotiate_backend(const nxcompat_backend_options *options,
                               nxcompat_backend_result *result);
const char *nxcompat_backend_state_name(nxcompat_backend_state value);
nxcompat_result_code
nxcompat_negotiate_backend_v2(const nxcompat_backend_options_v2 *options,
                              nxcompat_backend_result_v2 *result);

/* Capture facts only after the real GL context/drawable exists. This never
 * changes sampler state, texture formats, context version or present owner. */
int nxcompat_capture_graphics(const nxcompat_graphics_options *options,
                              nxcompat_graphics *graphics);
const char *nxcompat_gpu_family_name(nxcompat_gpu_family value);
int nxcompat_format_graphics_line(const nxcompat_graphics *graphics,
                                  char *output, size_t output_size);

/* Finite capability registry. The returned definitions are immutable and
 * follow capabilities-v1.json field-for-field in numeric order. Registries are
 * caller-owned and intentionally contain no process-global mutable state. */
size_t nxcompat_capability_definition_count(void);
const nxcompat_capability_definition *
nxcompat_capability_definition_at(size_t index);
const nxcompat_capability_definition *
nxcompat_capability_definition_by_id(nxcompat_capability_id capability_id);
const nxcompat_capability_definition *
nxcompat_capability_definition_by_name(const char *name);

nxcompat_result_code nxcompat_registry_create(nxcompat_registry **registry);
void nxcompat_registry_destroy(nxcompat_registry *registry);
nxcompat_result_code nxcompat_registry_seed_host(nxcompat_registry *registry,
                                                 const nxcompat_host *host);
nxcompat_result_code nxcompat_registry_get(
    const nxcompat_registry *registry, nxcompat_capability_id capability_id,
    nxcompat_capability_evidence *evidence);

/* NXCOMPAT_REQUIRED_CAPABILITIES is a newline-separated list generated from
 * nxport.json. Unknown, duplicated or malformed entries fail closed. Every
 * `_ex` entry point writes a finite reason on every return when reason != NULL;
 * wrappers preserve the compact signatures. */
nxcompat_result_code nxcompat_requirements_parse(
    const char *newline_separated, nxcompat_requirements *requirements);
nxcompat_result_code nxcompat_requirements_parse_ex(
    const char *newline_separated, nxcompat_requirements *requirements,
    nxcompat_reason_code *reason);
nxcompat_result_code
nxcompat_requirements_parse_runtime(nxcompat_requirements *requirements);
nxcompat_result_code nxcompat_requirements_parse_runtime_ex(
    nxcompat_requirements *requirements, nxcompat_reason_code *reason);
nxcompat_result_code nxcompat_requirements_evaluate(
    const nxcompat_registry *registry,
    const nxcompat_requirements *requirements, nxcompat_phase phase,
    nxcompat_requirement_report *report);

/* Publication is transactional: a malformed or stale receipt leaves every
 * registry entry and previously accepted receipt byte-identical. */
nxcompat_result_code nxcompat_registry_publish_graphics(
    nxcompat_registry *registry, const nxcompat_graphics_receipt *receipt);
nxcompat_result_code nxcompat_registry_publish_graphics_ex(
    nxcompat_registry *registry, const nxcompat_graphics_receipt *receipt,
    nxcompat_reason_code *reason);
nxcompat_result_code nxcompat_registry_publish_audio(
    nxcompat_registry *registry, const nxcompat_audio_receipt *receipt);
nxcompat_result_code nxcompat_registry_publish_audio_ex(
    nxcompat_registry *registry, const nxcompat_audio_receipt *receipt,
    nxcompat_reason_code *reason);
nxcompat_result_code nxcompat_registry_publish_input(
    nxcompat_registry *registry, const nxcompat_input_receipt *receipt);
nxcompat_result_code nxcompat_registry_publish_input_ex(
    nxcompat_registry *registry, const nxcompat_input_receipt *receipt,
    nxcompat_reason_code *reason);
/* runtime_report sanitizes public labels and omits raw GL extension text;
 * controller identities and paths are absent from the type by construction. */
nxcompat_result_code nxcompat_registry_runtime_report(
    const nxcompat_registry *registry,
    const nxcompat_requirements *requirements, nxcompat_phase phase,
    nxcompat_runtime_report *report);

const char *nxcompat_evidence_state_name(nxcompat_evidence_state value);
const char *nxcompat_phase_name(nxcompat_phase value);
const char *nxcompat_source_name(nxcompat_source value);
const char *nxcompat_capability_role_name(nxcompat_capability_role value);
const char *nxcompat_requirement_state_name(nxcompat_requirement_state value);
const char *nxcompat_audio_lifetime_name(nxcompat_audio_lifetime value);
const char *
nxcompat_input_mapping_source_name(nxcompat_input_mapping_source value);
const char *nxcompat_input_change_name(nxcompat_input_change value);

const char *nxcompat_arch_name(nxcompat_arch value);
const char *nxcompat_reason_name(nxcompat_reason_code value);
const char *nxcompat_observation_name(nxcompat_observation_id value);
const nxcompat_observation *
nxcompat_probe_find_observation(const nxcompat_probe_result *result,
                                nxcompat_observation_id id);
const char *nxcompat_memory_class_name(nxcompat_memory_class value);
const char *nxcompat_filesystem_class_name(nxcompat_filesystem_class value);
const char *nxcompat_action_name(nxcompat_action_id value);
const char *nxcompat_action_state_name(nxcompat_action_state value);

/* Human-readable strings are bounded, single-line and contain no hostname,
 * IP address, credential or user path outside the configured game directory. */
int nxcompat_format_device_line(const nxcompat_host *host, char *output,
                                size_t output_size);
int nxcompat_format_fix_line(const nxcompat_plan *plan, char *output,
                             size_t output_size);

/* JSON is intended for the PortMaster/NXExtract status UI and diagnostics. */
int nxcompat_format_json(const nxcompat_host *host, const nxcompat_plan *plan,
                         char *output, size_t output_size);

/* Runtime JSON contains only finite IDs/enums, numeric receipt facts and
 * explicitly sanitized public labels. Paths, sockets, controller identities,
 * environment values and free-form diagnostic text are never serialized. */
int nxcompat_format_runtime_json(const nxcompat_host *host,
                                 const nxcompat_plan_v2 *plan,
                                 const nxcompat_runtime_report *report,
                                 char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif
