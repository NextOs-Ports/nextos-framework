/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXANDROID_H
#define NXANDROID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXANDROID_API_VERSION 1u
#define NXANDROID_VERSION "0.1.0"
#define NXANDROID_NO_MODULE ((size_t)-1)
#define NXANDROID_MODULE_NAME_MAX 128u
#define NXANDROID_SYMBOL_NAME_MAX 256u
#define NXANDROID_CONTRACT_ID_MAX 256u
#define NXANDROID_STUB_SEMANTICS_MAX 1024u
#define NXANDROID_MAX_MODULES 64u
#define NXANDROID_MAX_STEPS 4096u
#define NXANDROID_MAX_CATALOG_ENTRIES 4096u
#define NXANDROID_MAX_IMPORT_REQUESTS 4096u

typedef enum nxandroid_result {
  NXANDROID_OK = 0,
  NXANDROID_EINVAL = -1,
  NXANDROID_ENOMEM = -2,
  NXANDROID_EPROFILE = -3,
  NXANDROID_ESTATE = -4,
  NXANDROID_EREENTRANT = -5,
  NXANDROID_ECALLBACK = -6,
  NXANDROID_EROLLBACK = -7,
  NXANDROID_ECATALOG = -8,
  NXANDROID_EUNRESOLVED = -9,
  NXANDROID_ECONTRACT = -10
} nxandroid_result;

const char *nxandroid_result_string(nxandroid_result result);

typedef enum nxandroid_jni_policy {
  NXANDROID_JNI_NONE = 0,
  NXANDROID_JNI_REQUIRED
} nxandroid_jni_policy;

typedef struct nxandroid_module_spec {
  const char *name;
  nxandroid_jni_policy jni_policy;
} nxandroid_module_spec;

/* These phases are lifecycle boundaries, not implementations. In particular,
 * the core contains no JavaVM, JNIEnv, Activity, Surface, GL or input object. */
typedef enum nxandroid_phase {
  NXANDROID_PHASE_MODULE_INITIALIZED = 1,
  NXANDROID_PHASE_MODULE_JNI,
  NXANDROID_PHASE_ACTIVITY_CREATE,
  NXANDROID_PHASE_GRAPHICS_REQUEST,
  NXANDROID_PHASE_SURFACE_UP,
  NXANDROID_PHASE_SURFACE_CHANGED,
  NXANDROID_PHASE_GL_READY,
  NXANDROID_PHASE_RESUME,
  NXANDROID_PHASE_FOCUS_GAIN,
  NXANDROID_PHASE_ENTRY,
  NXANDROID_PHASE_OBJECTS_READY,
  NXANDROID_PHASE_INPUT_ENABLE,
  /* Adapter-owned blocking runtime call. It returns only when that driven
   * engine loop has ended and shutdown may begin. */
  NXANDROID_PHASE_RUN_LOOP,
  NXANDROID_PHASE_INPUT_DISABLE,
  NXANDROID_PHASE_FOCUS_LOSS,
  NXANDROID_PHASE_PAUSE,
  NXANDROID_PHASE_SURFACE_DOWN,
  NXANDROID_PHASE_SAVE,
  NXANDROID_PHASE_NATIVE_SHUTDOWN,
  NXANDROID_PHASE_TERMINAL,
  /* Narrow blocking owner (for example SDL_main) that bundles entry, objects,
   * graphics, input, loop and terminal internally. Must be the final step. */
  NXANDROID_PHASE_RUNTIME_DELEGATED
} nxandroid_phase;

const char *nxandroid_phase_name(nxandroid_phase phase);

typedef enum nxandroid_terminal_policy {
  NXANDROID_TERMINAL_NONE = 0,
  /* The adapter callback returns normally to its launcher/caller. */
  NXANDROID_TERMINAL_RETURN,
  /* The adapter owns any proven terminal action. The core still only calls the
   * callback and returns; it never invokes exit(), _exit() or a signal. */
  NXANDROID_TERMINAL_ADAPTER
} nxandroid_terminal_policy;

