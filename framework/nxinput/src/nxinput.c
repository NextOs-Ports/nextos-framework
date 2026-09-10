/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput.h"

#include "nxinput_authority_sdl.h"
#include "nxinput_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct nxinput_slot {
  SDL_GameController *controller;
  SDL_JoystickID instance_id;
  uint32_t generation;
  uint32_t capabilities;
  int left_stick_binding_present;
  int16_t raw_axes[NXINPUT_CORE_AXIS_COUNT];
  nxinput_core_pad core;
  char name[NXINPUT_NAME_MAX];
  char guid[NXINPUT_GUID_MAX];
} nxinput_slot;

struct nxinput_context {
  nxinput_config config;
  nxinput_slot slots[NXINPUT_MAX_PADS];
  Uint32 owned_sdl_subsystems;
  Uint32 last_rescan_ticks;
  int previous_controller_event_state;
  int focused;
  int quit_requested;
  int host_analog_sticks_hint;
  nxinput_cursor_context cursor_context;
  /* C3/114A: the single mapping decision. Every pad reaches gameplay only
   * through nxinput_authority_admit(); nothing here applies a mapping. */
  nxinput_authority authority;
  int authority_ready;
  uint32_t mapping_refusals;
};

/* Authority 5 (raw passthrough) is legal only when the CONSUMER declared it
 * understands a raw pad. There is no environment variable and no heuristic
 * for it: a port calls nxinput_declare_raw_consumer() before creating the
 * context, or raw never happens. */
static int nxinput_raw_consumer_declared;

void nxinput_declare_raw_consumer(int accepts_raw) {
  nxinput_raw_consumer_declared = accepts_raw ? 1 : 0;
}

static const SDL_GameControllerButton nxinput_sdl_buttons[] = {
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT};

typedef char nxinput_button_table_size_must_match[
    sizeof(nxinput_sdl_buttons) / sizeof(nxinput_sdl_buttons[0]) ==
            (size_t)NXINPUT_BUTTON_COUNT
        ? 1
        : -1];

static float nxinput_clampf(float value, float minimum, float maximum) {
  if (!(value >= minimum))
    return minimum;
  if (value > maximum)
    return maximum;
  return value;
}

static void nxinput_copy_string(char *destination, size_t destination_size,
                                const char *source) {
  size_t length;

  if (!destination || destination_size == 0u)
    return;
  if (!source)
    source = "";
  length = strlen(source);
  if (length >= destination_size)
    length = destination_size - 1u;
  memcpy(destination, source, length);
  destination[length] = '\0';
}

void nxinput_config_init(nxinput_config *config) {
  if (!config)
    return;
  memset(config, 0, sizeof(*config));
  config->api_version = NXINPUT_API_VERSION;
  config->struct_size = sizeof(*config);
  config->initialize_sdl = 1;
  config->rescan_interval_ms = 1000u;
  config->stick_enter_deadzone = 0.20f;
  config->stick_exit_deadzone = 0.15f;
  config->trigger_deadzone = 0.05f;
  config->cursor_speed = 1.25f;
  config->cursor_smoothing = 0.045f;
}

static int nxinput_config_valid(const nxinput_config *config) {
  if (config->api_version != NXINPUT_API_VERSION ||
      config->struct_size < sizeof(*config)) {
    SDL_SetError("nxinput: incompatible configuration ABI");
    return 0;
  }
  if (!(config->stick_enter_deadzone >= 0.0f &&
        config->stick_enter_deadzone < 1.0f) ||
      !(config->stick_exit_deadzone >= 0.0f &&
        config->stick_exit_deadzone <= config->stick_enter_deadzone) ||
      !(config->trigger_deadzone >= 0.0f &&
        config->trigger_deadzone < 1.0f) ||
      !(config->cursor_speed >= 0.0f) ||
      !(config->cursor_smoothing >= 0.0f)) {
    SDL_SetError("nxinput: invalid deadzone or cursor configuration");
    return 0;
  }
  return 1;
}

/* V4-CONTROLLERS-03 / C3 (mission 114A): the blind loader that used to live
 * here -- SDL_GAMECONTROLLERCONFIG_FILE loaded wholesale, then every
 * SDL_GAMECONTROLLERCONFIG line applied with the return value discarded and
 * no readback -- was REMOVED. It ignored the authority order, could not
 * reach the port bundle, and let a setter that silently dropped or rewrote a
 * mapping pass as success. The single decision is now
 * nxinput_authority_admit(), which runs nxinput_sovereign_resolve() with the
 * real sources, the MEASURED capabilities of the pad and the real
 * setter+readback of SDL. This file applies no mapping of its own. */
