/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-GRAPHICS-04 pure state machine gates. No SDL, no GL, no clock: every
 * number, including monotonic time, is injected. */
#include "nxgl_graphics_present_gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, name)                                     \
  do {                                                        \
    if (cond) {                                               \
      printf("ok %s\n", name);                                \
    } else {                                                  \
      printf("FAIL %s (line %d)\n", name, __LINE__);          \
      g_failures++;                                           \
    }                                                         \
  } while (0)

static void make_contract(nxgl_graphics_contract *c) {
  (void)nxgl_graphics_contract_default(c);
  c->api = NXGL_GRAPHICS_API_GLES;
  c->profile = NXGL_GRAPHICS_PROFILE_ES;
  c->version_major = 3;
  c->version_minor = 0;
  c->version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
  c->shader_dialect = NXGL_SHADER_DIALECT_ESSL300;
  c->drawable_ready_timeout_ms = 8000;
}

/* A fully measured, matching pre-present evidence: live GLES 3.1 context,
 * providers recorded, shader probe PASS, drawable at the given size. */
static void make_measured(nxgl_graphics_evidence *ev, int w, int h) {
  (void)nxgl_graphics_evidence_init(ev);
  (void)snprintf(ev->run_id, sizeof ev->run_id, "porttest-1-2-3");
  (void)snprintf(ev->generation, sizeof ev->generation, "gen-abc");
  (void)snprintf(ev->commit, sizeof ev->commit, "commit-abc");
  (void)snprintf(ev->port_id, sizeof ev->port_id, "fixtureport");
  (void)snprintf(ev->port_version, sizeof ev->port_version, "1.0.0");
  (void)snprintf(ev->gles_provider, sizeof ev->gles_provider,
                 "/fake/libGLESv2.so");
  (void)snprintf(ev->egl_provider, sizeof ev->egl_provider, "/fake/libEGL.so");
  (void)snprintf(ev->dso_build_id, sizeof ev->dso_build_id, "aa11");
  (void)snprintf(ev->egl_build_id, sizeof ev->egl_build_id, "bb22");
  ev->sdl_major = 2;
  ev->obtained.api = NXGL_GRAPHICS_API_GLES;
  ev->obtained.profile = NXGL_GRAPHICS_PROFILE_ES;
  ev->obtained.version_major = 3;
  ev->obtained.version_minor = 1;
  ev->drawable_w = w;
  ev->drawable_h = h;
  ev->shader_probe = NXGL_SHADER_PROBE_PASS;
}

/* Preflight to AWAITING on tokens (7, 9); asserts internally. */
static void arm_gate(nxgl_graphics_present_gate *gate,
                     const nxgl_graphics_contract *contract, int pre_w,
                     int pre_h) {
  nxgl_graphics_evidence measured;
  nxgl_graphics_gate_result result;
  nxgl_graphics_gate_status status;
  (void)nxgl_graphics_present_gate_init(gate);
  (void)nxgl_graphics_gate_result_init(&result);
  make_measured(&measured, pre_w, pre_h);
  status = nxgl_graphics_present_gate_preflight(gate, contract, &measured, 7,
                                                9, &result);
  if (status != NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT) {
    printf("FAIL arm_gate could not reach AWAITING\n");
    g_failures++;
  }
}

static nxgl_graphics_gate_status observe(
    nxgl_graphics_present_gate *gate, const nxgl_graphics_contract *contract,
    int w, int h, int read_ok, int64_t now,
    nxgl_graphics_gate_result *result, char *receipt, size_t cap) {
  return nxgl_graphics_present_gate_observe(
      gate, contract, 7, 9, w, h, read_ok, "/fake/libGLESv2.so",
      "/fake/libEGL.so", now, result, receipt, cap);
}

