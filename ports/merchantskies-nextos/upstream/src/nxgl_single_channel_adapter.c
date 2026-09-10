/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nxgl_single_channel_adapter.h"

#include <dlfcn.h>
#include <limits.h>
#include <string.h>

#define NXGL_SCA_GL_TEXTURE_2D 0x0DE1u
#define NXGL_SCA_GL_VERSION 0x1F02u
#define NXGL_SCA_GL_EXTENSIONS 0x1F03u
#define NXGL_SCA_GL_UNSIGNED_BYTE 0x1401u
#define NXGL_SCA_GL_UNPACK_ROW_LENGTH 0x0CF2u
#define NXGL_SCA_GL_UNPACK_SKIP_ROWS 0x0CF3u
#define NXGL_SCA_GL_UNPACK_SKIP_PIXELS 0x0CF4u
#define NXGL_SCA_GL_UNPACK_ALIGNMENT 0x0CF5u
#define NXGL_SCA_MAX_TEXTURE_UNITS 32u

static void *(*g_sca_resolver)(const char *);

void nxgl_single_channel_adapter_set_resolver(void *(*resolver)(const char *)) {
  g_sca_resolver = resolver;
}

static void *nxgl_sca_resolve(const char *name) {
  void *found;
  if (name == NULL) {
    return NULL;
  }
  if (g_sca_resolver != NULL) {
    found = g_sca_resolver(name);
    if (found != NULL) {
      return found;
    }
  }
  found = dlsym(RTLD_DEFAULT, name);
  if (found != NULL) {
    return found;
  }
  {
    void *(*get_proc)(const char *) =
        (void *(*)(const char *))dlsym(RTLD_DEFAULT,
                                      "SDL_GL_GetProcAddress");
    return get_proc != NULL ? get_proc(name) : NULL;
  }
}

static void nxgl_sca_clear_runtime(nxgl_sc_adapter *adapter) {
  unsigned i;
  (void)nxgl_single_channel_tracker_reset(&adapter->tracker);
  memset(adapter->targets, 0, sizeof(adapter->targets));
  memset(adapter->contexts, 0, sizeof(adapter->contexts));
  memset(adapter->threads, 0, sizeof(adapter->threads));
  memset(adapter->bindings, 0, sizeof(adapter->bindings));
  for (i = 0u; i < NXGL_SC_ADAPTER_THREADS; i++) {
    adapter->threads[i].unpack_alignment = 4;
  }
  adapter->measured = 0;
  adapter->route = NXGL_SC_ROUTE_PASSTHROUGH;
  memset(&adapter->caps, 0, sizeof(adapter->caps));
  adapter->poisoned = 0;
}

int nxgl_single_channel_adapter_init(nxgl_sc_adapter *adapter) {
  if (adapter == NULL) {
    return -1;
  }
  memset(adapter, 0, sizeof(*adapter));
  nxgl_sca_clear_runtime(adapter);
  return 0;
}

int nxgl_single_channel_adapter_configure(
    nxgl_sc_adapter *adapter, const nxgl_sc_adapter_config *config) {
  if (adapter == NULL || config == NULL ||
      config->struct_size != sizeof(*config) ||
      config->api_version != NXGL_SC_ADAPTER_CONFIG_API_VERSION ||
      (config->semantic != NXGL_SC_SEMANTIC_PRESERVE &&
       config->semantic != NXGL_SC_SEMANTIC_ALPHA_MASK &&
       config->semantic != NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT) ||
      (config->allow_legacy_fallback != 0 &&
       config->allow_legacy_fallback != 1)) {
    return -1;
  }
  adapter->enabled = 0;
  adapter->configured = 1;
  adapter->config = *config;
  nxgl_sca_clear_runtime(adapter);
  return 0;
}

void nxgl_single_channel_adapter_enable(nxgl_sc_adapter *adapter, int on) {
  if (adapter != NULL) {
    adapter->enabled = (on && adapter->configured) ? 1 : 0;
  }
}