static int nxinput_authority_bootstrap(nxinput_context *input) {
  nxinput_authority_runtime runtime;

  if (nxinput_authority_runtime_sdl(&runtime,
                                    nxinput_raw_consumer_declared) != 0 ||
      nxinput_authority_init(&input->authority, &runtime) != 0) {
    SDL_SetError("nxinput: the mapping authority could not be initialized");
    return 0;
  }
  input->authority_ready = 1;
  return 1;
}

static int nxinput_binding_reachable(SDL_GameController *controller,
                                     SDL_GameControllerButtonBind binding) {
  SDL_Joystick *joystick;

  if (!controller)
    return 0;
  joystick = SDL_GameControllerGetJoystick(controller);
  if (!joystick)
    return 0;

  switch (binding.bindType) {
  case SDL_CONTROLLER_BINDTYPE_BUTTON:
    return binding.value.button >= 0 &&
           binding.value.button < SDL_JoystickNumButtons(joystick);
  case SDL_CONTROLLER_BINDTYPE_AXIS:
    return binding.value.axis >= 0 &&
           binding.value.axis < SDL_JoystickNumAxes(joystick);
  case SDL_CONTROLLER_BINDTYPE_HAT:
    return binding.value.hat.hat < SDL_JoystickNumHats(joystick) &&
           binding.value.hat.hat_mask != 0;
  case SDL_CONTROLLER_BINDTYPE_NONE:
  default:
    return 0;
  }
}

