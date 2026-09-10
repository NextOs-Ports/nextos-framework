/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_gptk_godot4_glue.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/input/input_map.h"

#include <math.h>
#include <string.h>

static int godot4_enqueue(const char *action, int pressed, float strength) {
  Input *input = Input::get_singleton();
  if (input == nullptr || action == nullptr || action[0] == '\0')
    return -1;
  Ref<InputEventAction> event;
  event.instantiate();
  if (event.is_null())
    return -1;
  event->set_action(StringName(action));
  event->set_pressed(pressed != 0);
  event->set_strength(pressed ? strength : 0.0f);
  input->parse_input_event(event);
  return 0;
}

int nxinput_godot4_validate_action_sinks(
    const nxinput_godot4_action_sink *sinks, size_t count) {
  InputMap *map = InputMap::get_singleton();
  if (map == nullptr || (count != 0u && sinks == nullptr))
    return -1;
  for (size_t i = 0; i < count; i++) {
    if (sinks[i].semantic_action == nullptr ||
        sinks[i].semantic_action[0] == '\0' ||
        sinks[i].inputmap_action == nullptr ||
        sinks[i].inputmap_action[0] == '\0' ||
        !map->has_action(StringName(sinks[i].inputmap_action)))
      return -1;
  }
  return 0;
}

int nxinput_godot4_validate_vector_sinks(
    const nxinput_godot4_vector_sink *sinks, size_t count) {
  InputMap *map = InputMap::get_singleton();
  if (map == nullptr || (count != 0u && sinks == nullptr))
    return -1;
  for (size_t i = 0; i < count; i++) {
    bool has_direction = false;
    if (sinks[i].semantic_action == nullptr ||
        sinks[i].semantic_action[0] == '\0')
      return -1;
    for (size_t direction = 0; direction < 4u; direction++) {
      const char *name = sinks[i].directions[direction];
      if (name != nullptr && name[0] != '\0') {
        has_direction = true;
        if (!map->has_action(StringName(name)))
          return -1;
      }
    }
    if (!has_direction)
      return -1;
  }
  return 0;
}

int nxinput_godot4_action_callback(void *user, const char *action,
                                   int pressed, float value) {
  nxinput_godot4_action_sink *sink =
      static_cast<nxinput_godot4_action_sink *>(user);
  if (sink == nullptr || action == nullptr ||
      sink->semantic_action == nullptr ||
      strcmp(sink->semantic_action, action) != 0)
    return -1;
  nxinput_godot_action_effect effect =
      nxinput_godot_action_preview(&sink->latch, pressed != 0);
  if (effect == NXINPUT_GODOT_ACTION_INVALID)
    return -1;
  if (effect == NXINPUT_GODOT_ACTION_DELIVER &&
      godot4_enqueue(sink->inputmap_action, pressed != 0,
                     value > 0.0f ? value : 1.0f) != 0)
    return -1;
  return nxinput_godot_action_commit(&sink->latch, pressed != 0);
}

int nxinput_godot4_vector_callback(void *user, const char *action,
                                   float x, float y) {
  nxinput_godot4_vector_sink *sink =
      static_cast<nxinput_godot4_vector_sink *>(user);
  float next[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (sink == nullptr || action == nullptr ||
      sink->semantic_action == nullptr ||
      strcmp(sink->semantic_action, action) != 0)
    return -1;
  nxinput_godot_split_vector(x, y, next);
  for (size_t i = 0; i < 4u; i++) {
    const char *name = sink->directions[i];
    if (name == nullptr || name[0] == '\0')
      continue;
    if (next[i] > 0.0f) {
      if ((sink->strengths[i] <= 0.0f ||
           fabsf(next[i] - sink->strengths[i]) > 0.01f) &&
          godot4_enqueue(name, 1, next[i]) != 0)
        return -1;
    } else if (sink->strengths[i] > 0.0f &&
               godot4_enqueue(name, 0, 0.0f) != 0) {
      return -1;
    }
    sink->strengths[i] = next[i];
  }
  return 0;
}

int nxinput_godot4_vector_release(nxinput_godot4_vector_sink *sink) {
  if (sink == nullptr)
    return -1;
  for (size_t i = 0; i < 4u; i++) {
    const char *name = sink->directions[i];
    if (name != nullptr && name[0] != '\0' && sink->strengths[i] > 0.0f) {
      if (godot4_enqueue(name, 0, 0.0f) != 0)
        return -1;
      sink->strengths[i] = 0.0f;
    }
  }
  return 0;
}

const char *nxinput_gptk_godot4_glue_marker(void) {
  return "nxinput-gptk-godot4-glue/1";
}
