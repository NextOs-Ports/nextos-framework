/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static nxgl_surface_observation_v2 observation(
    nxgl_surface_event_v2 event) {
  nxgl_surface_observation_v2 value;
  memset(&value, 0, sizeof(value));
  value.api_version = NXGL_API_VERSION_V2;
  value.struct_size = sizeof(value);
  value.event = event;
  value.window_width = 640;
  value.window_height = 480;
  value.drawable_width = 1280;
  value.drawable_height = 720;
  return value;
}

static void test_observations_are_monotonic_and_passive(void) {
  nxgl_surface_state_v2 state;
  nxgl_surface_observation_v2 event;
  nxgl_surface_state_v2 before_recreate;
  nxgl_surface_state_v2_init(&state);
  CHECK(state.api_version == NXGL_API_VERSION_V2);
  CHECK(state.generation == 0u);
  CHECK(state.context_generation == 1u);
  CHECK(state.focused == 1 && state.minimized == 0 && state.context_lost == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_RESIZED);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 1u);
  CHECK(state.window_width == 640 && state.drawable_width == 1280);

  event = observation(NXGL_SURFACE_EVENT_V2_FOCUS_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 2u && state.focused == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_MINIMIZED);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 3u && state.minimized == 1 && state.focused == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 4u && state.context_lost == 1);
  CHECK(state.context_generation == 1u);
  memcpy(&before_recreate, &state, sizeof(before_recreate));

  /* Observing loss performs no native recreation.  Only an explicit, later
   * CONTEXT_RECREATED observation advances the context generation. */
  event = observation(NXGL_SURFACE_EVENT_V2_RESTORED);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.context_lost == 1 && state.context_generation == 1u);
  CHECK(state.generation == before_recreate.generation + 1u);

  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED);
  event.drawable_width = 1920;
  event.drawable_height = 1080;
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.context_lost == 0 && state.context_generation == 2u);
  CHECK(state.generation == 6u);
  CHECK(state.drawable_width == 1920 && state.drawable_height == 1080);
}

static void test_failed_observations_are_atomic(void) {
  nxgl_surface_state_v2 state;
  nxgl_surface_state_v2 before;
  nxgl_surface_observation_v2 event;
  nxgl_surface_state_v2_init(&state);
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_RESIZED);
  event.drawable_height = 0;
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  state.focused = 2;
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_FOCUS_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  state.context_generation = 3u;
  memcpy(&before, &state, sizeof(before));
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  state.window_width = 640;
  memcpy(&before, &state, sizeof(before));
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  state.generation = UINT64_MAX;
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_FOCUS_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  state.context_generation = UINT64_MAX;
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_RESIZED);
  event.drawable_width = NXGL_SURFACE_DIMENSION_MAX + 1;
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);
}

static nxgl_silhouette_observation_v2 black_silhouette(void) {
  nxgl_silhouette_observation_v2 value;
  memset(&value, 0, sizeof(value));
  value.api_version = NXGL_API_VERSION_V2;
  value.struct_size = sizeof(value);
  value.pixels_are_black = 1;
  value.silhouette_is_intact = 1;
  return value;
}

static void test_silhouette_prioritizes_sampler_wrap_atlas(void) {
  nxgl_silhouette_observation_v2 value = black_silhouette();
  nxgl_silhouette_diagnosis_v2 diagnosis =
      NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE;
  /* Even before evidence collection, a perfect black silhouette starts with
   * sampler/wrap/atlas rather than shader or lighting speculation. */
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_SAMPLER_WRAP_ATLAS);

  value.uses_texture_atlas = 1;
  value.repeating_or_mirrored_uv = 1;
  value.forced_clamp_to_edge = 1;
  value.sampler_override_active = 1;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_SAMPLER_WRAP_ATLAS);

  value.silhouette_is_intact = 0;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE);

  value.pixels_are_black = 0;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_NOT_APPLICABLE);
}

static void test_silhouette_validation_does_not_overwrite_result(void) {
  nxgl_silhouette_observation_v2 value = black_silhouette();
  nxgl_silhouette_diagnosis_v2 diagnosis =
      NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE;
  value.forced_clamp_to_edge = 2;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE);
}