typedef struct nxandroid_step {
  nxandroid_phase phase;
  /* Required only for MODULE_INITIALIZED/MODULE_JNI. */
  size_t module_index;
  /* Non-zero adapter epoch. GRAPHICS/SURFACE/GL/ENTRY/INPUT use the active
   * surface generation. With the explicit pre-Surface-callback profile opt-in,
   * ENTRY/OBJECTS instead use that same generation while it is still pending.
   * RESUME/PAUSE and FOCUS_GAIN/LOSS form independent same-id pairs because
   * Android may deliver them without an active Surface. Module/final phases
   * use zero. */
  uint32_t cycle_id;
  /* Stable adapter-owned semantic identifier; mandatory for every step. */
  const char *contract_id;
  /* Optional. The core calls rollback only when this is non-empty. It never
   * invents teardown for a phase that the adapter did not mark rollbackable. */
  const char *rollback_contract_id;
  /* NONE except on TERMINAL. */
  nxandroid_terminal_policy terminal_policy;
  /* Optional explicit compensation group. A successful later step with the
   * matching closes_rollback_group consumes every open rollback in the group,
   * preventing teardown from being called twice after forward shutdown. */
  uint32_t rollback_group;
  uint32_t closes_rollback_group;
} nxandroid_step;

enum nxandroid_profile_flag {
  /* Required for NXANDROID_TERMINAL_ADAPTER. This is an adapter opt-in, not
   * authority for the core to terminate a process. */
  NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL = 1u << 0,
  /* Allows one final RUNTIME_DELEGATED callback. The core waits for that
   * callback and makes no claims about lifecycle bundled inside it. */
  NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME = 1u << 1,
  /* Source-proven adapter exception for engines whose host GL/context setup
   * and native entry/object creation precede the guest Surface callback.
   * ENTRY still requires GRAPHICS_REQUEST plus GL_READY for the same pending
   * cycle. INPUT_ENABLE and RUN_LOOP still require an active Surface. */
  NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK = 1u << 2
};

typedef struct nxandroid_profile {
  uint32_t api_version;
  size_t struct_size;
  const nxandroid_module_spec *modules;
  size_t module_count;
  const nxandroid_step *steps;
  size_t step_count;
  uint32_t flags;
} nxandroid_profile;

/* Adapter callbacks return zero on success and a stable adapter-specific
 * non-zero status on failure. The step pointers remain valid until destroy.
 * The adapter must keep both callback code and userdata alive and callable
 * until context destruction has completed, including every rollback. */
typedef int (*nxandroid_invoke_callback)(void *userdata,
                                         const nxandroid_step *step);
typedef int (*nxandroid_rollback_callback)(void *userdata,
                                           const nxandroid_step *step);

typedef struct nxandroid_ops {
  uint32_t api_version;
  size_t struct_size;
  nxandroid_invoke_callback invoke;
  nxandroid_rollback_callback rollback;
  void *userdata;
} nxandroid_ops;

typedef enum nxandroid_context_state {
  NXANDROID_CONTEXT_READY = 0,
  NXANDROID_CONTEXT_RUNNING,
  NXANDROID_CONTEXT_COMPLETE,
  NXANDROID_CONTEXT_FAILED,
  NXANDROID_CONTEXT_ABORTED
} nxandroid_context_state;

typedef struct nxandroid_context nxandroid_context;

nxandroid_result nxandroid_profile_validate(const nxandroid_profile *profile,
                                             size_t *bad_step);
nxandroid_result nxandroid_context_create(const nxandroid_profile *profile,
                                           const nxandroid_ops *ops,
                                           nxandroid_context **output);
nxandroid_result nxandroid_context_step(nxandroid_context *context);
nxandroid_result nxandroid_context_run(nxandroid_context *context);
nxandroid_result nxandroid_context_abort(nxandroid_context *context);
/* A result-bearing destroy makes a same-context destroy attempt observable
 * during callbacks. On a real destroy, *context is always set to NULL. */
nxandroid_result nxandroid_context_destroy(nxandroid_context **context);