static void nxinput_detect_slot_capabilities(nxinput_slot *slot) {
  SDL_GameControllerButtonBind dpad_up;
  SDL_GameControllerButtonBind dpad_down;
  SDL_GameControllerButtonBind dpad_left;
  SDL_GameControllerButtonBind dpad_right;
  SDL_GameControllerButtonBind left_x;
  SDL_GameControllerButtonBind left_y;
  SDL_GameControllerButtonBind right_x;
  SDL_GameControllerButtonBind right_y;
  SDL_GameControllerButtonBind left_trigger;
  SDL_GameControllerButtonBind right_trigger;

  if (!slot)
    return;
  slot->capabilities = 0u;
  slot->left_stick_binding_present = 0;
  if (!slot->controller)
    return;

  dpad_up = SDL_GameControllerGetBindForButton(
      slot->controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
  dpad_down = SDL_GameControllerGetBindForButton(
      slot->controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  dpad_left = SDL_GameControllerGetBindForButton(
      slot->controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  dpad_right = SDL_GameControllerGetBindForButton(
      slot->controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  left_x = SDL_GameControllerGetBindForAxis(
      slot->controller, SDL_CONTROLLER_AXIS_LEFTX);
  left_y = SDL_GameControllerGetBindForAxis(
      slot->controller, SDL_CONTROLLER_AXIS_LEFTY);
  right_x = SDL_GameControllerGetBindForAxis(
      slot->controller, SDL_CONTROLLER_AXIS_RIGHTX);
  right_y = SDL_GameControllerGetBindForAxis(
      slot->controller, SDL_CONTROLLER_AXIS_RIGHTY);
  left_trigger = SDL_GameControllerGetBindForAxis(
      slot->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  right_trigger = SDL_GameControllerGetBindForAxis(
      slot->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

  if (nxinput_binding_reachable(slot->controller, dpad_up) &&
      nxinput_binding_reachable(slot->controller, dpad_down) &&
      nxinput_binding_reachable(slot->controller, dpad_left) &&
      nxinput_binding_reachable(slot->controller, dpad_right))
    slot->capabilities |= NXINPUT_PAD_CAP_DPAD;
  slot->left_stick_binding_present =
      nxinput_binding_reachable(slot->controller, left_x) ||
      nxinput_binding_reachable(slot->controller, left_y);
  if (nxinput_binding_reachable(slot->controller, left_x) &&
      nxinput_binding_reachable(slot->controller, left_y))
    slot->capabilities |= NXINPUT_PAD_CAP_LEFT_STICK;
  if (nxinput_binding_reachable(slot->controller, right_x) &&
      nxinput_binding_reachable(slot->controller, right_y))
    slot->capabilities |= NXINPUT_PAD_CAP_RIGHT_STICK;
  if (nxinput_binding_reachable(slot->controller, left_trigger))
    slot->capabilities |= NXINPUT_PAD_CAP_LEFT_TRIGGER;
  if (nxinput_binding_reachable(slot->controller, right_trigger))
    slot->capabilities |= NXINPUT_PAD_CAP_RIGHT_TRIGGER;
}

static int nxinput_read_host_analog_sticks_hint(void) {
  const char *value = SDL_getenv("NXINPUT_ANALOG_STICKS_HINT");
  if (!value ||
      (value[0] != '0' && value[0] != '1' && value[0] != '2') ||
      value[1] != '\0')
    return NXINPUT_ANALOG_STICKS_HINT_UNKNOWN;
  return (int)(value[0] - '0');
}

/* V4-CONTROLLERS-03 / C3: the topology-triggered post-load rewrite that used
 * to live here (nxinput_normalize_device_mapping and its handheld-gap
 * detector) was REMOVED from the default path. It rewrote a/b/x/y/guide
 * AFTER the PortMaster/CFW mapping was loaded and, on the physically
 * captured GO-Super mapping, contradicted the sovereign x/y order
 * (tests/corpus/red-portmaster-handheld-rewrite.json). The sovereign
 * authority order lives in nxinput_sovereign.h; only opt-in converters with
 * a measured input, output and consumer (e.g. the SDL3 PortMaster manager)
 * may transform a mapping, and no automatic A/B swap, GUIDE->R3 promotion or
 * second normalization over a complete mapping is ever applied here. */

static int nxinput_find_instance_internal(const nxinput_context *input,
                                          SDL_JoystickID instance_id) {
  unsigned int index;
  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    if (input->slots[index].controller &&
        input->slots[index].instance_id == instance_id)
      return (int)index;
  }
  return -1;
}

static void nxinput_refresh_axes(nxinput_context *input, nxinput_slot *slot) {
  nxinput_core_set_axes(&slot->core, slot->raw_axes,
                        input->config.stick_enter_deadzone,
                        input->config.stick_exit_deadzone,
                        input->config.trigger_deadzone);
}

static void nxinput_sample_slot(nxinput_context *input, nxinput_slot *slot) {
  unsigned int index;

  if (!slot->controller || !input->focused)
    return;

  slot->raw_axes[0] = (int16_t)SDL_GameControllerGetAxis(
      slot->controller, SDL_CONTROLLER_AXIS_LEFTX);
  slot->raw_axes[1] = (int16_t)SDL_GameControllerGetAxis(
      slot->controller, SDL_CONTROLLER_AXIS_LEFTY);
  slot->raw_axes[2] = (int16_t)SDL_GameControllerGetAxis(
      slot->controller, SDL_CONTROLLER_AXIS_RIGHTX);
  slot->raw_axes[3] = (int16_t)SDL_GameControllerGetAxis(
      slot->controller, SDL_CONTROLLER_AXIS_RIGHTY);
  slot->raw_axes[4] = (int16_t)SDL_GameControllerGetAxis(
      slot->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  slot->raw_axes[5] = (int16_t)SDL_GameControllerGetAxis(
      slot->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  nxinput_refresh_axes(input, slot);

  for (index = 0u; index < (unsigned int)NXINPUT_BUTTON_COUNT; index++) {
    int down = SDL_GameControllerGetButton(slot->controller,
                                           nxinput_sdl_buttons[index]) != 0;
    nxinput_core_set_button(
        &slot->core, (nxinput_button)index, down,
        input->cursor_context == NXINPUT_CURSOR_MENU,
        &input->quit_requested);
  }
}

static void nxinput_disconnect_slot(nxinput_context *input,
                                    unsigned int index) {
  nxinput_slot *slot;

  if (index >= NXINPUT_MAX_PADS)
    return;
  slot = &input->slots[index];
  if (!slot->controller)
    return;

  nxinput_core_release_all(&slot->core);
  memset(slot->raw_axes, 0, sizeof(slot->raw_axes));
  SDL_GameControllerClose(slot->controller);
  slot->controller = NULL;
  slot->instance_id = (SDL_JoystickID)-1;
  slot->generation++;
  slot->capabilities = 0u;
  slot->left_stick_binding_present = 0;
  slot->name[0] = '\0';
  slot->guid[0] = '\0';
}

static int nxinput_open_device(nxinput_context *input, int device_index) {
  SDL_GameController *controller;
  SDL_Joystick *joystick;
  SDL_JoystickID instance_id;
  SDL_JoystickGUID guid;
  unsigned int index;
  nxinput_slot *slot;
  uint32_t retained_releases;
  float retained_cursor_x;
  float retained_cursor_y;
  nxinput_sovereign_decision decision;

  if (device_index < 0 || !input->authority_ready)
    return 0;

  /* The pad's identity comes from the JOYSTICK layer, before any mapping is
   * chosen: a pad that SDL does not yet recognize as a game controller is
   * exactly the case authority 3 (the port bundle) exists for, so it must
   * still reach the decision. */
  joystick = SDL_JoystickOpen(device_index);
  if (!joystick)
    return 0;
  instance_id = SDL_JoystickInstanceID(joystick);
  SDL_JoystickClose(joystick);
  if (instance_id < 0)
    return 0;
  if (nxinput_find_instance_internal(input, instance_id) >= 0)
    return 1;

  /* C3/114A: the single mapping decision. It measures this pad, walks the
   * sovereign authority order and installs the winner through a real
   * setter+readback. A FAIL_EXPLICIT decision means no authority could serve
   * this pad: it is NOT opened, and gameplay never sees it. */
  if (nxinput_authority_admit(&input->authority, device_index,
                              (int32_t)instance_id, &decision) != 0) {
    input->mapping_refusals++;
    SDL_SetError("nxinput: no mapping authority could serve pad %d (%s)",
                 device_index,
                 nxinput_sovereign_reason_name(decision.reason));
    return 0;
  }
  /* The decision won, so the runtime now holds a validated mapping for this
   * GUID. If SDL still refuses to present it as a game controller there is
   * nothing to play with: fail closed instead of opening a raw joystick. */
  if (!SDL_IsGameController(device_index)) {
    nxinput_authority_forget(&input->authority, (int32_t)instance_id);
    input->mapping_refusals++;
    SDL_SetError("nxinput: pad %d has a decision but no usable controller",
                 device_index);
    return 0;
  }
  controller = SDL_GameControllerOpen(device_index);
  if (!controller) {
    nxinput_authority_forget(&input->authority, (int32_t)instance_id);
    input->mapping_refusals++;
    return 0;
  }
  joystick = SDL_GameControllerGetJoystick(controller);
  if (!joystick) {
    SDL_GameControllerClose(controller);
    nxinput_authority_forget(&input->authority, (int32_t)instance_id);
    input->mapping_refusals++;
    return 0;
  }

  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    if (!input->slots[index].controller)
      break;
  }
  if (index == NXINPUT_MAX_PADS) {
    SDL_GameControllerClose(controller);
    nxinput_authority_forget(&input->authority, (int32_t)instance_id);
    return 0;
  }

  slot = &input->slots[index];
  retained_releases = slot->core.released_latch;
  retained_cursor_x = slot->core.cursor_x;
  retained_cursor_y = slot->core.cursor_y;
  nxinput_core_pad_init(&slot->core);
  slot->core.released_latch = retained_releases;
  slot->core.cursor_x = retained_cursor_x;
  slot->core.cursor_y = retained_cursor_y;
  memset(slot->raw_axes, 0, sizeof(slot->raw_axes));

  slot->controller = controller;
  slot->instance_id = instance_id;
  nxinput_detect_slot_capabilities(slot);
  slot->generation++;
  nxinput_copy_string(slot->name, sizeof(slot->name),
                      SDL_GameControllerName(controller));
  guid = SDL_JoystickGetGUID(joystick);
  SDL_JoystickGetGUIDString(guid, slot->guid, (int)sizeof(slot->guid));
  nxinput_sample_slot(input, slot);
  return 1;
}

static void nxinput_rescan(nxinput_context *input) {
  int count;
  int index;

  count = SDL_NumJoysticks();
  for (index = 0; index < count; index++)
    (void)nxinput_open_device(input, index);
  input->last_rescan_ticks = SDL_GetTicks();
}

nxinput_context *nxinput_create(const nxinput_config *config) {
  nxinput_config effective;
  nxinput_context *input;
  Uint32 wanted;
  Uint32 initialized;
  Uint32 missing;
  unsigned int index;

  nxinput_config_init(&effective);
  if (config) {
    if (config->api_version != NXINPUT_API_VERSION ||
        config->struct_size < sizeof(*config)) {
      SDL_SetError("nxinput: incompatible configuration ABI");
      return NULL;
    }
    effective = *config;
  }
  if (!nxinput_config_valid(&effective))
    return NULL;

  input = (nxinput_context *)calloc(1u, sizeof(*input));
  if (!input) {
    SDL_SetError("nxinput: out of memory");
    return NULL;
  }
  input->config = effective;
  input->focused = 1;
  input->host_analog_sticks_hint = nxinput_read_host_analog_sticks_hint();
  input->cursor_context = NXINPUT_CURSOR_OFF;
  input->previous_controller_event_state = SDL_QUERY;
  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    input->slots[index].instance_id = (SDL_JoystickID)-1;
    nxinput_core_pad_init(&input->slots[index].core);
  }

  wanted = SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  initialized = SDL_WasInit(wanted);
  missing = wanted & ~initialized;
  if (effective.initialize_sdl) {
    if (missing != 0u && SDL_InitSubSystem(missing) != 0) {
      free(input);
      return NULL;
    }
    input->owned_sdl_subsystems = missing;
  } else if ((initialized & SDL_INIT_GAMECONTROLLER) == 0u) {
    SDL_SetError("nxinput: SDL game controller subsystem is not initialized");
    free(input);
    return NULL;
  }

  input->previous_controller_event_state =
      SDL_GameControllerEventState(SDL_QUERY);
  (void)SDL_GameControllerEventState(SDL_ENABLE);
  if (!nxinput_authority_bootstrap(input)) {
    if (input->owned_sdl_subsystems != 0u)
      SDL_QuitSubSystem(input->owned_sdl_subsystems);
    free(input);
    return NULL;
  }
  nxinput_rescan(input);
  /* Explicit failure BEFORE gameplay: pads are physically present and not a
   * single one could be served by any authority. Returning a context here
   * would hand the port a controllerless session that looks healthy. */
  if (input->authority.admitted == 0u && input->mapping_refusals > 0u) {
    SDL_SetError("nxinput: %u pad(s) present and no mapping authority could "
                 "serve any of them",
                 (unsigned int)input->mapping_refusals);
    nxinput_destroy(input);
    return NULL;
  }
  return input;
}

const struct nxinput_authority *nxinput_mapping_authority(
    const nxinput_context *input) {
  return input ? &input->authority : NULL;
}

void nxinput_destroy(nxinput_context *input) {
  unsigned int index;

  if (!input)
    return;
  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    if (input->slots[index].controller)
      SDL_GameControllerClose(input->slots[index].controller);
  }
  if (input->previous_controller_event_state == SDL_DISABLE)
    (void)SDL_GameControllerEventState(SDL_DISABLE);
  if (input->owned_sdl_subsystems != 0u)
    SDL_QuitSubSystem(input->owned_sdl_subsystems);
  free(input);
}

static int nxinput_button_from_sdl(Uint8 value, nxinput_button *button) {
  unsigned int index;
  for (index = 0u; index < (unsigned int)NXINPUT_BUTTON_COUNT; index++) {
    if (value == (Uint8)nxinput_sdl_buttons[index]) {
      *button = (nxinput_button)index;
      return 1;
    }
  }
  return 0;
}

void nxinput_observe_event(nxinput_context *input, const SDL_Event *event) {
  int slot_index;

  if (!input || !event)
    return;

  switch (event->type) {
  case SDL_CONTROLLERDEVICEADDED:
    (void)nxinput_open_device(input, event->cdevice.which);
    break;
  case SDL_CONTROLLERDEVICEREMOVED:
    slot_index = nxinput_find_instance_internal(
        input, (SDL_JoystickID)event->cdevice.which);
    if (slot_index >= 0)
      nxinput_disconnect_slot(input, (unsigned int)slot_index);
    /* The mapping decision dies with the device: a pad that comes back is
     * measured and resolved again, and never inherits this one's mapping. */
    nxinput_authority_forget(&input->authority,
                             (int32_t)event->cdevice.which);
    break;
#if SDL_VERSION_ATLEAST(2, 0, 4)
  case SDL_CONTROLLERDEVICEREMAPPED:
    slot_index = nxinput_find_instance_internal(
        input, (SDL_JoystickID)event->cdevice.which);
    if (slot_index >= 0) {
      uint32_t old_capabilities = input->slots[slot_index].capabilities;
      int old_left_binding =
          input->slots[slot_index].left_stick_binding_present;
      nxinput_detect_slot_capabilities(&input->slots[slot_index]);
      if (old_capabilities != input->slots[slot_index].capabilities ||
          old_left_binding !=
              input->slots[slot_index].left_stick_binding_present)
        input->slots[slot_index].generation++;
      nxinput_sample_slot(input, &input->slots[slot_index]);
    }
    break;
#endif
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    nxinput_button button;
    slot_index = nxinput_find_instance_internal(
        input, (SDL_JoystickID)event->cbutton.which);
    if (slot_index < 0) {
      nxinput_rescan(input);
      slot_index = nxinput_find_instance_internal(
          input, (SDL_JoystickID)event->cbutton.which);
    }
    if (slot_index >= 0 && input->focused &&
        nxinput_button_from_sdl(event->cbutton.button, &button)) {
      nxinput_core_set_button(
          &input->slots[slot_index].core, button,
          event->type == SDL_CONTROLLERBUTTONDOWN,
          input->cursor_context == NXINPUT_CURSOR_MENU,
          &input->quit_requested);
    }
    break;
  }
  case SDL_CONTROLLERAXISMOTION:
    slot_index = nxinput_find_instance_internal(
        input, (SDL_JoystickID)event->caxis.which);
    if (slot_index >= 0 && input->focused &&
        event->caxis.axis < (Uint8)NXINPUT_CORE_AXIS_COUNT) {
      input->slots[slot_index].raw_axes[event->caxis.axis] =
          (int16_t)event->caxis.value;
      nxinput_refresh_axes(input, &input->slots[slot_index]);
    }
    break;
  case SDL_WINDOWEVENT:
    if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
      nxinput_set_focus(input, 0);
    else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
      nxinput_set_focus(input, 1);
    break;
  case SDL_APP_WILLENTERBACKGROUND:
  case SDL_APP_DIDENTERBACKGROUND:
    nxinput_set_focus(input, 0);
    break;
  case SDL_APP_WILLENTERFOREGROUND:
  case SDL_APP_DIDENTERFOREGROUND:
    nxinput_set_focus(input, 1);
    break;
  default:
    break;
  }
}

void nxinput_poll(nxinput_context *input) {
  unsigned int index;
  Uint32 now;

  if (!input)
    return;
  SDL_GameControllerUpdate();

  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    nxinput_slot *slot = &input->slots[index];
    if (!slot->controller)
      continue;
    if (!SDL_GameControllerGetAttached(slot->controller)) {
      SDL_JoystickID gone = slot->instance_id;
      nxinput_disconnect_slot(input, index);
      nxinput_authority_forget(&input->authority, (int32_t)gone);
      continue;
    }
    nxinput_sample_slot(input, slot);
  }

  if (input->config.rescan_interval_ms == 0u)
    return;
  now = SDL_GetTicks();
  if ((Uint32)(now - input->last_rescan_ticks) >=
      input->config.rescan_interval_ms)
    nxinput_rescan(input);
}

void nxinput_set_focus(nxinput_context *input, int focused) {
  unsigned int index;

  if (!input)
    return;
  focused = focused != 0;
  if (input->focused == focused)
    return;
  input->focused = focused;
  if (focused)
    return;

  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    if (input->slots[index].controller) {
      nxinput_core_release_all(&input->slots[index].core);
      memset(input->slots[index].raw_axes, 0,
             sizeof(input->slots[index].raw_axes));
    }
  }
}

unsigned int nxinput_connected_count(const nxinput_context *input) {
  unsigned int count = 0u;
  unsigned int index;
  if (!input)
    return 0u;
  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    if (input->slots[index].controller)
      count++;
  }
  return count;
}

