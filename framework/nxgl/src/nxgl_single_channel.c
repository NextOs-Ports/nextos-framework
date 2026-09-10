/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_single_channel.h"

#include <stdio.h>

#define NXGL_SC_GL_ZERO 0u
#define NXGL_SC_GL_ONE 1u
#define NXGL_SC_GL_RED 0x1903u
#define NXGL_SC_GL_R8 0x8229u
#define NXGL_SC_GL_ALPHA 0x1906u
#define NXGL_SC_GL_ALPHA8 0x803Cu
#define NXGL_SC_GL_LUMINANCE 0x1909u
#define NXGL_SC_GL_LUMINANCE8 0x8040u
#define NXGL_SC_GL_LUMINANCE_ALPHA 0x190Au

static int nxgl_sc_bool(int value) { return value == 0 || value == 1; }

static int nxgl_sc_semantic_valid(nxgl_sc_semantic semantic) {
  return semantic == NXGL_SC_SEMANTIC_PRESERVE ||
         semantic == NXGL_SC_SEMANTIC_ALPHA_MASK ||
         semantic == NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT;
}

static int nxgl_sc_caps_valid(const nxgl_texture_capabilities *caps) {
  return caps != NULL &&
         caps->api_version == NXGL_SINGLE_CHANNEL_API_VERSION &&
         caps->struct_size == sizeof(*caps) &&
         nxgl_sc_bool(caps->physical_es3) &&
         nxgl_sc_bool(caps->has_tex_storage) &&
         nxgl_sc_bool(caps->has_texture_swizzle) &&
         nxgl_sc_bool(caps->has_gl_red);
}

nxgl_single_channel_route nxgl_single_channel_decide(
    const nxgl_texture_capabilities *caps) {
  if (!nxgl_sc_caps_valid(caps)) {
    return NXGL_SC_ROUTE_PASSTHROUGH;
  }
  if (caps->physical_es3 && caps->has_tex_storage &&
      caps->has_texture_swizzle && caps->has_gl_red) {
    return NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE;
  }
  return NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP;
}

int nxgl_single_channel_coherent(nxgl_single_channel_route storage_route,
                                 nxgl_single_channel_route upload_route) {
  if (storage_route == NXGL_SC_ROUTE_PASSTHROUGH ||
      upload_route == NXGL_SC_ROUTE_PASSTHROUGH) {
    return 0;
  }
  return storage_route == upload_route ? 1 : 0;
}

const char *nxgl_single_channel_route_name(nxgl_single_channel_route route) {
  switch (route) {
    case NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE:
      return "native-r8-swizzle";
    case NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP:
      return "luminance-alpha-dup";
    case NXGL_SC_ROUTE_PASSTHROUGH:
    default:
      return "passthrough";
  }
}

size_t nxgl_single_channel_receipt(const nxgl_texture_capabilities *caps,
                                   nxgl_single_channel_route route,
                                   char *buf, size_t cap) {
  int written;
  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!nxgl_sc_caps_valid(caps)) {
    return 0u;
  }
  written = snprintf(buf, cap,
                     "SINGLE-CHANNEL: route=%s es3=%d tex_storage=%d "
                     "swizzle=%d red=%d",
                     nxgl_single_channel_route_name(route),
                     caps->physical_es3, caps->has_tex_storage,
                     caps->has_texture_swizzle, caps->has_gl_red);
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}

static nxgl_sc_channel_kind nxgl_sc_classify_one(unsigned value) {
  switch (value) {
    case NXGL_SC_GL_RED:
    case NXGL_SC_GL_R8:
      return NXGL_SC_KIND_RED;
    case NXGL_SC_GL_ALPHA:
    case NXGL_SC_GL_ALPHA8:
      return NXGL_SC_KIND_ALPHA;
    case NXGL_SC_GL_LUMINANCE:
    case NXGL_SC_GL_LUMINANCE8:
      return NXGL_SC_KIND_LUMINANCE;
    default:
      return NXGL_SC_KIND_RGBA;
  }
}

nxgl_sc_channel_kind nxgl_single_channel_classify_internal(
    unsigned gl_internalformat) {
  return nxgl_sc_classify_one(gl_internalformat);
}

nxgl_sc_channel_kind nxgl_single_channel_classify(unsigned gl_internalformat,
                                                  unsigned gl_format) {
  nxgl_sc_channel_kind internal = nxgl_sc_classify_one(gl_internalformat);
  nxgl_sc_channel_kind format = nxgl_sc_classify_one(gl_format);
  if (internal == NXGL_SC_KIND_RGBA && format == NXGL_SC_KIND_RGBA) {
    return NXGL_SC_KIND_RGBA;
  }
  if (internal == NXGL_SC_KIND_RGBA || format == NXGL_SC_KIND_RGBA ||
      internal != format) {
    return NXGL_SC_KIND_INVALID;
  }
  return internal;
}

static void nxgl_sc_plan_clear(nxgl_sc_upload_plan *out) {
  out->handled = 0;
  out->internalformat = 0u;
  out->format = 0u;
  out->duplicate_byte = 0;
  out->apply_swizzle_a_from_r = 0;
  out->reject = 0;
  out->transform = NXGL_SC_TRANSFORM_NONE;
  out->apply_swizzle = 0;
  out->swizzle_r = NXGL_SC_GL_RED;
  out->swizzle_g = NXGL_SC_GL_ZERO;
  out->swizzle_b = NXGL_SC_GL_ZERO;
  out->swizzle_a = NXGL_SC_GL_ONE;
}

int nxgl_single_channel_plan_v2(nxgl_single_channel_route route,
                                nxgl_sc_channel_kind kind,
                                nxgl_sc_semantic semantic,
                                nxgl_sc_upload_plan *out) {
  if (out == NULL) {
    return 0;
  }
  nxgl_sc_plan_clear(out);
  if (kind == NXGL_SC_KIND_RGBA) {
    return 1;
  }
  if (kind == NXGL_SC_KIND_INVALID || !nxgl_sc_semantic_valid(semantic) ||
      route == NXGL_SC_ROUTE_PASSTHROUGH) {
    out->reject = 1;
    return 0;
  }

  out->handled = 1;
  if (route == NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) {
    out->internalformat = NXGL_SC_GL_R8;
    out->format = NXGL_SC_GL_RED;
    out->apply_swizzle = 1;
    if (semantic == NXGL_SC_SEMANTIC_ALPHA_MASK ||
        (semantic == NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT &&
         kind == NXGL_SC_KIND_RED) ||
        kind == NXGL_SC_KIND_ALPHA) {
      out->swizzle_r = NXGL_SC_GL_ONE;
      out->swizzle_g = NXGL_SC_GL_ONE;
      out->swizzle_b = NXGL_SC_GL_ONE;
      out->swizzle_a = NXGL_SC_GL_RED;
      out->apply_swizzle_a_from_r = 1;
    } else if (kind == NXGL_SC_KIND_LUMINANCE) {
      out->swizzle_r = NXGL_SC_GL_RED;
      out->swizzle_g = NXGL_SC_GL_RED;
      out->swizzle_b = NXGL_SC_GL_RED;
      out->swizzle_a = NXGL_SC_GL_ONE;
    }
    return 1;
  }

  if (route != NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP ||
      (semantic == NXGL_SC_SEMANTIC_PRESERVE &&
       kind == NXGL_SC_KIND_RED)) {
    /* LA cannot represent RED's native (r,0,0,1) sampling semantics. */
    out->handled = 0;
    out->reject = 1;
    return 0;
  }
  out->internalformat = NXGL_SC_GL_LUMINANCE_ALPHA;
  out->format = NXGL_SC_GL_LUMINANCE_ALPHA;
  out->duplicate_byte = 1;
  if (semantic == NXGL_SC_SEMANTIC_ALPHA_MASK) {
    out->transform = NXGL_SC_TRANSFORM_MASK_TO_LA;
  } else if (semantic == NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT &&
             kind == NXGL_SC_KIND_RED) {
    out->transform = NXGL_SC_TRANSFORM_RED_TO_LA_DUP;
  } else if (kind == NXGL_SC_KIND_ALPHA) {
    out->transform = NXGL_SC_TRANSFORM_ALPHA_TO_LA;
  } else {
    out->transform = NXGL_SC_TRANSFORM_LUMINANCE_TO_LA;
  }
  return 1;
}

int nxgl_single_channel_plan(nxgl_single_channel_route route,
                             nxgl_sc_channel_kind kind,
                             nxgl_sc_upload_plan *out) {
  return nxgl_single_channel_plan_v2(route, kind,
                                     NXGL_SC_SEMANTIC_PRESERVE, out);
}

int nxgl_single_channel_tracker_reset(nxgl_sc_tracker *tracker) {
  unsigned i;
  if (tracker == NULL) {
    return -1;
  }
  for (i = 0u; i < NXGL_SC_TRACKER_CAPACITY; i++) {
    tracker->slots[i].texture_id = 0u;
    tracker->slots[i].in_use = 0;
    tracker->slots[i].storage_route = -1;
    tracker->slots[i].upload_route = -1;
    tracker->slots[i].context_id = (uintptr_t)0;
    tracker->slots[i].target = 0u;
    tracker->slots[i].channel_kind = -1;
    tracker->slots[i].semantic = -1;
  }
  tracker->used = 0u;
  return 0;
}