int main(void) {
  nxgl_graphics_contract contract;
  nxgl_graphics_present_gate gate;
  nxgl_graphics_evidence measured;
  nxgl_graphics_gate_result result;
  nxgl_graphics_gate_status status;
  char receipt[2048];
  char line[256];

  make_contract(&contract);
  (void)nxgl_graphics_gate_result_init(&result);

  /* 1. An all-zero (UNINITIALIZED) gate refuses everything, including
   * after_present, with no crash and no promotion. */
  memset(&gate, 0, sizeof gate);
  status = observe(&gate, &contract, 640, 480, 1, 100, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED,
        "uninitialized gate refuses after_present");
  memset(&gate, 0, sizeof gate);
  gate.api_version = NXGL_GRAPHICS_PRESENT_GATE_API_VERSION;
  gate.struct_size = sizeof gate; /* zeroed phase == UNINITIALIZED */
  status = observe(&gate, &contract, 640, 480, 1, 100, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_MISUSE,
        "phase UNINITIALIZED is misuse, fail closed");

  /* 2. Valid context + shader + 1x1 drawable -> AWAITING immediately, no
   * receipt, reason OK is NOT approval (status is the authority). */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 1, 1);
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
            result.status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
            result.reason == NXGL_GRAPHICS_OK && gate.receipt_emitted == 0,
        "1x1 pre-present is AWAITING, never OK, no receipt");
  CHECK(nxgl_graphics_present_gate_prepresent_line(&gate, line, sizeof line) >
            0 &&
            strstr(line, "GRAPHICS-PREPRESENT-EVIDENCE:") == line &&
            strstr(line, "state=awaiting-first-present") != NULL &&
            strstr(line, "final=0") != NULL &&
            strstr(line, "drawable=1x1") != NULL,
        "pre-present diagnostic line is explicit and non-final");

  /* 3. A drawable already large at preflight is STILL only pending. */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 640, 480);
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
            gate.receipt_emitted == 0,
        "640x480 pre-present is still pending, not proof");

  /* 4. Desktop GL for a GLES contract fails BEFORE the pending state. */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 1, 1);
  measured.obtained.api = NXGL_GRAPHICS_API_GL;
  measured.obtained.profile = NXGL_GRAPHICS_PROFILE_COMPAT;
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
        "desktop GL for GLES contract is terminal pre-present");

  /* 5a. Version below minimum fails before pending. */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 1, 1);
  measured.obtained.version_major = 2;
  measured.obtained.version_minor = 0;
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_VERSION_BELOW_MINIMUM,
        "version below minimum is terminal pre-present");

  /* 5b. Shader probe that did not PASS fails before pending; SKIPPED is not
   * a pass. */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 1, 1);
  measured.shader_probe = NXGL_SHADER_PROBE_SKIPPED;
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_SHADER_PROBE_FAILED,
        "shader SKIPPED is not a pass pre-present");

  /* 5c. Dead provider (all-zero obtained sentinel) is provider-nominal-only. */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 1, 1);
  memset(&measured.obtained, 0, sizeof measured.obtained);
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY,
        "zero-sentinel obtained is provider-nominal-only");

  /* 6. Happy path: 1x1 -> 640x480 after the first present. One PROVED, one
   * receipt carrying the post-present extension fields. */
  arm_gate(&gate, &contract, 1, 1);
  status = observe(&gate, &contract, 1, 1, 1, 1000, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
            receipt[0] == '\0',
        "first present with 1x1 stays pending inside the budget");
  status = observe(&gate, &contract, 640, 480, 1, 1200, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_PROVED &&
            result.reason == NXGL_GRAPHICS_OK,
        "usable drawable after real present is PROVED");
  CHECK(strstr(receipt, "GRAPHICS-EVIDENCE:") == receipt &&
            strstr(receipt, "verdict=OK") != NULL &&
            strstr(receipt, "reason=ok") != NULL &&
            strstr(receipt, "drawable=640x480") != NULL &&
            strstr(receipt, "phase=post-first-present") != NULL &&
            strstr(receipt, "first_present=1") != NULL &&
            strstr(receipt, "pre_drawable=1x1") != NULL &&
            strstr(receipt, "shader_probe=pass") != NULL &&
            strstr(receipt, "run_id=porttest-1-2-3") != NULL &&
            strstr(receipt, "generation=gen-abc") != NULL &&
            strstr(receipt, "commit=commit-abc") != NULL &&
            strstr(receipt, "port_id=fixtureport") != NULL &&
            strstr(receipt, "port_version=1.0.0") != NULL &&
            strstr(receipt, "provider_gles=/fake/libGLESv2.so") != NULL &&
            strstr(receipt, "build_id=aa11") != NULL &&
            strstr(receipt, "egl_build_id=bb22") != NULL &&
            strstr(receipt, "sdl=2") != NULL &&
            strstr(receipt, "requested=gles/es/3.0/minimum") != NULL &&
            strstr(receipt, "obtained=gles/es/3.1") != NULL,
        "final receipt carries verdict, identities and phase fields");

  /* 7. PROVED is stable and one-shot: no second receipt ever. */
  status = observe(&gate, &contract, 640, 480, 1, 1300, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_PROVED && receipt[0] == '\0',
        "later presents keep PROVED with no new receipt");

  /* 8. The deadline starts at the FIRST present and never restarts: repeated
   * presents do not extend it, and 1x1 fails exactly at the deadline. */
  arm_gate(&gate, &contract, 1, 1);
  status = observe(&gate, &contract, 1, 1, 1, 5000, &result, receipt,
                   sizeof receipt); /* deadline = 13000 */
  status = observe(&gate, &contract, 1, 1, 1, 12999, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT,
        "one ms before the deadline is still pending");
  status = observe(&gate, &contract, 1, 1, 1, 13000, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_DRAWABLE_STUCK_1X1,
        "persistent 1x1 fails exactly at the monotonic deadline");
  CHECK(strstr(receipt, "verdict=FAIL") != NULL &&
            strstr(receipt, "reason=drawable-stuck-1x1") != NULL &&
            strstr(receipt, "phase=post-first-present") != NULL,
        "post-present rejection leaves a FAIL diagnostic receipt");
  status = observe(&gate, &contract, 640, 480, 1, 14000, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED && receipt[0] == '\0',
        "REJECTED is terminal: no hidden recovery, no second receipt");

  /* 9. 0x0, unreadable size and absurd size fail closed with stable reasons. */
  arm_gate(&gate, &contract, 1, 1);
  status = observe(&gate, &contract, 0, 0, 1, 100, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_DRAWABLE_UNREADABLE,
        "0x0 post-present is drawable-unreadable, never success");
  arm_gate(&gate, &contract, 1, 1);
  status = observe(&gate, &contract, 0, 0, 0, 100, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_DRAWABLE_UNREADABLE,
        "an unreadable size symbol is drawable-unreadable");
  arm_gate(&gate, &contract, 1, 1);
  status = observe(&gate, &contract, 1000000, 480, 1, 100, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_DRAWABLE_ABSURD,
        "an absurd dimension is drawable-absurd, never 1x1 nor success");

  /* 10. Token changes between phases fail closed. */
  arm_gate(&gate, &contract, 1, 1);
  status = nxgl_graphics_present_gate_observe(
      &gate, &contract, 8, 9, 640, 480, 1, "/fake/libGLESv2.so",
      "/fake/libEGL.so", 100, &result, receipt, sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_IDENTITY_CHANGED,
        "window token change is terminal");
  arm_gate(&gate, &contract, 1, 1);
  status = nxgl_graphics_present_gate_observe(
      &gate, &contract, 7, 10, 640, 480, 1, "/fake/libGLESv2.so",
      "/fake/libEGL.so", 100, &result, receipt, sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_IDENTITY_CHANGED,
        "context token change is terminal");

  /* 11. Provider swap between phases fails closed. */
  arm_gate(&gate, &contract, 1, 1);
  status = nxgl_graphics_present_gate_observe(
      &gate, &contract, 7, 9, 640, 480, 1, "/other/libGLESv2.so",
      "/fake/libEGL.so", 100, &result, receipt, sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_IDENTITY_CHANGED,
        "GLES provider swap is terminal");

  /* 12. A broken monotonic clock fails closed. */
  arm_gate(&gate, &contract, 1, 1);
  status = observe(&gate, &contract, 640, 480, 1, -1, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_CLOCK_UNAVAILABLE,
        "an unreadable clock cannot bound the deadline");

  /* 13. Reset invalidates every prior proof: PROVED gate reset to READY needs
   * a fresh preflight; observe right after reset is misuse. */
  arm_gate(&gate, &contract, 1, 1);
  (void)observe(&gate, &contract, 640, 480, 1, 100, &result, receipt,
                sizeof receipt);
  nxgl_graphics_present_gate_reset(&gate);
  status = observe(&gate, &contract, 640, 480, 1, 200, &result, receipt,
                   sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_MISUSE,
        "reset invalidates the proof; observe without preflight is misuse");

  /* 14. Preflight on a non-READY gate is misuse (double preflight). */
  arm_gate(&gate, &contract, 1, 1);
  make_measured(&measured, 1, 1);
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_MISUSE,
        "a second preflight on the same gate is misuse");

  /* 15. Malformed structures fail without side effects. */
  (void)nxgl_graphics_present_gate_init(&gate);
  make_measured(&measured, 1, 1);
  measured.api_version = 99;
  status = nxgl_graphics_present_gate_preflight(&gate, &contract, &measured, 7,
                                                9, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
            result.reason == NXGL_GRAPHICS_GATE_MISUSE,
        "a mis-versioned measured evidence is refused");
  {
    nxgl_graphics_contract bad;
    make_contract(&bad);
    bad.struct_size = 1;
    (void)nxgl_graphics_present_gate_init(&gate);
    make_measured(&measured, 1, 1);
    status = nxgl_graphics_present_gate_preflight(&gate, &bad, &measured, 7, 9,
                                                  &result);
    CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
              result.reason == NXGL_GRAPHICS_CONTRACT_INVALID,
          "a malformed contract is refused before any wait");
  }

  /* 16. A discardable probe gate never authorizes the real context: the real
   * pair gets its own gate and its own preflight. */
  {
    nxgl_graphics_present_gate probe_gate, real_gate;
    arm_gate(&probe_gate, &contract, 1, 1);
    (void)observe(&probe_gate, &contract, 640, 480, 1, 100, &result, receipt,
                  sizeof receipt); /* probe concluded */
    (void)nxgl_graphics_present_gate_init(&real_gate);
    status = nxgl_graphics_present_gate_observe(
        &real_gate, &contract, 21, 22, 640, 480, 1, "/fake/libGLESv2.so",
        "/fake/libEGL.so", 200, &result, receipt, sizeof receipt);
    CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
              result.reason == NXGL_GRAPHICS_GATE_MISUSE,
        "a probe gate's conclusion never authorizes the real context");
  }

  /* 17. The legacy validator still refuses 1x1 exactly as before. */
  CHECK(nxgl_graphics_drawable_usable(1, 1) == 0 &&
            nxgl_graphics_drawable_usable(0, 0) == 0 &&
            nxgl_graphics_drawable_usable(640, 1) == 0 &&
            nxgl_graphics_drawable_usable(640, 480) == 1,
        "nxgl_graphics_drawable_usable(1,1) stays false");

  if (g_failures != 0) {
    printf("test_v4_graphics_present_gate: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_v4_graphics_present_gate: ALL PASS\n");
  return 0;
}
