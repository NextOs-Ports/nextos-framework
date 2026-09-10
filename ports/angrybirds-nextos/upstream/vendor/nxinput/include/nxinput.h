/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_H
#define NXINPUT_H

#include <stddef.h>
#include <stdint.h>

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_API_VERSION 1u
#define NXINPUT_VERSION "0.3.1"

#define NXINPUT_MAX_PADS 4u
#define NXINPUT_NAME_MAX 128u
#define NXINPUT_GUID_MAX 33u

typedef struct nxinput_context nxinput_context;

/* Xbox-position semantics. The physical labels are resolved by SDL's inherited
 * controller mapping, normally supplied by PortMaster/the firmware through
 * SDL_GAMECONTROLLERCONFIG. */
typedef enum nxinput_button {
  NXINPUT_BUTTON_A = 0,
  NXINPUT_BUTTON_B,
  NXINPUT_BUTTON_X,
  NXINPUT_BUTTON_Y,
  NXINPUT_BUTTON_BACK,
  NXINPUT_BUTTON_GUIDE,
  NXINPUT_BUTTON_START,
  NXINPUT_BUTTON_LEFT_STICK,
  NXINPUT_BUTTON_RIGHT_STICK,
  NXINPUT_BUTTON_LEFT_SHOULDER,
  NXINPUT_BUTTON_RIGHT_SHOULDER,
  NXINPUT_BUTTON_DPAD_UP,
  NXINPUT_BUTTON_DPAD_DOWN,
  NXINPUT_BUTTON_DPAD_LEFT,
  NXINPUT_BUTTON_DPAD_RIGHT,
  NXINPUT_BUTTON_COUNT
} nxinput_button;

#define NXINPUT_BUTTON_SELECT NXINPUT_BUTTON_BACK
#define NXINPUT_BUTTON_L3 NXINPUT_BUTTON_LEFT_STICK
#define NXINPUT_BUTTON_R3 NXINPUT_BUTTON_RIGHT_STICK
#define NXINPUT_BUTTON_LB NXINPUT_BUTTON_LEFT_SHOULDER
#define NXINPUT_BUTTON_RB NXINPUT_BUTTON_RIGHT_SHOULDER

#define NXINPUT_BUTTON_BIT(button) \
  (UINT32_C(1) << (unsigned int)(button))
#define NXINPUT_BUTTON_MASK_ALL \
  ((UINT32_C(1) << (unsigned int)NXINPUT_BUTTON_COUNT) - UINT32_C(1))

/* Physical logical bindings exposed by the active SDL_GameController mapping.
 * These facts are per pad and never inferred from a device name, GUID or the
 * host-wide PortMaster stick-count hint. */
typedef enum nxinput_pad_capability {
  NXINPUT_PAD_CAP_DPAD = UINT32_C(1) << 0,
  NXINPUT_PAD_CAP_LEFT_STICK = UINT32_C(1) << 1,
  NXINPUT_PAD_CAP_RIGHT_STICK = UINT32_C(1) << 2,
  NXINPUT_PAD_CAP_LEFT_TRIGGER = UINT32_C(1) << 3,
  NXINPUT_PAD_CAP_RIGHT_TRIGGER = UINT32_C(1) << 4
} nxinput_pad_capability;

#define NXINPUT_PAD_CAP_MASK_ALL                                           \
  (NXINPUT_PAD_CAP_DPAD | NXINPUT_PAD_CAP_LEFT_STICK |                    \
   NXINPUT_PAD_CAP_RIGHT_STICK | NXINPUT_PAD_CAP_LEFT_TRIGGER |           \
   NXINPUT_PAD_CAP_RIGHT_TRIGGER)

typedef enum nxinput_pad_option {
  NXINPUT_PAD_OPTION_NONE = 0,
  /* Duplicate a complete D-pad into the returned left-stick values only when
   * neither physical left-stick axis is bound. The stored/raw state and D-pad
   * buttons remain unchanged. */
  NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING = UINT32_C(1) << 0
} nxinput_pad_option;

#define NXINPUT_PAD_OPTION_MASK_ALL \
  NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING
#define NXINPUT_ANALOG_STICKS_HINT_UNKNOWN (-1)

typedef struct nxinput_config {
  uint32_t api_version;
  size_t struct_size;

  /* When non-zero, nxinput initializes missing SDL event/joystick/controller
   * subsystems and releases only the subsystem references it acquired. */
  int initialize_sdl;

  /* Polling also performs a conservative rescan so a missed hotplug event is
   * recoverable. Zero disables periodic rescans after the initial scan. */
  uint32_t rescan_interval_ms;

  /* A neutral stick becomes active at enter_deadzone and returns to neutral at
   * exit_deadzone. exit_deadzone must not exceed enter_deadzone. */
  float stick_enter_deadzone;
  float stick_exit_deadzone;
  float trigger_deadzone;

  /* Cursor speed is normalized screen lengths/second. Smoothing is a time
   * constant in seconds; zero selects an immediate velocity response. */
  float cursor_speed;
  float cursor_smoothing;
} nxinput_config;