static void test_provider_name_prefilter_is_transport_aware(void) {
  /* ArkOS/AeUX regression: this is a GBM-capable object despite carrying an
   * auxiliary Wayland token.  A blanket strstr("wayland") rejection loses the
   * only provider that matches the live Mali kernel ABI. */
  CHECK(nxgl_provider_name_compatible(
      "kmsdrm", "libmali-bifrost-g31-rxp0-wayland-gbm.so"));
  CHECK(nxgl_provider_name_compatible(
      NULL, "libmali-bifrost-g31-rxp0-wayland-gbm.so"));
  CHECK(nxgl_provider_name_compatible("kmsdrm", "libmali-utgard-gbm.so"));
  CHECK(nxgl_provider_name_compatible("kmsdrm", "libMali.so"));
  CHECK(!nxgl_provider_name_compatible("kmsdrm", "libmali-wayland.so"));
  CHECK(!nxgl_provider_name_compatible("kmsdrm", "libmali-x11.so"));

  CHECK(nxgl_provider_name_compatible("wayland", "libmali-wayland.so"));
  CHECK(!nxgl_provider_name_compatible("wayland", "libmali-x11.so"));
  CHECK(nxgl_provider_name_compatible("x11", "libmali-x11.so"));
  CHECK(!nxgl_provider_name_compatible("x11", "libmali-wayland.so"));

  CHECK(!nxgl_provider_name_compatible("kmsdrm", "libmali-dummy-gbm.so"));
  CHECK(!nxgl_provider_name_compatible("wayland", "libmali-stub.so"));
  CHECK(!nxgl_provider_name_compatible("kmsdrm", "libmali-headless.so"));
  CHECK(!nxgl_provider_name_compatible("kmsdrm", NULL));

  /* 0.2.14: so' o BASENAME classifica -- o nome chega como caminho real e um
   * diretorio "stubs"/"drm" no meio nao pode vetar um blob bom nem promover
   * um objeto qualquer. */
  CHECK(nxgl_provider_name_compatible(
      "kmsdrm", "/usr/lib/stubs/libmali-utgard-gbm.so"));
  CHECK(!nxgl_provider_name_compatible(
      "kmsdrm", "/usr/lib/drm/libmali-wayland.so"));
  CHECK(!nxgl_provider_name_compatible(
      "wayland", "/opt/surfaceless/libmali-stub.so"));
  CHECK(nxgl_provider_name_compatible(
      "wayland", "/usr/lib/x11-compat/libmali-wayland.so"));
}

static void test_sdl_provider_pair_requires_complete_failure_proof(void) {
  const char *provider = "libmali-bifrost-g31-rxp0-gbm.so";

  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, provider, 1, 1, 1,
                                    1, 1) ==
        NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", "Mali-G31", provider, 1, 1,
                                    1, 1, 1) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, provider, 1, 1, 0,
                                    1, 1) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, provider, 1, 1, 1,
                                    0, 1) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, provider, 1, 1, 1,
                                    1, 0) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, "libmali-wayland.so",
                                    1, 1, 1, 1, 1) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, "libmali-dummy-gbm.so",
                                    1, 1, 1, 1, 1) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  CHECK(nxgl_plan_sdl_provider_pair("kmsdrm", NULL, provider, 2, 1, 1,
                                    1, 1) ==
        NXGL_SDL_PROVIDER_PAIR_INVALID);
}

static void test_precontext_recovery_is_one_shot_and_fail_closed(void) {
  nxgl_sdl_precontext_recovery_v2 recovery;
  nxgl_sdl_precontext_recovery_v2_init(&recovery);
  CHECK(recovery.api_version == NXGL_API_VERSION_V2);
  recovery.video_backend = "KMSDRM";
  recovery.provider_name = "libMali.so";
  recovery.failed_stage = NXGL_OPEN_STAGE_V2_CONTEXT_CREATE;
  recovery.final_reason = NXGL_OPEN_REASON_V2_CONTEXT_FAILED;
  recovery.attempts_exhausted = 1;
  recovery.same_object = 1;
  recovery.exports_egl = 1;
  recovery.exports_engine_gles = 1;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT);

  recovery.inherited_provider_hint = 1;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  recovery.inherited_provider_hint = 0;
  recovery.same_object = 0;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  recovery.same_object = 1;
  recovery.failed_stage = NXGL_OPEN_STAGE_V2_WINDOW_CREATE;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  recovery.final_reason = NXGL_OPEN_REASON_V2_WINDOW_FAILED;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT);
  recovery.final_reason = NXGL_OPEN_REASON_V2_CONTEXT_FAILED;
  recovery.failed_stage = NXGL_OPEN_STAGE_V2_CONTEXT_CREATE;
  recovery.provider_name = "libmali-wayland.so";
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_NO_ACTION);
  recovery.provider_name = "libMali.so";
  recovery.exports_egl = 2;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_INVALID);
  recovery.exports_egl = 1;
  recovery.struct_size = 0u;
  CHECK(nxgl_plan_sdl_precontext_recovery_v2(&recovery) ==
        NXGL_SDL_PROVIDER_PAIR_INVALID);
}

