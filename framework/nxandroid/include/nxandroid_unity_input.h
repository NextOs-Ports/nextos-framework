/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXANDROID_UNITY_INPUT_H
#define NXANDROID_UNITY_INPUT_H

#include "nxandroid_android_input.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Unity is not one input ABI.  This boundary records the exact runtime and
 * consumer contract selected by an adapter, consumes only the already-frozen
 * C7 event stream, and distinguishes producer acknowledgement from a consumer
 * receipt.  It never discovers a Unity class, calls IL2CPP/Mono, reads a pad,
 * invents an identity, or resolves an address/RVA. */
#define NXANDROID_UNITY_INPUT_API_VERSION 1u
#define NXANDROID_UNITY_MAX_PADS NXANDROID_ANDROID_MAX_PADS
#define NXANDROID_UNITY_TEXT_MAX 129u
#define NXANDROID_UNITY_SHA256_LENGTH 64u
#define NXANDROID_UNITY_GIT_ID_LENGTH 40u

typedef enum nxandroid_unity_result {
  NXANDROID_UNITY_OK = 0,
  NXANDROID_UNITY_EINVAL = -1,
  NXANDROID_UNITY_ESTATE = -2,
  NXANDROID_UNITY_ECALLBACK = -3,
  NXANDROID_UNITY_ENOTFOUND = -4,
  NXANDROID_UNITY_EDUPLICATE = -5,
  NXANDROID_UNITY_EUNPROVEN = -6
} nxandroid_unity_result;

typedef enum nxandroid_unity_profile_kind {
  NXANDROID_UNITY_LEGACY_INPUT = 1,
  NXANDROID_UNITY_NEW_INPUT_SYSTEM,
  NXANDROID_UNITY_REWIRED,
  NXANDROID_UNITY_INCONTROL,
  NXANDROID_UNITY_RAW_ANDROID
} nxandroid_unity_profile_kind;

typedef enum nxandroid_unity_runtime_kind {
  NXANDROID_UNITY_RUNTIME_IL2CPP = 1,
  NXANDROID_UNITY_RUNTIME_MONO
} nxandroid_unity_runtime_kind;

typedef enum nxandroid_unity_evidence_class {
  NXANDROID_UNITY_EVIDENCE_FIXTURE = 1,
  NXANDROID_UNITY_EVIDENCE_REAL_API_HOST,
  NXANDROID_UNITY_EVIDENCE_PHYSICAL,
  NXANDROID_UNITY_EVIDENCE_PENDING,
  NXANDROID_UNITY_EVIDENCE_UNPROVEN,
  NXANDROID_UNITY_EVIDENCE_NA
} nxandroid_unity_evidence_class;

typedef enum nxandroid_unity_thread_kind {
  NXANDROID_UNITY_THREAD_PLAYER = 1,
  NXANDROID_UNITY_THREAD_ANDROID_INPUT
} nxandroid_unity_thread_kind;

typedef enum nxandroid_unity_identity_policy {
  NXANDROID_UNITY_IDENTITY_PHYSICAL = 1,
  NXANDROID_UNITY_IDENTITY_PROVEN_OVERRIDE
} nxandroid_unity_identity_policy;

typedef enum nxandroid_unity_lifecycle_phase {
  NXANDROID_UNITY_PHASE_INIT_ARRAY = 1,
  NXANDROID_UNITY_PHASE_JNI_ONLOAD_MAIN,
  NXANDROID_UNITY_PHASE_JNI_ONLOAD_RUNTIME,
  NXANDROID_UNITY_PHASE_JNI_ONLOAD_UNITY,
  NXANDROID_UNITY_PHASE_PLAYER_INIT,
  NXANDROID_UNITY_PHASE_SURFACE_CREATE,
  NXANDROID_UNITY_PHASE_SURFACE_CHANGE,
  NXANDROID_UNITY_PHASE_RESUME,
  NXANDROID_UNITY_PHASE_FOCUS_GAIN,
  NXANDROID_UNITY_PHASE_FRAME_LOOP,
  NXANDROID_UNITY_PHASE_FOCUS_LOSS,
  NXANDROID_UNITY_PHASE_PAUSE,
  NXANDROID_UNITY_PHASE_SURFACE_DESTROY,
  NXANDROID_UNITY_PHASE_SHUTDOWN
} nxandroid_unity_lifecycle_phase;

