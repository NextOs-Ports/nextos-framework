/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_godot_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "nxinput Godot runtime: FAIL: %s\n", message);
    exit(1);
  }
}

int main(void) {
  nxinput_godot_action_latch alias = {0};
  nxinput_godot_neutral_handoff handoff = {0};
  nxinput_godot_neutral_handoff native_handoff = {0};
  nxinput_godot_neutral_handoff suppressed_handoff = {0};
  nxinput_godot_vector_alias vector_alias = {{{0}}};
  nxinput_godot_lifecycle lifecycle = {0};
  float strengths[4] = {0};
  float aggregate[4] = {0};

  expect(nxinput_godot_action_preview(&alias, 1) ==
             NXINPUT_GODOT_ACTION_DELIVER,
         "first alias press delivers");
  expect(nxinput_godot_action_commit(&alias, 1) == 0,
         "first alias press commits after sink success");
  expect(nxinput_godot_action_preview(&alias, 1) ==
             NXINPUT_GODOT_ACTION_ACK_ONLY &&
             nxinput_godot_action_commit(&alias, 1) == 0,
         "second alias joins semantic OR");
  expect(nxinput_godot_action_preview(&alias, 0) ==
             NXINPUT_GODOT_ACTION_ACK_ONLY &&
             nxinput_godot_action_commit(&alias, 0) == 0,
         "first alias release does not release the semantic action");
  expect(nxinput_godot_action_preview(&alias, 0) ==
             NXINPUT_GODOT_ACTION_DELIVER &&
             nxinput_godot_action_commit(&alias, 0) == 0 &&
             alias.held_count == 0u,
         "final alias release reaches the sink");
  expect(nxinput_godot_action_preview(&alias, 0) ==
             NXINPUT_GODOT_ACTION_INVALID,
         "unmatched release fails closed");

  nxinput_godot_split_vector(0.20f, -0.20f, strengths);
  expect(strengths[0] == 0.20f && strengths[1] == 0.0f &&
             strengths[2] == 0.0f && strengths[3] == 0.20f,
         "adapter adds no deadzone or axis rescale");

  expect(nxinput_godot_vector_alias_update(
             &vector_alias, 0u, 0.80f, 0.0f, aggregate) == 0 &&
             aggregate[3] == 0.80f,
         "first stick owns the shared vector strength");
  expect(nxinput_godot_vector_alias_update(
             &vector_alias, 1u, 0.0f, 0.0f, aggregate) == 0 &&
             aggregate[3] == 0.80f,
         "neutral alias cannot release the held stick");
  expect(nxinput_godot_vector_alias_update(
             &vector_alias, 1u, 0.40f, -0.60f, aggregate) == 0 &&
             aggregate[0] == 0.60f && aggregate[3] == 0.80f,
         "two sticks aggregate independently by direction");
  expect(nxinput_godot_vector_alias_update(
             &vector_alias, 0u, 0.0f, 0.0f, aggregate) == 0 &&
             aggregate[0] == 0.60f && aggregate[3] == 0.40f,
         "centering one stick preserves the other alias");
  nxinput_godot_vector_alias_clear(&vector_alias);
  expect(nxinput_godot_vector_alias_update(
             &vector_alias, 1u, 0.0f, 0.0f, aggregate) == 0 &&
             aggregate[0] == 0.0f && aggregate[3] == 0.0f,
         "vector alias state clears at an authority boundary");

  nxinput_godot_handoff_snapshot(
      &handoff, nxinput_godot_control_bit(0) | nxinput_godot_control_bit(7),
      16, 0.8f, 0.0f, 17, 0.0f, 0.0f);
  expect(nxinput_godot_handoff_button(&handoff, 0, 1),
         "held native button stays native");
  expect(nxinput_godot_handoff_button(&handoff, 0, 0),
         "native release crosses the boundary");
  expect(!nxinput_godot_handoff_button(&handoff, 0, 1),
         "next press may be governed");
  expect(nxinput_godot_handoff_vector(&handoff, 16, 0.25f, 0.0f),
         "held native vector stays native");
  expect(nxinput_godot_handoff_vector(&handoff, 16, 0.05f, 0.02f),
         "ordinary released-stick drift crosses and clears the boundary");
  expect(!nxinput_godot_handoff_vector(&handoff, 16, 0.3f, 0.0f),
         "next vector may be governed");

  nxinput_godot_handoff_partition(
      &native_handoff, &suppressed_handoff,
      nxinput_godot_control_bit(0) | nxinput_godot_control_bit(2),
      nxinput_godot_control_bit(0) | nxinput_godot_control_bit(16),
      16, 0.8f, 0.0f, 17, 0.6f, 0.0f);
  expect(native_handoff.controls ==
             (nxinput_godot_control_bit(2) |
              nxinput_godot_control_bit(17)),
         "old NONE/native controls retain native ownership");
  expect(suppressed_handoff.controls ==
             (nxinput_godot_control_bit(0) |
              nxinput_godot_control_bit(16)),
         "old ACTION/null controls stay suppressed until neutral");
  expect((native_handoff.controls & suppressed_handoff.controls) == 0u,
         "partition has one authority per held control");

  /* A second transition before release must not reclassify either gesture. */
  nxinput_godot_handoff_partition(
      &native_handoff, &suppressed_handoff,
      nxinput_godot_control_bit(0) | nxinput_godot_control_bit(2),
      UINT32_MAX, 16, 0.8f, 0.0f, 17, 0.6f, 0.0f);
  expect((native_handoff.controls & nxinput_godot_control_bit(2)) != 0u &&
             (native_handoff.controls & nxinput_godot_control_bit(17)) != 0u,
         "second transition preserves earlier native ownership");
  expect((suppressed_handoff.controls & nxinput_godot_control_bit(0)) != 0u &&
             (suppressed_handoff.controls & nxinput_godot_control_bit(16)) != 0u,
         "second transition preserves earlier governed ownership");
  expect(nxinput_godot_handoff_button(&native_handoff, 2, 0) &&
             nxinput_godot_handoff_button(&suppressed_handoff, 0, 0),
         "native and governed button releases clear their own barriers");
  expect(nxinput_godot_handoff_vector(&native_handoff, 17, 0.0f, 0.0f) &&
             nxinput_godot_handoff_vector(
                 &suppressed_handoff, 16, 0.0f, 0.0f),
         "native and governed stick centers clear their own barriers");
  expect(native_handoff.controls == 0u && suppressed_handoff.controls == 0u,
         "all partitioned controls return to neutral");

  expect(nxinput_godot_lifecycle_health_allowed(&lifecycle),
         "healthy runtime may publish health");
  nxinput_godot_lifecycle_fail(&lifecycle);
  expect(nxinput_godot_lifecycle_consume_close(&lifecycle) &&
             !nxinput_godot_lifecycle_consume_close(&lifecycle),
         "fatal close request is one-shot");
  expect(!nxinput_godot_lifecycle_health_allowed(&lifecycle) &&
             nxinput_godot_lifecycle_exit_status(&lifecycle) != 0,
         "fatal remains health-blocking and nonzero after close consumption");
  expect(strcmp(nxinput_godot_runtime_marker(), "nxinput-godot-runtime/1") == 0,
         "runtime marker is stable");

  puts("nxinput Godot runtime: PASS aliases=6 analog=1 vector-alias=5 handoff=6 partition=8 lifecycle=5");
  return 0;
}