int nxgl_single_channel_adapter_enabled(const nxgl_sc_adapter *adapter) {
  return adapter != NULL && adapter->configured && adapter->enabled ? 1 : 0;
}

static int nxgl_sca_ext_present(const char *extensions, const char *name) {
  const char *cursor = extensions;
  size_t length;
  if (extensions == NULL || name == NULL) {
    return 0;
  }
  length = strlen(name);
  while ((cursor = strstr(cursor, name)) != NULL) {
    char before = cursor == extensions ? ' ' : cursor[-1];
    char after = cursor[length];
    if ((before == ' ' || before == '\0') &&
        (after == ' ' || after == '\0')) {
      return 1;
    }
    cursor += length;
  }
  return 0;
}

static void nxgl_sca_measure_caps(nxgl_texture_capabilities *caps) {
  const unsigned char *(*get_string)(unsigned) = NULL;
  const char *version = NULL;
  const char *extensions = NULL;
  int es3 = 0;
  memset(caps, 0, sizeof(*caps));
  get_string = (const unsigned char *(*)(unsigned))
      nxgl_sca_resolve("glGetString");
  if (get_string != NULL) {
    version = (const char *)get_string(NXGL_SCA_GL_VERSION);
    extensions = (const char *)get_string(NXGL_SCA_GL_EXTENSIONS);
  }
  if (version != NULL) {
    const char *prefix = strstr(version, "OpenGL ES ");
    if (prefix != NULL && prefix[10] >= '3' && prefix[10] <= '9') {
      es3 = 1;
    }
  }
  caps->api_version = NXGL_SINGLE_CHANNEL_API_VERSION;
  caps->struct_size = sizeof(*caps);
  caps->physical_es3 = es3;
  caps->has_tex_storage =
      nxgl_sca_resolve("glTexStorage2D") != NULL ? 1 : 0;
  caps->has_gl_red = es3 ||
                     nxgl_sca_ext_present(extensions, "GL_EXT_texture_rg");
  caps->has_texture_swizzle =
      es3 || nxgl_sca_ext_present(extensions, "GL_EXT_texture_swizzle") ||
      nxgl_sca_ext_present(extensions, "GL_ARB_texture_swizzle");
}

static nxgl_sc_context_state *nxgl_sca_context(nxgl_sc_adapter *adapter,
                                               uintptr_t context_id,
                                               int create) {
  unsigned i;
  if (context_id == (uintptr_t)0) {
    return NULL;
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_CONTEXTS; i++) {
    if (adapter->contexts[i].in_use &&
        adapter->contexts[i].context_id == context_id) {
      return &adapter->contexts[i];
    }
  }
  if (!create) {
    return NULL;
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_CONTEXTS; i++) {
    if (!adapter->contexts[i].in_use) {
      adapter->contexts[i].in_use = 1;
      adapter->contexts[i].context_id = context_id;
      adapter->contexts[i].route = NXGL_SC_ROUTE_PASSTHROUGH;
      return &adapter->contexts[i];
    }
  }
  adapter->poisoned = 1;
  return NULL;
}

nxgl_single_channel_route nxgl_single_channel_adapter_measure_context(
    nxgl_sc_adapter *adapter, uintptr_t context_id) {
  nxgl_sc_context_state *context;
  if (adapter == NULL || context_id == (uintptr_t)0) {
    return NXGL_SC_ROUTE_PASSTHROUGH;
  }
  context = nxgl_sca_context(adapter, context_id, 1);
  if (context == NULL) {
    return NXGL_SC_ROUTE_PASSTHROUGH;
  }
  nxgl_sca_measure_caps(&context->caps);
  context->route = nxgl_single_channel_decide(&context->caps);
  if (context->route == NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP &&
      (!adapter->configured || !adapter->config.allow_legacy_fallback)) {
    context->route = NXGL_SC_ROUTE_PASSTHROUGH;
  }
  context->measured = 1;
  adapter->caps = context->caps;
  adapter->route = context->route;
  adapter->measured = 1;
  return context->route;
}

