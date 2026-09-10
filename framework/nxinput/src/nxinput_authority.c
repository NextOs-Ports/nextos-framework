/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_authority -- see include/nxinput_authority.h.
 *
 * SDL-free by construction: every effect goes through the injected runtime
 * vtable. This is the production decision path; the SDL backing of the
 * vtable lives in nxinput_authority_sdl.c. */
#include "nxinput_authority.h"

#include <stdlib.h>
#include <string.h>

/* The readback closure: apply the candidate line to the real runtime, then
 * ask the runtime what it EFFECTIVELY holds for this GUID. An empty line is
 * the authority-4 query -- nothing is applied, the runtime is only asked
 * what its built-in database would use. */
struct readback_ctx {
  const nxinput_authority_runtime *runtime;
  const char *guid;
  uint32_t applies;
};

static int authority_readback(void *userdata, const char *line, char *out,
                              size_t cap) {
  struct readback_ctx *ctx = (struct readback_ctx *)userdata;
  if (line == NULL || out == NULL || cap == 0u) {
    return -1;
  }
  out[0] = '\0';
  if (line[0] != '\0') {
    if (ctx->runtime->apply_mapping_fn(ctx->runtime->userdata, line) != 0) {
      return -1;
    }
    ctx->applies++;
  }
  if (ctx->runtime->mapping_for_guid_fn(ctx->runtime->userdata, ctx->guid,
                                        out, cap) != 0) {
    return -1;
  }
  return 0;
}

static int runtime_valid(const nxinput_authority_runtime *runtime) {
  return runtime != NULL &&
         runtime->api_version == NXINPUT_AUTHORITY_API_VERSION &&
         runtime->struct_size == sizeof(*runtime) &&
         runtime->getenv_fn != NULL && runtime->read_text_fn != NULL &&
         runtime->device_guid_fn != NULL && runtime->device_caps_fn != NULL &&
         runtime->apply_mapping_fn != NULL &&
         runtime->mapping_for_guid_fn != NULL;
}

int nxinput_authority_init(nxinput_authority *authority,
                           const nxinput_authority_runtime *runtime) {
  if (authority == NULL || !runtime_valid(runtime)) {
    return -1;
  }
  memset(authority, 0, sizeof(*authority));
  authority->api_version = NXINPUT_AUTHORITY_API_VERSION;
  authority->struct_size = sizeof(*authority);
  authority->runtime = *runtime;
  return 0;
}

const nxinput_authority_entry *nxinput_authority_find(
    const nxinput_authority *authority, int32_t instance_id) {
  unsigned int i;
  if (authority == NULL) {
    return NULL;
  }
  for (i = 0u; i < NXINPUT_AUTHORITY_MAX_DEVICES; i++) {
    if (authority->entries[i].in_use &&
        authority->entries[i].instance_id == instance_id) {
      return &authority->entries[i];
    }
  }
  return NULL;
}

void nxinput_authority_forget(nxinput_authority *authority,
                              int32_t instance_id) {
  unsigned int i;
  if (authority == NULL) {
    return;
  }
  for (i = 0u; i < NXINPUT_AUTHORITY_MAX_DEVICES; i++) {
    if (authority->entries[i].in_use &&
        authority->entries[i].instance_id == instance_id) {
      /* Erase, never keep: the next pad on this instance must not be able to
       * read one byte of the previous device's decision. */
      memset(&authority->entries[i], 0, sizeof(authority->entries[i]));
      if (authority->admitted > 0u) {
        authority->admitted--;
      }
      authority->forgotten++;
    }
  }
}

/* Read one text source into a fresh buffer. Returns NULL when the source is
 * absent, unreadable or oversized -- the ladder then simply yields. */