typedef enum nxandroid_unity_api_stage {
  NXANDROID_UNITY_API_LOW_LEVEL_RETURN = 1,
  NXANDROID_UNITY_API_ACTION_RETURN
} nxandroid_unity_api_stage;

typedef enum nxandroid_unity_device_operation {
  NXANDROID_UNITY_DEVICE_ADD = 1,
  NXANDROID_UNITY_DEVICE_REMOVE
} nxandroid_unity_device_operation;

typedef struct nxandroid_unity_control_contract {
  int reachable;
  nxandroid_android_signal_kind signal;
  const char *consumer_control;
} nxandroid_unity_control_contract;

/* Every string is exact and bounded.  Hashes are lower-case SHA-256, source
 * IDs are full Git object IDs, and API strings are full signatures rather
 * than name/arity lookups.  An adapter with copied RVAs has no representation
 * in this API and must fail before context creation. */
typedef struct nxandroid_unity_profile {
  uint32_t api_version;
  nxandroid_unity_profile_kind kind;
  nxandroid_unity_runtime_kind runtime;
  nxandroid_unity_evidence_class evidence;
  nxandroid_unity_identity_policy identity_policy;
  nxandroid_unity_thread_kind registration_thread;
  nxandroid_unity_thread_kind producer_thread;
  nxandroid_unity_thread_kind consumer_thread;
  nxandroid_android_sink_kind producer_button_sink;
  nxandroid_android_sink_kind producer_axis_sink;
  const char *profile_id;
  const char *unity_version;
  const char *plugin_name;
  const char *plugin_version;
  const char *consumer_name;
  const char *consumer_version;
  const char *abi;
  const char *assembly_name;
  const char *register_api;
  const char *unregister_api;
  const char *enumerate_api;
  const char *producer_button_api;
  const char *producer_axis_api;
  const char *consumer_button_api;
  const char *consumer_axis_api;
  const char *consumer_action_api;
  const char *metadata_sha256;
  const char *runtime_sha256;
  const char *source_commit;
  const char *source_tree;
  const char *artifact_sha256;
  const char *license_spdx;
  const char *identity_evidence_sha256;
  const char *identity_name;
  const char *identity_vendor;
  const char *identity_product;
  nxandroid_unity_control_contract
      controls[NXANDROID_ANDROID_CONTROL_COUNT];
} nxandroid_unity_profile;

typedef struct nxandroid_unity_device_request {
  uint32_t api_version;
  nxandroid_unity_device_operation operation;
  nxandroid_unity_profile_kind profile_kind;
  uint64_t sequence;
  int32_t instance_id;
  int32_t device_id;
  uint32_t generation;
  nxandroid_android_source_kind source;
  nxandroid_unity_identity_policy identity_policy;
  const char *identity_name;
  const char *identity_vendor;
  const char *identity_product;
  const char *api_signature;
} nxandroid_unity_device_request;

typedef struct nxandroid_unity_producer_request {
  uint32_t api_version;
  nxandroid_unity_profile_kind profile_kind;
  uint64_t sequence;
  int32_t instance_id;
  int32_t device_id;
  uint32_t generation;
  nxandroid_android_control control;
  nxandroid_android_event_kind event_kind;
  nxandroid_android_signal_kind signal;
  int pressed;
  float value;
  float x;
  float y;
  const char *action;
  const char *api_signature;
} nxandroid_unity_producer_request;

typedef struct nxandroid_unity_api_ack {
  uint32_t api_version;
  uint64_t sequence;
  int returned;
  int handled;
  int32_t return_value;
} nxandroid_unity_api_ack;