static void test_frame_proof_never_passes_without_evidence(void) {
  nxgl_frame_proof_observation_v2 proof;
  nxgl_frame_proof_verdict_v2 verdict;
  memset(&proof, 0, sizeof(proof));
  proof.api_version = NXGL_API_VERSION_V2;
  proof.struct_size = sizeof(proof);

  /* No sample taken: silence must not read as success. */
  proof.samples = 0;
  proof.best_non_black_percent = 0.0;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) == NXGL_SUCCESS);
  CHECK(verdict == NXGL_FRAME_PROOF_V2_UNKNOWN);

  /* Measured and empty: this is the black screen that used to exit 0. */
  proof.samples = 3;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) == NXGL_SUCCESS);
  CHECK(verdict == NXGL_FRAME_PROOF_V2_BLACK);

  /* The best sample decides, so one late frame with content is enough. */
  proof.best_non_black_percent = 78.6;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) == NXGL_SUCCESS);
  CHECK(verdict == NXGL_FRAME_PROOF_V2_OK);

  /* Exactly at the default threshold still counts as drawn. */
  proof.best_non_black_percent = NXGL_FRAME_PROOF_DEFAULT_MIN_NON_BLACK;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) == NXGL_SUCCESS);
  CHECK(verdict == NXGL_FRAME_PROOF_V2_OK);

  /* A caller may demand more than the default. */
  proof.minimum_non_black_percent = 10.0;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) == NXGL_SUCCESS);
  CHECK(verdict == NXGL_FRAME_PROOF_V2_BLACK);

  /* Out-of-range and short structs are rejected, not guessed. */
  proof.minimum_non_black_percent = 0.0;
  proof.best_non_black_percent = 100.1;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  proof.best_non_black_percent = 50.0;
  proof.samples = -1;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  proof.samples = 1;
  proof.struct_size = 0u;
  CHECK(nxgl_classify_frame_proof_v2(&proof, &verdict) ==
        NXGL_ERROR_INVALID_ARGUMENT);
}