nxgl_single_channel_route nxgl_single_channel_adapter_measure(
    nxgl_sc_adapter *adapter) {
  if (adapter == NULL) {
    return NXGL_SC_ROUTE_PASSTHROUGH;
  }
  /* Honest legacy receipt only. No context-scoped rewrite is armed. */
  nxgl_sca_measure_caps(&adapter->caps);
  adapter->route = nxgl_single_channel_decide(&adapter->caps);
  adapter->measured = 1;
  return adapter->route;
}

static nxgl_sc_thread_state *nxgl_sca_thread(nxgl_sc_adapter *adapter,
                                             uintptr_t context_id,
                                             uintptr_t thread_id,
                                             int create) {
  unsigned i;
  if (context_id == (uintptr_t)0 || thread_id == (uintptr_t)0) {
    return NULL;
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_THREADS; i++) {
    if (adapter->threads[i].in_use &&
        adapter->threads[i].context_id == context_id &&
        adapter->threads[i].thread_id == thread_id) {
      return &adapter->threads[i];
    }
  }
  if (!create) {
    return NULL;
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_THREADS; i++) {
    if (!adapter->threads[i].in_use) {
      nxgl_sc_thread_state *state = &adapter->threads[i];
      memset(state, 0, sizeof(*state));
      state->in_use = 1;
      state->context_id = context_id;
      state->thread_id = thread_id;
      state->unpack_alignment = 4;
      return state;
    }
  }
  adapter->poisoned = 1;
  return NULL;
}

int nxgl_single_channel_adapter_on_active_texture(nxgl_sc_adapter *adapter,
                                                  uintptr_t context_id,
                                                  uintptr_t thread_id,
                                                  unsigned unit) {
  nxgl_sc_thread_state *thread;
  if (adapter == NULL || unit >= NXGL_SCA_MAX_TEXTURE_UNITS) {
    if (adapter != NULL) {
      adapter->poisoned = 1;
    }
    return 0;
  }
  thread = nxgl_sca_thread(adapter, context_id, thread_id, 1);
  if (thread == NULL) {
    return 0;
  }
  thread->active_unit = unit;
  return 1;
}

static nxgl_sc_binding_state *nxgl_sca_binding(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned unit, unsigned target, int create) {
  unsigned i;
  for (i = 0u; i < NXGL_SC_ADAPTER_BINDINGS; i++) {
    nxgl_sc_binding_state *binding = &adapter->bindings[i];
    if (binding->in_use && binding->context_id == context_id &&
        binding->thread_id == thread_id && binding->unit == unit &&
        binding->target == target) {
      return binding;
    }
  }
  if (!create) {
    return NULL;
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_BINDINGS; i++) {
    if (!adapter->bindings[i].in_use) {
      nxgl_sc_binding_state *binding = &adapter->bindings[i];
      memset(binding, 0, sizeof(*binding));
      binding->in_use = 1;
      binding->context_id = context_id;
      binding->thread_id = thread_id;
      binding->unit = unit;
      binding->target = target;
      return binding;
    }
  }
  adapter->poisoned = 1;
  return NULL;
}

int nxgl_single_channel_adapter_on_bind_v2(nxgl_sc_adapter *adapter,
                                           uintptr_t context_id,
                                           uintptr_t thread_id,
                                           unsigned target,
                                           unsigned texture_id) {
  nxgl_sc_thread_state *thread;
  nxgl_sc_binding_state *binding;
  if (adapter == NULL || target == 0u) {
    return 0;
  }
  thread = nxgl_sca_thread(adapter, context_id, thread_id, 1);
  if (thread == NULL) {
    return 0;
  }
  binding = nxgl_sca_binding(adapter, context_id, thread_id,
                             thread->active_unit, target, 1);
  if (binding == NULL) {
    return 0;
  }
  binding->texture_id = texture_id;
  return 1;
}

int nxgl_single_channel_adapter_on_pixel_store(nxgl_sc_adapter *adapter,
                                               uintptr_t context_id,
                                               uintptr_t thread_id,
                                               unsigned pname, int value) {
  nxgl_sc_thread_state *thread;
  if (adapter == NULL) {
    return 0;
  }
  thread = nxgl_sca_thread(adapter, context_id, thread_id, 1);
  if (thread == NULL) {
    return 0;
  }
  switch (pname) {
    case NXGL_SCA_GL_UNPACK_ALIGNMENT:
      if (value != 1 && value != 2 && value != 4 && value != 8) {
        adapter->poisoned = 1;
        return 0;
      }
      thread->unpack_alignment = value;
      return 1;
    case NXGL_SCA_GL_UNPACK_ROW_LENGTH:
      if (value < 0) {
        adapter->poisoned = 1;
        return 0;
      }
      thread->unpack_row_length = value;
      return 1;
    case NXGL_SCA_GL_UNPACK_SKIP_ROWS:
      if (value < 0) {
        adapter->poisoned = 1;
        return 0;
      }
      thread->unpack_skip_rows = value;
      return 1;
    case NXGL_SCA_GL_UNPACK_SKIP_PIXELS:
      if (value < 0) {
        adapter->poisoned = 1;
        return 0;
      }
      thread->unpack_skip_pixels = value;
      return 1;
    default:
      return 0;
  }
}

int nxgl_single_channel_adapter_on_unpack_buffer(nxgl_sc_adapter *adapter,
                                                 uintptr_t context_id,
                                                 uintptr_t thread_id,
                                                 unsigned buffer_id) {
  nxgl_sc_thread_state *thread;
  if (adapter == NULL) {
    return 0;
  }
  thread = nxgl_sca_thread(adapter, context_id, thread_id, 1);
  if (thread == NULL) {
    return 0;
  }
  thread->pixel_unpack_buffer = buffer_id;
  return 1;
}

static void nxgl_sca_forget_object(nxgl_sc_adapter *adapter,
                                   uintptr_t context_id,
                                   unsigned texture_id) {
  unsigned i;
  for (i = 0u; i < NXGL_SC_TRACKER_CAPACITY; i++) {
    nxgl_sc_texture_state *state = &adapter->tracker.slots[i];
    if (state->in_use && state->context_id == context_id &&
        state->texture_id == texture_id) {
      (void)nxgl_single_channel_tracker_forget_v2(
          &adapter->tracker, context_id, state->target, texture_id);
    }
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_BINDINGS; i++) {
    if (adapter->bindings[i].in_use &&
        adapter->bindings[i].context_id == context_id &&
        adapter->bindings[i].texture_id == texture_id) {
      adapter->bindings[i].texture_id = 0u;
    }
  }
}

void nxgl_single_channel_adapter_on_delete_v2(nxgl_sc_adapter *adapter,
                                              uintptr_t context_id,
                                              int count,
                                              const unsigned *ids) {
  int i;
  if (adapter == NULL || context_id == (uintptr_t)0 || count <= 0 ||
      ids == NULL) {
    return;
  }
  for (i = 0; i < count; i++) {
    nxgl_sca_forget_object(adapter, context_id, ids[i]);
  }
}

void nxgl_single_channel_adapter_on_context_lost(nxgl_sc_adapter *adapter,
                                                 uintptr_t context_id) {
  unsigned i;
  if (adapter == NULL || context_id == (uintptr_t)0) {
    return;
  }
  nxgl_single_channel_tracker_forget_context(&adapter->tracker, context_id);
  for (i = 0u; i < NXGL_SC_ADAPTER_CONTEXTS; i++) {
    if (adapter->contexts[i].in_use &&
        adapter->contexts[i].context_id == context_id) {
      memset(&adapter->contexts[i], 0, sizeof(adapter->contexts[i]));
    }
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_THREADS; i++) {
    if (adapter->threads[i].in_use &&
        adapter->threads[i].context_id == context_id) {
      memset(&adapter->threads[i], 0, sizeof(adapter->threads[i]));
      adapter->threads[i].unpack_alignment = 4;
    }
  }
  for (i = 0u; i < NXGL_SC_ADAPTER_BINDINGS; i++) {
    if (adapter->bindings[i].in_use &&
        adapter->bindings[i].context_id == context_id) {
      memset(&adapter->bindings[i], 0, sizeof(adapter->bindings[i]));
    }
  }
}

static void nxgl_sca_op_init(nxgl_sc_op *out, unsigned internalformat,
                             unsigned format) {
  memset(out, 0, sizeof(*out));
  out->internalformat = internalformat;
  out->format = format;
  out->coherent = 1;
  out->status = NXGL_SC_OP_PASS;
  out->source_kind = NXGL_SC_KIND_RGBA;
}

static void nxgl_sca_reject(nxgl_sc_adapter *adapter, nxgl_sc_op *out) {
  out->handled = 0;
  out->coherent = 0;
  out->status = NXGL_SC_OP_REJECT;
  if (adapter != NULL) {
    adapter->poisoned = 1;
  }
}

static nxgl_sc_binding_state *nxgl_sca_current_binding(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, nxgl_sc_thread_state **thread_out) {
  nxgl_sc_thread_state *thread =
      nxgl_sca_thread(adapter, context_id, thread_id, 0);
  if (thread_out != NULL) {
    *thread_out = thread;
  }
  if (thread == NULL) {
    return NULL;
  }
  return nxgl_sca_binding(adapter, context_id, thread_id, thread->active_unit,
                          target, 0);
}

static int nxgl_sca_apply_plan(nxgl_sc_adapter *adapter,
                               nxgl_sc_context_state *context,
                               nxgl_sc_channel_kind kind,
                               nxgl_sc_op *out) {
  nxgl_sc_upload_plan plan;
  if (!nxgl_single_channel_plan_v2(context->route, kind,
                                   adapter->config.semantic, &plan) ||
      plan.reject) {
    nxgl_sca_reject(adapter, out);
    return 0;
  }
  if (!plan.handled) {
    return 1;
  }
  out->handled = 1;
  out->status = NXGL_SC_OP_REWRITE;
  out->source_kind = kind;
  out->internalformat = plan.internalformat;
  out->format = plan.format;
  out->duplicate_byte = plan.duplicate_byte;
  out->apply_swizzle_a_from_r = plan.apply_swizzle_a_from_r;
  out->transform = plan.transform;
  out->apply_swizzle = plan.apply_swizzle;
  out->swizzle_r = plan.swizzle_r;
  out->swizzle_g = plan.swizzle_g;
  out->swizzle_b = plan.swizzle_b;
  out->swizzle_a = plan.swizzle_a;
  return 1;
}

static int nxgl_sca_note(nxgl_sc_adapter *adapter,
                         uintptr_t context_id, unsigned target,
                         unsigned texture_id, int storage,
                         nxgl_sc_context_state *context,
                         nxgl_sc_channel_kind kind, nxgl_sc_op *out) {
  nxgl_sc_note_result note = nxgl_single_channel_tracker_note_v2(
      &adapter->tracker, context_id, target, texture_id, storage,
      context->route, kind, adapter->config.semantic);
  if (note != NXGL_SC_NOTE_COHERENT) {
    nxgl_sca_reject(adapter, out);
    return 0;
  }
  return 1;
}

static int nxgl_sca_ready(nxgl_sc_adapter *adapter, uintptr_t context_id,
                          nxgl_sc_context_state **context_out) {
  nxgl_sc_context_state *context;
  if (adapter == NULL || !adapter->enabled || !adapter->configured) {
    return 0;
  }
  context = nxgl_sca_context(adapter, context_id, 0);
  if (context_out != NULL) {
    *context_out = context;
  }
  return context != NULL && context->measured;
}

int nxgl_single_channel_adapter_plan_storage_v2(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, unsigned internalformat, nxgl_sc_op *out) {
  nxgl_sc_channel_kind kind;
  nxgl_sc_context_state *context = NULL;
  nxgl_sc_binding_state *binding;
  if (out == NULL) {
    return 0;
  }
  nxgl_sca_op_init(out, internalformat, 0u);
  kind = nxgl_single_channel_classify_internal(internalformat);
  if (kind == NXGL_SC_KIND_RGBA) {
    return 1;
  }
  out->source_kind = kind;
  if (target != NXGL_SCA_GL_TEXTURE_2D ||
      !nxgl_sca_ready(adapter, context_id, &context) || context == NULL ||
      adapter->poisoned || context->route == NXGL_SC_ROUTE_PASSTHROUGH) {
    nxgl_sca_reject(adapter, out);
    return 1;
  }
  binding = nxgl_sca_current_binding(adapter, context_id, thread_id, target,
                                     NULL);
  if (binding == NULL || binding->texture_id == 0u ||
      !nxgl_sca_apply_plan(adapter, context, kind, out) ||
      !nxgl_sca_note(adapter, context_id, target, binding->texture_id, 1,
                     context, kind, out)) {
    if (out->status != NXGL_SC_OP_REJECT) {
      nxgl_sca_reject(adapter, out);
    }
  }
  return 1;
}

static int nxgl_sca_mul(size_t a, size_t b, size_t *out) {
  if (out == NULL || (a != 0u && b > SIZE_MAX / a)) {
    return 0;
  }
  *out = a * b;
  return 1;
}

static int nxgl_sca_add(size_t a, size_t b, size_t *out) {
  if (out == NULL || b > SIZE_MAX - a) {
    return 0;
  }
  *out = a + b;
  return 1;
}

static int nxgl_sca_layout(nxgl_sc_adapter *adapter,
                           const nxgl_sc_thread_state *thread,
                           const nxgl_sc_image_desc *image,
                           nxgl_sc_op *out) {
  size_t row_pixels;
  size_t stride;
  size_t offset;
  size_t tail;
  size_t required;
  size_t converted_pixels;
  size_t alignment;
  if (thread == NULL || image == NULL || image->width <= 0 ||
      image->height <= 0 || image->xoffset < 0 || image->yoffset < 0 ||
      thread->unpack_row_length < 0 || thread->unpack_skip_rows < 0 ||
      thread->unpack_skip_pixels < 0 ||
      (thread->unpack_alignment != 1 && thread->unpack_alignment != 2 &&
       thread->unpack_alignment != 4 && thread->unpack_alignment != 8)) {
    nxgl_sca_reject(adapter, out);
    return 0;
  }
  row_pixels = thread->unpack_row_length > 0
                   ? (size_t)thread->unpack_row_length
                   : (size_t)image->width;
  if (row_pixels < (size_t)image->width ||
      (size_t)thread->unpack_skip_pixels > row_pixels - (size_t)image->width) {
    nxgl_sca_reject(adapter, out);
    return 0;
  }
  alignment = (size_t)thread->unpack_alignment;
  if (!nxgl_sca_add(row_pixels, alignment - 1u, &stride)) {
    nxgl_sca_reject(adapter, out);
    return 0;
  }
  stride -= stride % alignment;
  if (!nxgl_sca_mul((size_t)thread->unpack_skip_rows, stride, &offset) ||
      !nxgl_sca_add(offset, (size_t)thread->unpack_skip_pixels, &offset) ||
      !nxgl_sca_mul((size_t)(image->height - 1), stride, &tail) ||
      !nxgl_sca_add(offset, tail, &required) ||
      !nxgl_sca_add(required, (size_t)image->width, &required) ||
      !nxgl_sca_mul((size_t)image->width, (size_t)image->height,
                    &converted_pixels) ||
      !nxgl_sca_mul(converted_pixels, 2u, &out->converted_size) ||
      required > image->data_size) {
    nxgl_sca_reject(adapter, out);
    return 0;
  }
  out->source_offset = offset;
  out->source_stride = stride;
  out->source_row_bytes = (size_t)image->width;
  out->source_required = required;
  out->reset_unpack_for_converted_data = 1;
  return 1;
}