typedef uint64_t (*nxandroid_unity_thread_token_fn)(void *userdata);
typedef int (*nxandroid_unity_device_fn)(
    void *userdata, const nxandroid_unity_device_request *request,
    nxandroid_unity_api_ack *ack);
typedef int (*nxandroid_unity_producer_fn)(
    void *userdata, const nxandroid_unity_producer_request *request,
    nxandroid_unity_api_ack *ack);

typedef struct nxandroid_unity_ops {
  nxandroid_unity_thread_token_fn current_thread_token;
  nxandroid_unity_device_fn device;
  nxandroid_unity_producer_fn producer;
  void *userdata;
} nxandroid_unity_ops;

typedef struct nxandroid_unity_consumer_return {
  uint32_t api_version;
  nxandroid_unity_api_stage stage;
  uint64_t event_sequence;
  int32_t instance_id;
  uint32_t generation;
  nxandroid_android_control control;
  nxandroid_android_signal_kind signal;
  int api_returned;
  int handled;
  int pressed;
  float value;
  float x;
  float y;
  const char *api_signature;
} nxandroid_unity_consumer_return;

typedef struct nxandroid_unity_pending_state {
  uint64_t sequence;
  uint8_t valid;
  uint8_t low_level_returned;
  uint8_t action_returned;
  uint8_t signal;
  int pressed;
  float value;
  float x;
  float y;
} nxandroid_unity_pending_state;

typedef struct nxandroid_unity_pad_state {
  int connected;
  int32_t instance_id;
  int32_t device_id;
  uint32_t generation;
  nxandroid_unity_pending_state
      pending[NXANDROID_ANDROID_CONTROL_COUNT];
} nxandroid_unity_pad_state;

/* Fixed public storage; initialize only through context_init. */
typedef struct nxandroid_unity_context {
  uint32_t api_version;
  int initialized;
  int failed;
  int live;
  int focused;
  int resumed;
  int surface;
  int shutdown;
  unsigned int initial_phase_index;
  uint64_t lifecycle_thread_token;
  uint64_t input_thread_token;
  uint64_t active_cycle;
  const nxandroid_unity_profile *profile;
  nxandroid_unity_ops ops;
  nxandroid_unity_pad_state pads[NXANDROID_UNITY_MAX_PADS];
  uint64_t device_calls;
  uint64_t producer_returns;
  uint64_t low_level_returns;
  uint64_t action_returns;
  uint64_t complete_receipts;
  uint64_t invalidated_pending;
  uint64_t lifecycle_events;
} nxandroid_unity_context;

const char *nxandroid_unity_result_string(nxandroid_unity_result result);
const char *nxandroid_unity_profile_name(nxandroid_unity_profile_kind kind);

nxandroid_unity_result nxandroid_unity_profile_validate(
    const nxandroid_unity_profile *profile, char *error, size_t error_size);

nxandroid_unity_result nxandroid_unity_context_init(
    nxandroid_unity_context *context,
    const nxandroid_unity_profile *profile, const nxandroid_unity_ops *ops,
    char *error, size_t error_size);
void nxandroid_unity_context_reset(nxandroid_unity_context *context);

nxandroid_unity_result nxandroid_unity_lifecycle(
    nxandroid_unity_context *context, nxandroid_unity_lifecycle_phase phase,
    uint64_t cycle_id);

/* Install directly as nxandroid_android_profile.event.  The callback returns
 * only after the declared producer API callback has returned.  That is still
 * not a consumer receipt: consumer_return must separately observe the real
 * low-level and action API returns. */
int nxandroid_unity_android_event(void *userdata,
                                  const nxandroid_android_event *event,
                                  nxandroid_android_ack *ack);

nxandroid_unity_result nxandroid_unity_note_consumer_return(
    nxandroid_unity_context *context,
    const nxandroid_unity_consumer_return *returned);

nxandroid_unity_result nxandroid_unity_receipt(
    const nxandroid_unity_context *context, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif
