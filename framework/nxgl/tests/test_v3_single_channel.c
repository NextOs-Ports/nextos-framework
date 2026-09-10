/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for nxgl_single_channel (V3 single-channel texture route). */
#include "nxgl_single_channel.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void check(int cond, const char *msg) {
  if (!cond) {
    (void)fprintf(stderr, "nxgl_single_channel: %s\n", msg);
    exit(1);
  }
}

static nxgl_texture_capabilities caps(int es3, int storage, int swizzle,
                                      int red) {
  nxgl_texture_capabilities c;
  c.api_version = NXGL_SINGLE_CHANNEL_API_VERSION;
  c.struct_size = sizeof(c);
  c.physical_es3 = es3;
  c.has_tex_storage = storage;
  c.has_texture_swizzle = swizzle;
  c.has_gl_red = red;
  return c;
}

int main(void) {
  nxgl_texture_capabilities c;
  char buf[128];

  /* Additive enum contract: every pre-0.2.17 numeric value stays literal. */
  check(NXGL_SC_SEMANTIC_PRESERVE == 1 &&
            NXGL_SC_SEMANTIC_ALPHA_MASK == 2 &&
            NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT == 3,
        "semantic numeric compatibility");
  check(NXGL_SC_TRANSFORM_NONE == 0 &&
            NXGL_SC_TRANSFORM_ALPHA_TO_LA == 1 &&
            NXGL_SC_TRANSFORM_LUMINANCE_TO_LA == 2 &&
            NXGL_SC_TRANSFORM_MASK_TO_LA == 3 &&
            NXGL_SC_TRANSFORM_RED_TO_LA_DUP == 4,
        "transform numeric compatibility");

  /* Full ES3 contract -> native. */
  c = caps(1, 1, 1, 1);
  check(nxgl_single_channel_decide(&c) == NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
        "all ES3 caps -> native r8 swizzle");

  /* ES3 but swizzle not honored (the field bug device) -> narrow fallback. */
  c = caps(1, 1, 0, 1);
  check(nxgl_single_channel_decide(&c) == NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "missing swizzle -> luminance-alpha fallback");
  /* ES3 but no TexStorage -> fallback. */
  c = caps(1, 0, 1, 1);
  check(nxgl_single_channel_decide(&c) == NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "missing tex_storage -> fallback");
  /* ES3 but no GL_RED -> fallback. */
  c = caps(1, 1, 1, 0);
  check(nxgl_single_channel_decide(&c) == NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "missing GL_RED -> fallback");
  /* Plain GLES2 -> fallback. */
  c = caps(0, 0, 0, 0);
  check(nxgl_single_channel_decide(&c) == NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "gles2 -> fallback");
  /* Invalid/unmeasured caps cannot authorize a rewrite. */
  check(nxgl_single_channel_decide(NULL) == NXGL_SC_ROUTE_PASSTHROUGH,
        "null caps -> passthrough");
  c = caps(1, 1, 1, 1);
  c.api_version = 1u;
  check(nxgl_single_channel_decide(&c) == NXGL_SC_ROUTE_PASSTHROUGH,
        "stale capability API -> passthrough");

  /* Per-texture coherence: equal routes ok, MIXED is the empty-atlas bug. */
  check(nxgl_single_channel_coherent(NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
                                     NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == 1,
        "native/native coherent");
  check(nxgl_single_channel_coherent(NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
                                     NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP) == 1,
        "la/la coherent");
  check(nxgl_single_channel_coherent(NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
                                     NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP) == 0,
        "immutable R8 store + LA upload is INCOHERENT (empty-atlas bug)");
  check(nxgl_single_channel_coherent(NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
                                     NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == 0,
        "LA store + native upload is incoherent");

  /* Names + receipt. */
  check(strcmp(nxgl_single_channel_route_name(NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE),
               "native-r8-swizzle") == 0, "native name");
  check(strcmp(nxgl_single_channel_route_name(NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP),
               "luminance-alpha-dup") == 0, "fallback name");

  c = caps(0, 0, 0, 0);
  check(nxgl_single_channel_receipt(&c, NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
                                    buf, sizeof buf) > 0, "receipt formats");
  check(strcmp(buf,
               "SINGLE-CHANNEL: route=luminance-alpha-dup es3=0 tex_storage=0 "
               "swizzle=0 red=0") == 0, "receipt text");
  check(nxgl_single_channel_receipt(&c, NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
                                    buf, 8) == 0 && buf[0] == '\0',
        "short buffer -> empty");

  /* ---- item 6: classify formats. Single-channel is rewritten; RGBA never. */
  {
    /* GL enums. */
    const unsigned GL_RED = 0x1903u, GL_R8 = 0x8229u, GL_ALPHA = 0x1906u,
                   GL_ALPHA8 = 0x803Cu, GL_LUMINANCE = 0x1909u,
                   GL_LUMINANCE_ALPHA = 0x190Au, GL_RGBA = 0x1908u,
                   GL_RGBA8 = 0x8058u, GL_RG = 0x8227u,
                   GL_UNSIGNED_BYTE = 0x1401u;
    (void)GL_UNSIGNED_BYTE;
    check(nxgl_single_channel_classify(GL_R8, GL_RED) == NXGL_SC_KIND_RED,
          "R8/RED -> RED");
    check(nxgl_single_channel_classify(GL_ALPHA8, GL_ALPHA) ==
              NXGL_SC_KIND_ALPHA, "ALPHA8/ALPHA -> ALPHA");
    check(nxgl_single_channel_classify(GL_LUMINANCE, GL_LUMINANCE) ==
              NXGL_SC_KIND_LUMINANCE, "LUMINANCE -> LUMINANCE");
    check(nxgl_single_channel_classify(GL_RGBA8, GL_RGBA) == NXGL_SC_KIND_RGBA,
          "RGBA8/RGBA -> RGBA (never touched)");
    check(nxgl_single_channel_classify(GL_RG, GL_RG) == NXGL_SC_KIND_RGBA,
          "RG -> RGBA class (multi-channel, untouched)");
    /* A contradictory pair is rejected, not collapsed into an innocuous
     * multi-channel class. */
    check(nxgl_single_channel_classify(GL_R8, GL_RGBA) ==
              NXGL_SC_KIND_INVALID,
          "mismatched R8 internal + RGBA format -> invalid");
    check(nxgl_single_channel_classify(GL_ALPHA8, GL_LUMINANCE) ==
              NXGL_SC_KIND_INVALID,
          "ALPHA/LUMINANCE mismatch -> invalid");

    /* ---- plan: native swizzles R8/RED; fallback duplicates into LA. */
    nxgl_sc_upload_plan plan;
    check(nxgl_single_channel_plan(NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
                                   NXGL_SC_KIND_RED, &plan) == 1 &&
              plan.handled == 1 && plan.internalformat == GL_R8 &&
              plan.format == GL_RED && plan.duplicate_byte == 0 &&
              plan.apply_swizzle == 1 && plan.swizzle_r == GL_RED &&
              plan.swizzle_g == 0u && plan.swizzle_b == 0u &&
              plan.swizzle_a == 1u,
          "native RED preserves (r,0,0,1) with a complete swizzle");
    check(nxgl_single_channel_plan(NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
                                   NXGL_SC_KIND_RED, &plan) == 0 &&
              plan.reject == 1 && plan.handled == 0,
          "fallback cannot pretend LA preserves RED semantics");
    check(nxgl_single_channel_plan_v2(
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP, NXGL_SC_KIND_RED,
              NXGL_SC_SEMANTIC_ALPHA_MASK, &plan) == 1 &&
              plan.handled == 1 &&
              plan.internalformat == GL_LUMINANCE_ALPHA &&
              plan.format == GL_LUMINANCE_ALPHA && plan.duplicate_byte == 1 &&
              plan.transform == NXGL_SC_TRANSFORM_MASK_TO_LA,
          "explicit RED alpha-mask fallback becomes (255,coverage)");
    check(nxgl_single_channel_plan_v2(
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE, NXGL_SC_KIND_RED,
              NXGL_SC_SEMANTIC_ALPHA_MASK, &plan) == 1 &&
              plan.internalformat == GL_R8 && plan.format == GL_RED &&
              plan.transform == NXGL_SC_TRANSFORM_NONE &&
              plan.apply_swizzle == 1 && plan.swizzle_r == 1u &&
              plan.swizzle_g == 1u && plan.swizzle_b == 1u &&
              plan.swizzle_a == GL_RED,
          "alpha-mask native R8/RED swizzle stays literal");
    check(nxgl_single_channel_plan_v2(
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP, NXGL_SC_KIND_RED,
              NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT, &plan) == 1 &&
              plan.handled == 1 &&
              plan.internalformat == GL_LUMINANCE_ALPHA &&
              plan.format == GL_LUMINANCE_ALPHA && plan.duplicate_byte == 1 &&
              plan.transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP,
          "explicit RED coverage fallback becomes (coverage,coverage)");
    check(nxgl_single_channel_plan_v2(
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE, NXGL_SC_KIND_RED,
              NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT, &plan) == 1 &&
              plan.internalformat == GL_R8 && plan.format == GL_RED &&
              plan.transform == NXGL_SC_TRANSFORM_NONE &&
              plan.apply_swizzle == 1 && plan.swizzle_r == 1u &&
              plan.swizzle_g == 1u && plan.swizzle_b == 1u &&
              plan.swizzle_a == GL_RED,
          "RED coverage native route keeps R8/RED alpha-mask swizzle");
    check(nxgl_single_channel_plan_v2(
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP, NXGL_SC_KIND_ALPHA,
              NXGL_SC_SEMANTIC_PRESERVE, &plan) == 1 &&
              plan.transform == NXGL_SC_TRANSFORM_ALPHA_TO_LA,
          "ALPHA fallback preserves (1,1,1,a)");
    check(nxgl_single_channel_plan_v2(
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP, NXGL_SC_KIND_LUMINANCE,
              NXGL_SC_SEMANTIC_PRESERVE, &plan) == 1 &&
              plan.transform == NXGL_SC_TRANSFORM_LUMINANCE_TO_LA,
          "LUMINANCE fallback preserves (l,l,l,1)");
    check(nxgl_single_channel_plan(NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
                                   NXGL_SC_KIND_RGBA, &plan) == 1 &&
              plan.handled == 0,
          "RGBA plan: pass through, not rewritten");
    check(nxgl_single_channel_plan(NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
                                   NXGL_SC_KIND_RED, NULL) == 0,
          "plan NULL out -> 0");
  }

  /* ---- item 6: per-texture tracker catches mixed routes (empty-atlas bug). */
  {
    static nxgl_sc_tracker tracker;
    check(nxgl_single_channel_tracker_reset(&tracker) == 0, "tracker reset");
    check(nxgl_single_channel_tracker_reset(NULL) == -1, "reset NULL -> -1");
    /* Same route for store then upload on texture 7 is coherent. */
    check(nxgl_single_channel_tracker_note(&tracker, 7u, 1,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == NXGL_SC_NOTE_COHERENT,
          "tex7 native store coherent");
    check(nxgl_single_channel_tracker_note(&tracker, 7u, 0,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == NXGL_SC_NOTE_COHERENT,
          "tex7 native upload coherent");
    /* Texture 9: immutable R8 store then a LUMINANCE_ALPHA upload = the bug. */
    check(nxgl_single_channel_tracker_note(&tracker, 9u, 1,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == NXGL_SC_NOTE_COHERENT,
          "tex9 native store coherent");
    check(nxgl_single_channel_tracker_note(&tracker, 9u, 0,
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP) == NXGL_SC_NOTE_MIXED,
          "tex9 native store + LA upload = MIXED (empty-atlas bug)");
    /* Texture 9 is independent from texture 7 (per-id state). */
    check(nxgl_single_channel_tracker_note(&tracker, 7u, 0,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == NXGL_SC_NOTE_COHERENT,
          "tex7 still coherent after tex9 mixed");
    /* Forget + reuse the id starts clean (no false MIXED across a delete). */
    check(nxgl_single_channel_tracker_forget(&tracker, 9u) == 1,
          "forget tex9");
    check(nxgl_single_channel_tracker_forget(&tracker, 9u) == 0,
          "forget again -> 0");
    check(nxgl_single_channel_tracker_note(&tracker, 9u, 1,
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP) == NXGL_SC_NOTE_COHERENT,
          "reused tex9 LA store is clean after forget");
    check(nxgl_single_channel_tracker_note(&tracker, 9u, 0,
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP) == NXGL_SC_NOTE_COHERENT,
          "reused tex9 LA upload coherent");
    /* Overflow is fail-safe (never a silent COHERENT past capacity). */
    {
      unsigned id;
      nxgl_sc_note_result last = NXGL_SC_NOTE_COHERENT;
      nxgl_single_channel_tracker_reset(&tracker);
      for (id = 1u; id <= NXGL_SC_TRACKER_CAPACITY + 4u; id++) {
        last = nxgl_single_channel_tracker_note(&tracker, id, 1,
                                                NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE);
        if (last == NXGL_SC_NOTE_OVERFLOW) {
          break;
        }
      }
      check(last == NXGL_SC_NOTE_OVERFLOW, "tracker overflow is reported");
    }
    check(nxgl_single_channel_tracker_note(NULL, 1u, 1,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE) == NXGL_SC_NOTE_OVERFLOW,
          "note NULL tracker -> overflow (fail safe)");

    nxgl_single_channel_tracker_reset(&tracker);
    check(nxgl_single_channel_tracker_note_v2(
              &tracker, (uintptr_t)10u, 0x0DE1u, 3u, 1,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE, NXGL_SC_KIND_ALPHA,
              NXGL_SC_SEMANTIC_PRESERVE) == NXGL_SC_NOTE_COHERENT,
          "v2 tracker records context/target/kind/semantic");
    check(nxgl_single_channel_tracker_note_v2(
              &tracker, (uintptr_t)11u, 0x0DE1u, 3u, 0,
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP, NXGL_SC_KIND_ALPHA,
              NXGL_SC_SEMANTIC_PRESERVE) == NXGL_SC_NOTE_COHERENT,
          "same numeric id in another context is independent");
    check(nxgl_single_channel_tracker_note_v2(
              &tracker, (uintptr_t)10u, 0x0DE1u, 3u, 0,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE, NXGL_SC_KIND_LUMINANCE,
              NXGL_SC_SEMANTIC_PRESERVE) == NXGL_SC_NOTE_MIXED,
          "kind cannot change inside one texture object");
    nxgl_single_channel_tracker_forget_context(&tracker, (uintptr_t)10u);
    check(nxgl_single_channel_tracker_note_v2(
              &tracker, (uintptr_t)10u, 0x0DE1u, 3u, 0,
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE, NXGL_SC_KIND_LUMINANCE,
              NXGL_SC_SEMANTIC_PRESERVE) == NXGL_SC_NOTE_COHERENT,
          "context loss clears only that context's object state");
  }

  (void)puts("nxgl_single_channel tests: ok "
             "(decide/coherence/receipt/classify/plan/tracker)");
  return 0;
}