static nxgl_sc_texture_state *nxgl_sc_find(nxgl_sc_tracker *tracker,
                                           uintptr_t context_id,
                                           unsigned target,
                                           unsigned texture_id) {
  unsigned i;
  for (i = 0u; i < NXGL_SC_TRACKER_CAPACITY; i++) {
    nxgl_sc_texture_state *state = &tracker->slots[i];
    if (state->in_use && state->context_id == context_id &&
        state->target == target && state->texture_id == texture_id) {
      return state;
    }
  }
  return NULL;
}

nxgl_sc_note_result nxgl_single_channel_tracker_note_v2(
    nxgl_sc_tracker *tracker, uintptr_t context_id, unsigned target,
    unsigned texture_id, int is_storage, nxgl_single_channel_route route,
    nxgl_sc_channel_kind kind, nxgl_sc_semantic semantic) {
  nxgl_sc_texture_state *state;
  unsigned i;
  if (tracker == NULL || context_id == (uintptr_t)0 || target == 0u ||
      texture_id == 0u || (is_storage != 0 && is_storage != 1) ||
      (route != NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE &&
       route != NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP) ||
      kind < NXGL_SC_KIND_RED || kind > NXGL_SC_KIND_LUMINANCE ||
      !nxgl_sc_semantic_valid(semantic)) {
    return NXGL_SC_NOTE_OVERFLOW;
  }
  state = nxgl_sc_find(tracker, context_id, target, texture_id);
  if (state == NULL) {
    for (i = 0u; i < NXGL_SC_TRACKER_CAPACITY; i++) {
      if (!tracker->slots[i].in_use) {
        state = &tracker->slots[i];
        state->texture_id = texture_id;
        state->in_use = 1;
        state->storage_route = -1;
        state->upload_route = -1;
        state->context_id = context_id;
        state->target = target;
        state->channel_kind = (int)kind;
        state->semantic = (int)semantic;
        tracker->used++;
        break;
      }
    }
  }
  if (state == NULL) {
    return NXGL_SC_NOTE_OVERFLOW;
  }
  if (state->channel_kind != (int)kind || state->semantic != (int)semantic) {
    return NXGL_SC_NOTE_MIXED;
  }
  if (is_storage) {
    state->storage_route = (int)route;
  } else {
    state->upload_route = (int)route;
  }
  if (state->storage_route != -1 && state->upload_route != -1 &&
      state->storage_route != state->upload_route) {
    return NXGL_SC_NOTE_MIXED;
  }
  return NXGL_SC_NOTE_COHERENT;
}

nxgl_sc_note_result nxgl_single_channel_tracker_note(
    nxgl_sc_tracker *tracker, unsigned texture_id, int is_storage,
    nxgl_single_channel_route route) {
  return nxgl_single_channel_tracker_note_v2(
      tracker, (uintptr_t)1u, 1u, texture_id, is_storage, route,
      NXGL_SC_KIND_RED, NXGL_SC_SEMANTIC_ALPHA_MASK);
}

int nxgl_single_channel_tracker_forget_v2(nxgl_sc_tracker *tracker,
                                          uintptr_t context_id,
                                          unsigned target,
                                          unsigned texture_id) {
  nxgl_sc_texture_state *state;
  if (tracker == NULL) {
    return 0;
  }
  state = nxgl_sc_find(tracker, context_id, target, texture_id);
  if (state == NULL) {
    return 0;
  }
  state->in_use = 0;
  state->texture_id = 0u;
  state->storage_route = -1;
  state->upload_route = -1;
  state->context_id = (uintptr_t)0;
  state->target = 0u;
  state->channel_kind = -1;
  state->semantic = -1;
  if (tracker->used > 0u) {
    tracker->used--;
  }
  return 1;
}

int nxgl_single_channel_tracker_forget(nxgl_sc_tracker *tracker,
                                       unsigned texture_id) {
  return nxgl_single_channel_tracker_forget_v2(
      tracker, (uintptr_t)1u, 1u, texture_id);
}

void nxgl_single_channel_tracker_forget_context(nxgl_sc_tracker *tracker,
                                                uintptr_t context_id) {
  unsigned i;
  if (tracker == NULL || context_id == (uintptr_t)0) {
    return;
  }
  for (i = 0u; i < NXGL_SC_TRACKER_CAPACITY; i++) {
    if (tracker->slots[i].in_use &&
        tracker->slots[i].context_id == context_id) {
      tracker->slots[i].in_use = 0;
      tracker->slots[i].texture_id = 0u;
      tracker->slots[i].storage_route = -1;
      tracker->slots[i].upload_route = -1;
      tracker->slots[i].context_id = (uintptr_t)0;
      tracker->slots[i].target = 0u;
      tracker->slots[i].channel_kind = -1;
      tracker->slots[i].semantic = -1;
      if (tracker->used > 0u) {
        tracker->used--;
      }
    }
  }
}