/* Getters are also same-context operations: from inside invoke/rollback they
 * return a sentinel and make the outer callback fail closed. The callback
 * already receives the complete immutable step and never needs a getter. */
nxandroid_context_state nxandroid_context_get_state(nxandroid_context *context);
size_t nxandroid_context_get_next_step(nxandroid_context *context);
int nxandroid_context_get_adapter_status(nxandroid_context *context);
int nxandroid_context_get_rollback_status(nxandroid_context *context);

typedef enum nxandroid_abi {
  NXANDROID_ABI_ARMV7_BIONIC = 1,
  NXANDROID_ABI_AARCH64_BIONIC,
  NXANDROID_ABI_X86_BIONIC,
  NXANDROID_ABI_X86_64_BIONIC
} nxandroid_abi;

typedef enum nxandroid_import_domain {
  NXANDROID_IMPORT_BIONIC = 1,
  NXANDROID_IMPORT_JNI,
  NXANDROID_IMPORT_NDK
} nxandroid_import_domain;

typedef enum nxandroid_symbol_kind {
  NXANDROID_SYMBOL_FUNCTION = 1,
  NXANDROID_SYMBOL_DATA
} nxandroid_symbol_kind;

typedef enum nxandroid_import_criticality {
  NXANDROID_IMPORT_OPTIONAL = 1,
  NXANDROID_IMPORT_CRITICAL
} nxandroid_import_criticality;

typedef enum nxandroid_import_binding {
  NXANDROID_BIND_STRONG = 1,
  NXANDROID_BIND_WEAK
} nxandroid_import_binding;

typedef enum nxandroid_provider_kind {
  NXANDROID_PROVIDER_IMPLEMENTATION = 1,
  NXANDROID_PROVIDER_STUB
} nxandroid_provider_kind;

/* A catalog entry is an explicit address plus an ABI contract. There is no
 * lookup callback and therefore no dlsym or generic fallback path. */
typedef struct nxandroid_catalog_entry {
  const char *name;
  nxandroid_abi abi;
  nxandroid_import_domain domain;
  nxandroid_symbol_kind symbol_kind;
  nxandroid_import_criticality criticality;
  const char *contract_id;
  uintptr_t address;
  nxandroid_provider_kind provider_kind;
  /* Mandatory and non-empty for a STUB; must be NULL for an implementation. */
  const char *stub_semantics;
} nxandroid_catalog_entry;

typedef struct nxandroid_import_request {
  const char *name;
  nxandroid_abi abi;
  nxandroid_import_domain domain;
  nxandroid_symbol_kind symbol_kind;
  nxandroid_import_criticality criticality;
  nxandroid_import_binding binding;
  const char *contract_id;
  /* A matching explicit stub is rejected unless this is non-zero. */
  int allow_stub;
} nxandroid_import_request;

typedef struct nxandroid_import_binding_result {
  int resolved;
  uintptr_t address;
  nxandroid_provider_kind provider_kind;
  /* These strings are borrowed from the catalog and remain valid until its
   * destruction. Provider-owned code/data named by address must outlive every
   * binding consumer and the catalog itself. Destruction order is consumers,
   * then catalog, then providers. */
  const char *contract_id;
  const char *stub_semantics;
} nxandroid_import_binding_result;

typedef struct nxandroid_import_catalog nxandroid_import_catalog;

nxandroid_result
nxandroid_import_catalog_create(const nxandroid_catalog_entry *entries,
                                size_t entry_count,
                                nxandroid_import_catalog **output);
void nxandroid_import_catalog_destroy(nxandroid_import_catalog **catalog);
/* Successful string pointers are catalog-owned and remain valid until catalog
 * destruction. Atomic with respect to output: on any error, bindings are untouched.
 * Unknown non-critical weak imports resolve to zero. Unknown critical weak
 * imports fail exactly like critical strong imports. */
nxandroid_result nxandroid_imports_resolve(
    const nxandroid_import_catalog *catalog,
    const nxandroid_import_request *requests, size_t request_count,
    nxandroid_import_binding_result *bindings, size_t *bad_request);

#ifdef __cplusplus
}
#endif

#endif