static char *load_env_file(const nxinput_authority_runtime *runtime,
                           const char *env_name) {
  const char *path = runtime->getenv_fn(runtime->userdata, env_name);
  char *buffer;
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }
  buffer = (char *)malloc(NXINPUT_AUTHORITY_SOURCE_MAX);
  if (buffer == NULL) {
    return NULL;
  }
  buffer[0] = '\0';
  if (runtime->read_text_fn(runtime->userdata, path, buffer,
                            NXINPUT_AUTHORITY_SOURCE_MAX) != 0) {
    free(buffer);
    return NULL;
  }
  return buffer;
}

int nxinput_authority_admit(nxinput_authority *authority, int device_index,
                            int32_t instance_id,
                            nxinput_sovereign_decision *decision) {
  nxinput_sovereign_request request;
  nxinput_sovereign_decision local;
  struct readback_ctx ctx;
  nxinput_authority_entry *slot = NULL;
  const char *env_mapping;
  char *database = NULL;
  char *bundle = NULL;
  unsigned int i;
  int buttons = 0, axes = 0, hats = 0;
  int admitted;

  if (decision != NULL) {
    (void)nxinput_sovereign_decision_init(decision);
  }
  if (authority == NULL ||
      authority->api_version != NXINPUT_AUTHORITY_API_VERSION ||
      !runtime_valid(&authority->runtime)) {
    return -1;
  }
  (void)nxinput_sovereign_decision_init(&local);

  /* A reconnection never inherits: whatever this instance held is gone
   * BEFORE the pad is measured again. */
  nxinput_authority_forget(authority, instance_id);

  (void)nxinput_sovereign_request_init(&request);
  if (authority->runtime.device_guid_fn(authority->runtime.userdata,
                                        device_index, request.guid,
                                        sizeof request.guid) != 0) {
    authority->refused++;
    return -1;
  }
  /* Capabilities are MEASURED from the pad that is connected right now. */
  if (authority->runtime.device_caps_fn(authority->runtime.userdata,
                                        device_index, &buttons, &axes,
                                        &hats) != 0) {
    authority->refused++;
    return -1;
  }
  request.caps.buttons = buttons;
  request.caps.axes = axes;
  request.caps.hats = hats;
  request.runtime_has_builtin = authority->runtime.runtime_has_builtin;
  request.consumer_accepts_raw = authority->runtime.consumer_accepts_raw;

  env_mapping = authority->runtime.getenv_fn(authority->runtime.userdata,
                                             NXINPUT_AUTHORITY_ENV_MAPPING);
  database = load_env_file(&authority->runtime,
                           NXINPUT_AUTHORITY_ENV_DATABASE);
  bundle = load_env_file(&authority->runtime, NXINPUT_AUTHORITY_ENV_BUNDLE);

  ctx.runtime = &authority->runtime;
  ctx.guid = request.guid;
  ctx.applies = 0u;

  authority->resolutions++;
  (void)nxinput_sovereign_resolve(&request, env_mapping, database, bundle,
                                  authority_readback, &ctx, &local);
  free(database);
  free(bundle);

  admitted = (local.source != NXINPUT_SOVEREIGN_FAIL_EXPLICIT);
  if (decision != NULL) {
    *decision = local;
  }
  if (!admitted) {
    authority->refused++;
    return -1;
  }

  for (i = 0u; i < NXINPUT_AUTHORITY_MAX_DEVICES; i++) {
    if (!authority->entries[i].in_use) {
      slot = &authority->entries[i];
      break;
    }
  }
  if (slot == NULL) {
    authority->refused++;
    return -1;
  }
  memset(slot, 0, sizeof(*slot));
  slot->in_use = 1;
  slot->instance_id = instance_id;
  memcpy(slot->guid, request.guid, sizeof slot->guid);
  slot->buttons = buttons;
  slot->axes = axes;
  slot->hats = hats;
  slot->source = local.source;
  memcpy(slot->line, local.line, sizeof slot->line);
  authority->generation++;
  slot->resolved_generation = authority->generation;
  authority->admitted++;
  return 0;
}