int nxinput_first_connected(const nxinput_context *input) {
  unsigned int index;
  if (!input)
    return -1;
  for (index = 0u; index < NXINPUT_MAX_PADS; index++) {
    if (input->slots[index].controller)
      return (int)index;
  }
  return -1;
}

SDL_GameController *nxinput_pad_sdl_controller(const nxinput_context *input,
                                               unsigned int slot) {
  if (!input || slot >= NXINPUT_MAX_PADS)
    return NULL;
  return input->slots[slot].controller;
}

int nxinput_find_instance(const nxinput_context *input, int32_t instance_id) {
  if (!input)
    return -1;
  return nxinput_find_instance_internal(input, (SDL_JoystickID)instance_id);
}

static void nxinput_apply_pad_options(const nxinput_slot *slot,
                                      uint32_t options,
                                      nxinput_pad_state *state) {
  int horizontal;
  int vertical;
  float scale;

  if (!slot || !state ||
      (options & NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING) == 0u ||
      (slot->capabilities & NXINPUT_PAD_CAP_DPAD) == 0u ||
      slot->left_stick_binding_present)
    return;

  horizontal =
      ((state->buttons &
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT)) != 0u) -
      ((state->buttons &
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT)) != 0u);
  vertical =
      ((state->buttons &
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_DOWN)) != 0u) -
      ((state->buttons &
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_UP)) != 0u);
  scale = horizontal != 0 && vertical != 0 ? 0.70710678118f : 1.0f;
  state->left_x = (float)horizontal * scale;
  state->left_y = (float)vertical * scale;
}

