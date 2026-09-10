/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-GRAPHICS-03 implementation. See nxgl_egl_binding.h for the contract. */

#include "nxgl_egl_binding.h"

#include <stdio.h>
#include <string.h>

#define NXGL_EGL_PROBE_SYMBOL "eglGetCurrentContext"

static int nxgl_egl_name_valid(const char *name) {
  size_t index;
  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  for (index = 0; index < 128u; ++index) {
    const char character = name[index];
    if (character == '\0') {
      return 1;
    }
    if (!((character >= 'A' && character <= 'Z') ||
          (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_')) {
      return 0;
    }
  }
  return 0;
}

static int nxgl_egl_is_egl_name(const char *name) {
  return name != NULL && strncmp(name, "egl", 3) == 0;
}

static int nxgl_egl_request_valid(const nxgl_egl_binding_request *request) {
  size_t index;
  size_t other;
  int saw_probe = 0;
  if (request == NULL || request->struct_size != sizeof(*request) ||
      request->api_version != NXGL_EGL_BINDING_API_VERSION) {
    return 0;
  }
  if (request->declared_imports == NULL || request->declared_import_count == 0 ||
      request->declared_import_count > NXGL_EGL_BINDING_MAX_IMPORTS) {
    return 0;
  }
  if (request->guest.struct_size != sizeof(request->guest)) {
    return 0;
  }
  for (index = 0; index < request->declared_import_count; ++index) {
    const char *name = request->declared_imports[index];
    if (!nxgl_egl_name_valid(name) || !nxgl_egl_is_egl_name(name)) {
      return 0;
    }
    for (other = 0; other < index; ++other) {
      if (strcmp(name, request->declared_imports[other]) == 0) {
        return 0;
      }
    }
    if (strcmp(name, NXGL_EGL_PROBE_SYMBOL) == 0) {
      saw_probe = 1;
    }
  }
  /* The probe is the symbol that proves ownership of the current context. A
   * port that does not import it cannot be bound by this contract. */
  return saw_probe;
}

static int nxgl_egl_ops_valid(const nxgl_egl_dl_ops *ops) {
  return ops != NULL && ops->struct_size == sizeof(*ops) &&
         ops->resolve != NULL && ops->identify != NULL &&
         ops->open_local != NULL && ops->symbol != NULL &&
         ops->current_context != NULL && ops->promote_global != NULL &&
         ops->close != NULL;
}

static void nxgl_egl_copy(char *destination, size_t cap, const char *source) {
  size_t length;
  if (destination == NULL || cap == 0) {
    return;
  }
  if (source == NULL) {
    destination[0] = '\0';
    return;
  }
  length = strlen(source);
  if (length >= cap) {
    length = cap - 1;
  }
  memcpy(destination, source, length);
  destination[length] = '\0';
}

static nxgl_egl_binding_status nxgl_egl_fail(nxgl_egl_binding *binding,
                                             const nxgl_egl_dl_ops *ops,
                                             void *handle,
                                             nxgl_egl_binding_status status) {
  if (handle != NULL && ops != NULL && ops->close != NULL) {
    ops->close(ops->userdata, handle);
  }
  binding->handle = NULL;
  binding->promoted = 0;
  binding->entry_count = 0;
  binding->status = status;
  return status;
}

nxgl_egl_binding_status nxgl_egl_binding_resolve(
    const nxgl_egl_binding_request *request, const nxgl_egl_dl_ops *ops,
    nxgl_egl_binding *out_binding) {
  nxgl_egl_dso_identity identity;
  nxgl_egl_binding binding;
  void *probe_address;
  void *handle;
  void *candidate_probe;
  size_t index;

  if (out_binding == NULL) {
    return NXGL_EGL_BINDING_INVALID_ARGUMENT;
  }
  memset(&binding, 0, sizeof(binding));
  binding.struct_size = sizeof(binding);
  binding.api_version = NXGL_EGL_BINDING_API_VERSION;
  binding.status = NXGL_EGL_BINDING_INVALID_ARGUMENT;
  if (!nxgl_egl_request_valid(request) || !nxgl_egl_ops_valid(ops)) {
    *out_binding = binding;
    return NXGL_EGL_BINDING_INVALID_ARGUMENT;
  }
  binding.guest = request->guest;
  binding.expected_imports = request->declared_import_count;

  /* 1. Identity comes from the SDL resolver, not from a SONAME guess. */
  probe_address = ops->resolve(ops->userdata, NXGL_EGL_PROBE_SYMBOL);
  if (probe_address == NULL) {
    nxgl_egl_copy(binding.failed_symbol, sizeof(binding.failed_symbol),
                  NXGL_EGL_PROBE_SYMBOL);
    *out_binding = binding;
    return nxgl_egl_fail(out_binding, ops, NULL, NXGL_EGL_BINDING_NO_RESOLVER);
  }

  /* 2. dladdr names the concrete object that already owns the context. */
  memset(&identity, 0, sizeof(identity));
  identity.struct_size = sizeof(identity);
  if (ops->identify(ops->userdata, probe_address, &identity) == 0 ||
      identity.path[0] == '\0') {
    *out_binding = binding;
    return nxgl_egl_fail(out_binding, ops, NULL,
                         NXGL_EGL_BINDING_NO_PROVIDER_IDENTITY);
  }
  identity.path[NXGL_EGL_BINDING_PATH_MAX - 1] = '\0';
  identity.build_id[NXGL_EGL_BINDING_BUILD_ID_MAX - 1] = '\0';
  binding.provider = identity;

  /* 3. The candidate opens LOCAL first. Nothing reaches the global namespace
   *    before every proof below has passed. */
  handle = ops->open_local(ops->userdata, identity.path);
  if (handle == NULL) {
    *out_binding = binding;
    return nxgl_egl_fail(out_binding, ops, NULL, NXGL_EGL_BINDING_OPEN_FAILED);
  }

  /* 4. Every declared import must exist in THIS object. */
  for (index = 0; index < request->declared_import_count; ++index) {
    const char *name = request->declared_imports[index];
    void *address = ops->symbol(ops->userdata, handle, name);
    if (address == NULL) {
      nxgl_egl_copy(binding.failed_symbol, sizeof(binding.failed_symbol), name);
      *out_binding = binding;
      return nxgl_egl_fail(out_binding, ops, handle,
                           NXGL_EGL_BINDING_MISSING_IMPORT);
    }
    binding.entries[index].name = name;
    binding.entries[index].address = (uintptr_t)address;
    binding.resolved_imports += 1u;
    if (strcmp(name, NXGL_EGL_PROBE_SYMBOL) == 0) {
      candidate_probe = address;
      /* 5. Same provider as the resolver, not a lookalike behind the same
       *    SONAME. */
      if (candidate_probe != probe_address) {
        nxgl_egl_copy(binding.failed_symbol, sizeof(binding.failed_symbol),
                      name);
        *out_binding = binding;
        return nxgl_egl_fail(out_binding, ops, handle,
                             NXGL_EGL_BINDING_PROVIDER_MISMATCH);
      }
    }
  }
  binding.entry_count = request->declared_import_count;

  /* 6. A provider without a current context is not the owner of this frame. */
  binding.context_current = ops->current_context(ops->userdata, handle) != 0;
  if (!binding.context_current) {
    *out_binding = binding;
    return nxgl_egl_fail(out_binding, ops, handle,
                         NXGL_EGL_BINDING_NULL_CONTEXT);
  }

  /* 7. Only now is the SAME object promoted, and only if it is not already
   *    global. */
  if (identity.already_global) {
    binding.promoted = 0;
  } else if (ops->promote_global(ops->userdata, identity.path) == 0) {
    *out_binding = binding;
    return nxgl_egl_fail(out_binding, ops, handle,
                         NXGL_EGL_BINDING_PROMOTION_FAILED);
  } else {
    binding.promoted = 1;
  }

  binding.handle = handle;
  binding.status = NXGL_EGL_BINDING_OK;
  *out_binding = binding;
  return NXGL_EGL_BINDING_OK;
}

nxgl_egl_binding_status nxgl_egl_binding_check_inventory(
    const nxgl_egl_binding_request *request,
    const char *const *guest_undefined, size_t guest_undefined_count,
    char *out_offender, size_t out_offender_cap) {
  size_t index;
  size_t declared;
  if (out_offender != NULL && out_offender_cap > 0) {
    out_offender[0] = '\0';
  }
  if (!nxgl_egl_request_valid(request)) {
    return NXGL_EGL_BINDING_INVALID_ARGUMENT;
  }
  if (guest_undefined_count != 0 && guest_undefined == NULL) {
    return NXGL_EGL_BINDING_INVALID_ARGUMENT;
  }
  for (index = 0; index < guest_undefined_count; ++index) {
    const char *name = guest_undefined[index];
    int found = 0;
    if (name == NULL) {
      return NXGL_EGL_BINDING_INVALID_ARGUMENT;
    }
    if (!nxgl_egl_is_egl_name(name)) {
      continue;
    }
    for (declared = 0; declared < request->declared_import_count; ++declared) {
      if (strcmp(name, request->declared_imports[declared]) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) {
      nxgl_egl_copy(out_offender, out_offender_cap, name);
      return NXGL_EGL_BINDING_UNDECLARED_IMPORT;
    }
  }
  return NXGL_EGL_BINDING_OK;
}

const char *nxgl_egl_binding_status_name(nxgl_egl_binding_status status) {
  switch (status) {
    case NXGL_EGL_BINDING_OK: return "ok";
    case NXGL_EGL_BINDING_INVALID_ARGUMENT: return "invalid-argument";
    case NXGL_EGL_BINDING_NO_RESOLVER: return "no-resolver";
    case NXGL_EGL_BINDING_NO_PROVIDER_IDENTITY: return "no-provider-identity";
    case NXGL_EGL_BINDING_OPEN_FAILED: return "open-failed";
    case NXGL_EGL_BINDING_MISSING_IMPORT: return "missing-import";
    case NXGL_EGL_BINDING_NULL_CONTEXT: return "null-context";
    case NXGL_EGL_BINDING_PROVIDER_MISMATCH: return "provider-mismatch";
    case NXGL_EGL_BINDING_PROMOTION_FAILED: return "promotion-failed";
    case NXGL_EGL_BINDING_UNDECLARED_IMPORT: return "undeclared-import";
    default: return "unknown";
  }
}

size_t nxgl_egl_binding_receipt(const nxgl_egl_binding *binding,
                                char *buf, size_t cap) {
  char scratch[1024];
  int written;
  if (binding == NULL || binding->struct_size != sizeof(*binding)) {
    return 0;
  }
  written = snprintf(
      scratch, sizeof(scratch),
      "{\"schema\":\"org.nextos.nxgl.egl-binding\",\"schema_version\":1,"
      "\"status\":\"%s\",\"provider\":{\"path\":\"%s\",\"build_id\":\"%s\","
      "\"already_global\":%d,\"promoted\":%d},"
      "\"guest\":{\"name\":\"%s\",\"build_id\":\"%s\"},"
      "\"imports\":{\"expected\":%zu,\"resolved\":%zu},"
      "\"context_current\":%d,\"failed_symbol\":\"%s\"}",
      nxgl_egl_binding_status_name(binding->status), binding->provider.path,
      binding->provider.build_id, binding->provider.already_global,
      binding->promoted, binding->guest.name, binding->guest.build_id,
      binding->expected_imports, binding->resolved_imports,
      binding->context_current, binding->failed_symbol);
  if (written < 0) {
    return 0;
  }
  if (buf != NULL && cap > 0) {
    size_t copy = (size_t)written < cap - 1 ? (size_t)written : cap - 1;
    memcpy(buf, scratch, copy);
    buf[copy] = '\0';
  }
  return (size_t)written;
}