static void test_black_only_accuses_the_port_when_the_launch_could_draw(void) {
  nxgl_launch_observation_v2 launch;
  nxgl_launch_context_v2 context;
  int conclusive = -1;
  memset(&launch, 0, sizeof(launch));
  launch.api_version = NXGL_API_VERSION_V2;
  launch.struct_size = sizeof(launch);

  /* Over SSH the window may never open for reasons unrelated to the port. */
  launch.remote_session = 1;
  CHECK(nxgl_classify_launch_context_v2(&launch, &context) == NXGL_SUCCESS);
  CHECK(context == NXGL_LAUNCH_CONTEXT_V2_REMOTE);
  CHECK(nxgl_frame_proof_is_conclusive_v2(context, &conclusive) ==
        NXGL_SUCCESS);
  CHECK(conclusive == 0);

  /* The frontend owns the display, so it wins over a parallel SSH login. */
  launch.frontend_launched = 1;
  CHECK(nxgl_classify_launch_context_v2(&launch, &context) == NXGL_SUCCESS);
  CHECK(context == NXGL_LAUNCH_CONTEXT_V2_FRONTEND);
  CHECK(nxgl_frame_proof_is_conclusive_v2(context, &conclusive) ==
        NXGL_SUCCESS);
  CHECK(conclusive == 1);

  /* A real VT with the display attached also settles the question. */
  launch.frontend_launched = 0;
  launch.remote_session = 0;
  launch.seat_vt = 1;
  CHECK(nxgl_classify_launch_context_v2(&launch, &context) == NXGL_SUCCESS);
  CHECK(context == NXGL_LAUNCH_CONTEXT_V2_CONSOLE);
  CHECK(nxgl_frame_proof_is_conclusive_v2(context, &conclusive) ==
        NXGL_SUCCESS);
  CHECK(conclusive == 1);

  /* Nothing known about the launch is not permission to accuse the port. */
  launch.seat_vt = 0;
  CHECK(nxgl_classify_launch_context_v2(&launch, &context) == NXGL_SUCCESS);
  CHECK(context == NXGL_LAUNCH_CONTEXT_V2_UNKNOWN);
  CHECK(nxgl_frame_proof_is_conclusive_v2(context, &conclusive) ==
        NXGL_SUCCESS);
  CHECK(conclusive == 0);

  launch.remote_session = 2;
  CHECK(nxgl_classify_launch_context_v2(&launch, &context) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  launch.remote_session = 0;
  launch.struct_size = 0u;
  CHECK(nxgl_classify_launch_context_v2(&launch, &context) ==
        NXGL_ERROR_INVALID_ARGUMENT);
}

static void test_client_array_bridge_is_gated_on_the_whole_tuple(void) {
  nxgl_client_array_observation_v2 obs;
  int bridge = -1;
  memset(&obs, 0, sizeof(obs));
  obs.api_version = NXGL_API_VERSION_V2;
  obs.struct_size = sizeof(obs);

  /* The device that actually crashes: ROCKNIX g24p0 on Mali-G52 under Wayland,
   * observed as SIGSEGV fault=0x3e in the first drawn frame. */
  obs.video_driver = "wayland";
  obs.renderer = "Mali-G52";
  obs.version = "OpenGL ES-CM 1.1 v1.g24p0-00eac0.fe7fa84c52ed3b756cd64cb3246a";
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 1);

  /* Tuples measured on the three devices that render this engine correctly.
   * Each must stay on the direct path: the bridge is a CPU-side mirror and
   * enabling it where it is not needed is a permanent cost. */
  obs.video_driver = "KMSDRM";
  obs.renderer = "Mali-G31";
  obs.version = "OpenGL ES-CM 1.1 v1.r13p0-01rel0.8747d14aec16ec21ac2b2da252c4";
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 0);

  obs.video_driver = "mali";
  obs.renderer = "Mali-450 MP";
  obs.version = "OpenGL ES-CM 1.1";
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 0);

  /* This one carries "wayland" inside the version string while running on
   * KMSDRM, so matching the version alone would wrongly enable the bridge. */
  obs.video_driver = "KMSDRM";
  obs.renderer = "Mali-G310";
  obs.version = "OpenGL ES-CM 1.1 v1.r44p0-wayland-drm-g310-dmaheap-aarch64";
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 0);

  /* Same GPU on another transport, and the same transport on another blob:
   * neither is the failing combination. */
  obs.video_driver = "KMSDRM";
  obs.renderer = "Mali-G52";
  obs.version = "OpenGL ES-CM 1.1 v1.g24p0-00eac0";
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 0);

  obs.video_driver = "wayland";
  obs.renderer = "Mali-G52";
  obs.version = "OpenGL ES-CM 1.1 v1.g18p0-01eac0";
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 0);

  /* A partial tuple is not evidence. */
  obs.video_driver = "wayland";
  obs.renderer = "Mali-G52";
  obs.version = NULL;
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) == NXGL_SUCCESS);
  CHECK(bridge == 0);

  obs.version = "OpenGL ES-CM 1.1 v1.g24p0-00eac0";
  obs.struct_size = 0u;
  CHECK(nxgl_classify_client_array_bridge_v2(&obs, &bridge) ==
        NXGL_ERROR_INVALID_ARGUMENT);
}

int main(void) {
  test_observations_are_monotonic_and_passive();
  test_failed_observations_are_atomic();
  test_silhouette_prioritizes_sampler_wrap_atlas();
  test_silhouette_validation_does_not_overwrite_result();
  test_provider_name_prefilter_is_transport_aware();
  test_sdl_provider_pair_requires_complete_failure_proof();
  test_precontext_recovery_is_one_shot_and_fail_closed();
  test_frame_proof_never_passes_without_evidence();
  test_black_only_accuses_the_port_when_the_launch_could_draw();
  test_client_array_bridge_is_gated_on_the_whole_tuple();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl diagnostic test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "nxgl pure lifecycle/diagnostic tests passed\n");
  return 0;
}