int nxinput_get_pad_with_options(const nxinput_context *input,
                                 unsigned int slot_index, uint32_t options,
                                 nxinput_pad_state *state) {
  const nxinput_slot *slot;

  if (!input || !state || slot_index >= NXINPUT_MAX_PADS ||
      (options & ~NXINPUT_PAD_OPTION_MASK_ALL) != 0u)
    return 0;
  slot = &input->slots[slot_index];
  memset(state, 0, sizeof(*state));
  state->slot = slot_index;
  state->connected = slot->controller != NULL;
  state->focused = input->focused;
  state->instance_id = (int32_t)slot->instance_id;
  state->generation = slot->generation;
  state->buttons = slot->core.buttons;
  state->pressed_latch = slot->core.pressed_latch;
  state->released_latch = slot->core.released_latch;
  state->left_x = slot->core.left_x;
  state->left_y = slot->core.left_y;
  state->right_x = slot->core.right_x;
  state->right_y = slot->core.right_y;
  state->left_trigger = slot->core.left_trigger;
  state->right_trigger = slot->core.right_trigger;
  nxinput_copy_string(state->name, sizeof(state->name), slot->name);
  nxinput_copy_string(state->guid, sizeof(state->guid), slot->guid);
  nxinput_apply_pad_options(slot, options, state);
  return 1;
}