static nxgl_sc_texture_state *nxgl_sca_texture_state(
    nxgl_sc_adapter *adapter, uintptr_t context_id, unsigned target,
    unsigned texture_id) {
  unsigned i;
  for (i = 0u; i < NXGL_SC_TRACKER_CAPACITY; i++) {
    nxgl_sc_texture_state *state = &adapter->tracker.slots[i];
    if (state->in_use && state->context_id == context_id &&
        state->target == target && state->texture_id == texture_id) {
      return state;
    }
  }
  return NULL;
}

static int nxgl_sca_plan_image_common(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, const nxgl_sc_image_desc *image, int subimage,
    nxgl_sc_op *out) {
  nxgl_sc_channel_kind kind;
  nxgl_sc_context_state *context = NULL;
  nxgl_sc_thread_state *thread = NULL;
  nxgl_sc_binding_state *binding;
  nxgl_sc_texture_state *texture = NULL;
  if (out == NULL || image == NULL) {
    return 0;
  }
  nxgl_sca_op_init(out, subimage ? 0u : image->internalformat, image->format);
  kind = subimage
             ? nxgl_single_channel_classify_internal(image->format)
             : nxgl_single_channel_classify(image->internalformat,
                                            image->format);
  if (kind == NXGL_SC_KIND_RGBA) {
    return 1;
  }
  out->source_kind = kind;
  if (kind == NXGL_SC_KIND_INVALID || target != NXGL_SCA_GL_TEXTURE_2D ||
      image->width <= 0 || image->height <= 0 || image->xoffset < 0 ||
      image->yoffset < 0 ||
      !nxgl_sca_ready(adapter, context_id, &context) || context == NULL ||
      adapter->poisoned || context->route == NXGL_SC_ROUTE_PASSTHROUGH ||
      image->type != NXGL_SCA_GL_UNSIGNED_BYTE) {
    nxgl_sca_reject(adapter, out);
    return 1;
  }
  binding = nxgl_sca_current_binding(adapter, context_id, thread_id, target,
                                     &thread);
  if (binding == NULL || binding->texture_id == 0u || thread == NULL) {
    nxgl_sca_reject(adapter, out);
    return 1;
  }
  if (subimage) {
    texture = nxgl_sca_texture_state(adapter, context_id, target,
                                     binding->texture_id);
    if (texture == NULL || texture->channel_kind != (int)kind ||
        texture->semantic != (int)adapter->config.semantic) {
      nxgl_sca_reject(adapter, out);
      return 1;
    }
  }
  if (!nxgl_sca_apply_plan(adapter, context, kind, out)) {
    return 1;
  }
  if (out->transform != NXGL_SC_TRANSFORM_NONE) {
    if (thread->pixel_unpack_buffer != 0u ||
        (image->pixels == NULL && subimage)) {
      nxgl_sca_reject(adapter, out);
      return 1;
    }
    if (image->pixels != NULL &&
        !nxgl_sca_layout(adapter, thread, image, out)) {
      return 1;
    }
  }
  if (!subimage &&
      !nxgl_sca_note(adapter, context_id, target, binding->texture_id, 1,
                     context, kind, out)) {
    return 1;
  }
  if (!nxgl_sca_note(adapter, context_id, target, binding->texture_id, 0,
                     context, kind, out)) {
    return 1;
  }
  return 1;
}

int nxgl_single_channel_adapter_plan_image_v2(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, const nxgl_sc_image_desc *image, nxgl_sc_op *out) {
  return nxgl_sca_plan_image_common(adapter, context_id, thread_id, target,
                                    image, 0, out);
}