typedef struct nxinput_pad_state {
  unsigned int slot;
  int connected;
  int focused;
  int32_t instance_id;
  uint32_t generation;

  uint32_t buttons;
  uint32_t pressed_latch;
  uint32_t released_latch;

  float left_x;
  float left_y;
  float right_x;
  float right_y;
  float left_trigger;
  float right_trigger;

  char name[NXINPUT_NAME_MAX];
  char guid[NXINPUT_GUID_MAX];
} nxinput_pad_state;

typedef enum nxinput_cursor_context {
  NXINPUT_CURSOR_OFF = 0,
  NXINPUT_CURSOR_GAMEPLAY,
  NXINPUT_CURSOR_MENU
} nxinput_cursor_context;

typedef struct nxinput_cursor_state {
  int active;
  int moved;
  int click_pending;
  float x;
  float y;
  float velocity_x;
  float velocity_y;
} nxinput_cursor_state;

typedef enum nxinput_cursor_option {
  NXINPUT_CURSOR_OPTION_NONE = 0,
  /* In a proven MENU context only, use the real left stick for cursor motion
   * when the opened controller has no reachable right-stick axes. This does
   * not change click binding and never applies in GAMEPLAY. */
  NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING = UINT32_C(1) << 0
} nxinput_cursor_option;

#define NXINPUT_CURSOR_OPTION_MASK_ALL \
  NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING

/* Fill a complete versioned configuration with conservative handheld defaults. */
void nxinput_config_init(nxinput_config *config);

/* Returns NULL and leaves a diagnostic in SDL_GetError() on failure. */
nxinput_context *nxinput_create(const nxinput_config *config);
void nxinput_destroy(nxinput_context *input);

/* Observe an event already obtained by the port's own SDL_PollEvent loop.
 * The event remains owned by the caller and must still be delivered to the
 * game/window/lifecycle handlers. nxinput never drains the SDL event queue. */
void nxinput_observe_event(nxinput_context *input, const SDL_Event *event);

/* Refreshes controller state without consuming application events. Call once
 * per game update, after forwarding all currently queued events when possible. */
void nxinput_poll(nxinput_context *input);

/* Explicit lifecycle hook for engines whose focus state does not arrive as an
 * SDL window/app event. Losing focus immediately releases all logical states. */
void nxinput_set_focus(nxinput_context *input, int focused);

unsigned int nxinput_connected_count(const nxinput_context *input);
int nxinput_first_connected(const nxinput_context *input);
int nxinput_find_instance(const nxinput_context *input, int32_t instance_id);
/* 0.3.2: the SDL_GameController behind a slot (NULL when disconnected), so an
 * adapter can read binds -- e.g. to derive the physical SELECT/START evdev
 * codes for nxinput_evdev_chord.h. Never close or remap it. */
SDL_GameController *nxinput_pad_sdl_controller(const nxinput_context *input,
                                               unsigned int slot);
int nxinput_get_pad(const nxinput_context *input, unsigned int slot,
                    nxinput_pad_state *state);

/* Additive API: nxinput_get_pad() remains the raw/default-off view. An adapter
 * must request options for each read, so no global state can silently change
 * another consumer or the nxcompat receipt path. */
int nxinput_get_pad_with_options(const nxinput_context *input,
                                 unsigned int slot, uint32_t options,
                                 nxinput_pad_state *state);
int nxinput_get_pad_capabilities(const nxinput_context *input,
                                 unsigned int slot, uint32_t *capabilities);

/* PortMaster's validated integrated-handheld hint: -1 when unavailable, else
 * 0, 1 or 2. It is host-wide diagnostic context, never a per-pad override. */
int nxinput_host_analog_sticks_hint(const nxinput_context *input);

/* A short down/up pair remains in pressed_latch until consumed. Consumption is
 * mask-selective and does not alter the current down state. */
uint32_t nxinput_consume_pressed(nxinput_context *input, unsigned int slot,
                                 uint32_t mask);
uint32_t nxinput_consume_released(nxinput_context *input, unsigned int slot,
                                  uint32_t mask);

/* BACK/SELECT + START creates a sticky request; nxinput never terminates the
 * process. The port must route this request through its normal safe shutdown. */
int nxinput_quit_requested(const nxinput_context *input);
int nxinput_consume_quit_request(nxinput_context *input);

/* The optional cursor derives only from right-stick + R3 while in MENU.
 * Controller state/latches remain available to the engine, so this adapter
 * never steals SDL events, D-pad, A, or any other native control. In GAMEPLAY,
 * right-stick and R3 have no cursor effect. Coordinates are normalized 0..1. */
void nxinput_set_cursor_context(nxinput_context *input,
                                nxinput_cursor_context context);
nxinput_cursor_context
nxinput_get_cursor_context(const nxinput_context *input);
int nxinput_cursor_warp(nxinput_context *input, unsigned int slot, float x,
                        float y);
int nxinput_cursor_update(nxinput_context *input, unsigned int slot,
                          float delta_seconds, nxinput_cursor_state *state);
int nxinput_cursor_update_with_options(nxinput_context *input,
                                       unsigned int slot,
                                       float delta_seconds,
                                       uint32_t options,
                                       nxinput_cursor_state *state);
int nxinput_cursor_consume_click(nxinput_context *input, unsigned int slot);

#ifdef __cplusplus
}
#endif

#endif