int nxinput_get_pad(const nxinput_context *input, unsigned int slot_index,
                    nxinput_pad_state *state) {
  return nxinput_get_pad_with_options(input, slot_index,
                                      NXINPUT_PAD_OPTION_NONE, state);
}

int nxinput_get_pad_capabilities(const nxinput_context *input,
                                 unsigned int slot_index,
                                 uint32_t *capabilities) {
  if (!input || !capabilities || slot_index >= NXINPUT_MAX_PADS)
    return 0;
  *capabilities = input->slots[slot_index].capabilities;
  return 1;
}

int nxinput_host_analog_sticks_hint(const nxinput_context *input) {
  return input ? input->host_analog_sticks_hint
               : NXINPUT_ANALOG_STICKS_HINT_UNKNOWN;
}

uint32_t nxinput_consume_pressed(nxinput_context *input,
                                 unsigned int slot_index, uint32_t mask) {
  uint32_t consumed;
  nxinput_core_pad *pad;

  if (!input || slot_index >= NXINPUT_MAX_PADS)
    return 0u;
  pad = &input->slots[slot_index].core;
  consumed = pad->pressed_latch & mask & NXINPUT_BUTTON_MASK_ALL;
  pad->pressed_latch &= ~consumed;
  return consumed;
}

uint32_t nxinput_consume_released(nxinput_context *input,
                                  unsigned int slot_index, uint32_t mask) {
  uint32_t consumed;
  nxinput_core_pad *pad;

  if (!input || slot_index >= NXINPUT_MAX_PADS)
    return 0u;
  pad = &input->slots[slot_index].core;
  consumed = pad->released_latch & mask & NXINPUT_BUTTON_MASK_ALL;
  pad->released_latch &= ~consumed;
  return consumed;
}