int nxgl_single_channel_adapter_plan_subimage_v2(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, const nxgl_sc_image_desc *image, nxgl_sc_op *out) {
  return nxgl_sca_plan_image_common(adapter, context_id, thread_id, target,
                                    image, 1, out);
}

size_t nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
    const unsigned char *src, size_t count, unsigned char *dst,
    size_t dst_cap) {
  size_t required;
  size_t i;
  if (src == NULL || dst == NULL || count == 0u || count > SIZE_MAX / 2u) {
    return 0u;
  }
  required = count * 2u;
  if (required > dst_cap) {
    return 0u;
  }
  for (i = 0u; i < count; i++) {
    dst[i * 2u] = src[i];
    dst[i * 2u + 1u] = src[i];
  }
  return required;
}

size_t nxgl_single_channel_adapter_convert(
    const nxgl_sc_op *op, const nxgl_sc_image_desc *image,
    unsigned char *dst, size_t dst_cap) {
  const unsigned char *src;
  size_t row;
  size_t col;
  size_t output = 0u;
  if (op == NULL || image == NULL || dst == NULL || image->pixels == NULL ||
      op->status != NXGL_SC_OP_REWRITE ||
      op->transform == NXGL_SC_TRANSFORM_NONE ||
      op->converted_size == 0u || op->converted_size > dst_cap ||
      op->source_required > image->data_size || image->width <= 0 ||
      image->height <= 0) {
    return 0u;
  }
  src = (const unsigned char *)image->pixels + op->source_offset;
  for (row = 0u; row < (size_t)image->height; row++) {
    const unsigned char *source_row = src + row * op->source_stride;
    if (op->transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP) {
      size_t written =
          nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
              source_row, (size_t)image->width, dst + output,
              dst_cap - output);
      if (written == 0u) {
        return 0u;
      }
      output += written;
      continue;
    }
    for (col = 0u; col < (size_t)image->width; col++) {
      unsigned char value = source_row[col];
      if (op->transform == NXGL_SC_TRANSFORM_LUMINANCE_TO_LA) {
        dst[output++] = value;
        dst[output++] = 0xffu;
      } else {
        dst[output++] = 0xffu;
        dst[output++] = value;
      }
    }
  }
  return output == op->converted_size ? output : 0u;
}

/* API-1 cannot supply context/thread/unit/pixel-store/PBO identity. Keep it
 * linkable and strictly pass-through; this blocks the old unsafe enable path. */
void nxgl_single_channel_adapter_on_bind(nxgl_sc_adapter *adapter,
                                         unsigned target,
                                         unsigned texture_id) {
  (void)adapter;
  (void)target;
  (void)texture_id;
}

void nxgl_single_channel_adapter_on_delete(nxgl_sc_adapter *adapter, int count,
                                           const unsigned *ids) {
  (void)adapter;
  (void)count;
  (void)ids;
}

int nxgl_single_channel_adapter_plan_storage(nxgl_sc_adapter *adapter,
                                             unsigned target,
                                             unsigned internalformat,
                                             unsigned format,
                                             nxgl_sc_op *out) {
  (void)adapter;
  (void)target;
  if (out == NULL) {
    return 0;
  }
  nxgl_sca_op_init(out, internalformat, format);
  return 1;
}

int nxgl_single_channel_adapter_plan_upload(nxgl_sc_adapter *adapter,
                                            unsigned target,
                                            unsigned internalformat,
                                            unsigned format,
                                            nxgl_sc_op *out) {
  return nxgl_single_channel_adapter_plan_storage(
      adapter, target, internalformat, format, out);
}

size_t nxgl_single_channel_adapter_duplicate_r_to_la(
    const unsigned char *src, size_t count, unsigned char *dst,
    size_t dst_cap) {
  (void)src;
  (void)count;
  (void)dst;
  (void)dst_cap;
  return 0u;
}
