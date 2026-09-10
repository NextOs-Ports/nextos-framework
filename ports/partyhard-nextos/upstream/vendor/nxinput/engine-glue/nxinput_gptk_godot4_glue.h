/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_GODOT4_GLUE_H
#define NXINPUT_GPTK_GODOT4_GLUE_H

/* Source-ABI template: compile this file inside the pinned Godot 4 tree.
 * The port supplies semantic/InputMap descriptors and owns context, pads,
 * co-op and lifecycle. No C++ binary ABI is promised across Godot versions. */

#include "nxinput_godot_runtime.h"

#include <stddef.h>

typedef struct nxinput_godot4_action_sink {
  const char *semantic_action;
  const char *inputmap_action;
  nxinput_godot_action_latch latch;
} nxinput_godot4_action_sink;

typedef struct nxinput_godot4_vector_sink {
  const char *semantic_action;
  const char *directions[4]; /* up, down, left, right */
  float strengths[4];
} nxinput_godot4_vector_sink;

/* InputMap existence is structural evidence only. Semantic consumer evidence
 * remains an external, port-owned release input. */
int nxinput_godot4_validate_action_sinks(
    const nxinput_godot4_action_sink *sinks, size_t count);
int nxinput_godot4_validate_vector_sinks(
    const nxinput_godot4_vector_sink *sinks, size_t count);

/* Direct nxinput_gptk_live callback shapes. Return 0 only when the Godot
 * InputEventAction enqueue succeeded (or an alias was already held). */
int nxinput_godot4_action_callback(void *user, const char *action,
                                   int pressed, float value);
int nxinput_godot4_vector_callback(void *user, const char *action,
                                   float x, float y);
int nxinput_godot4_vector_release(nxinput_godot4_vector_sink *sink);

const char *nxinput_gptk_godot4_glue_marker(void);

#endif