int nxinput_quit_requested(const nxinput_context *input) {
  return input ? input->quit_requested : 0;
}

int nxinput_consume_quit_request(nxinput_context *input) {
  int requested;
  if (!input)
    return 0;
  requested = input->quit_requested;
  input->quit_requested = 0;
  return requested;
}

void nxinput_set_cursor_context(nxinput_context *input,
                                nxinput_cursor_context context) {
  unsigned int index;

  if (!input || context < NXINPUT_CURSOR_OFF || context > NXINPUT_CURSOR_MENU)
    return;
  if (input->cursor_context == context)
    return;
  input->cursor_context = context;
  for (index = 0u; index < NXINPUT_MAX_PADS; index++)
    nxinput_core_cursor_reset_motion(&input->slots[index].core, 1);
}

nxinput_cursor_context
nxinput_get_cursor_context(const nxinput_context *input) {
  return input ? input->cursor_context : NXINPUT_CURSOR_OFF;
}

int nxinput_cursor_warp(nxinput_context *input, unsigned int slot_index,
                        float x, float y) {
  nxinput_core_pad *pad;
  if (!input || slot_index >= NXINPUT_MAX_PADS)
    return 0;
  pad = &input->slots[slot_index].core;
  pad->cursor_x = nxinput_clampf(x, 0.0f, 1.0f);
  pad->cursor_y = nxinput_clampf(y, 0.0f, 1.0f);
  nxinput_core_cursor_reset_motion(pad, 0);
  return 1;
}

int nxinput_cursor_update_with_options(nxinput_context *input,
                                       unsigned int slot_index,
                                       float delta_seconds,
                                       uint32_t options,
                                       nxinput_cursor_state *state) {
  nxinput_slot *slot;
  float cursor_x;
  float cursor_y;

  if (!input || !state || slot_index >= NXINPUT_MAX_PADS ||
      (options & ~NXINPUT_CURSOR_OPTION_MASK_ALL) != 0u)
    return 0;
  slot = &input->slots[slot_index];
  memset(state, 0, sizeof(*state));

  if (input->cursor_context != NXINPUT_CURSOR_MENU || !input->focused ||
      !slot->controller) {
    nxinput_core_cursor_reset_motion(&slot->core, 0);
    state->x = slot->core.cursor_x;
    state->y = slot->core.cursor_y;
    state->click_pending = slot->core.cursor_click_latch;
    return 1;
  }

  cursor_x = slot->core.right_x;
  cursor_y = slot->core.right_y;
  if ((options & NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING) != 0u &&
      (slot->capabilities & NXINPUT_PAD_CAP_RIGHT_STICK) == 0u &&
      (slot->capabilities & NXINPUT_PAD_CAP_LEFT_STICK) != 0u) {
    cursor_x = slot->core.left_x;
    cursor_y = slot->core.left_y;
  }
  /* P7 (0.10.0): a zero-stick pad may opt in to D-pad cursor motion, menu
   * only. While engaged, the D-pad latches are released so one physical
   * press never fires twice (cursor AND game D-pad). */
  if (nxinput_core_cursor_dpad_engages(slot->capabilities, options, 1)) {
    uint32_t dpad_mask =
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_UP) |
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_DOWN) |
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT) |
        NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT);
    nxinput_core_cursor_axes_from_dpad(slot->core.buttons, &cursor_x,
                                       &cursor_y);
    slot->core.pressed_latch &= ~dpad_mask;
    slot->core.released_latch &= ~dpad_mask;
  }
  nxinput_core_cursor_update_axes(
      &slot->core, cursor_x, cursor_y, delta_seconds,
      input->config.cursor_speed, input->config.cursor_smoothing, state);
  return 1;
}

int nxinput_cursor_update(nxinput_context *input, unsigned int slot_index,
                          float delta_seconds, nxinput_cursor_state *state) {
  return nxinput_cursor_update_with_options(
      input, slot_index, delta_seconds, NXINPUT_CURSOR_OPTION_NONE, state);
}

int nxinput_cursor_consume_click(nxinput_context *input,
                                 unsigned int slot_index) {
  int pending;
  if (!input || slot_index >= NXINPUT_MAX_PADS)
    return 0;
  pending = input->slots[slot_index].core.cursor_click_latch;
  input->slots[slot_index].core.cursor_click_latch = 0;
  return pending;
}
