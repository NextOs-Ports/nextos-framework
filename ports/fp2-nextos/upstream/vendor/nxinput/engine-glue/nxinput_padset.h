/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_padset -- nxinput 0.10.2: every admitted SDL game controller at
 * once, with the exit chord bound to ONE instance.
 *
 * WHY
 *   A port that opens a single controller cannot be proven by an automated
 *   on-device pad (the real pad is index 0 and the uinput clone the proof
 *   creates would be ignored), and it cannot honour the rule that
 *   SELECT on one pad plus START on another never ends the game. Both are
 *   framework concerns, so they live here once instead of inside each
 *   adapter.
 *
 * WHAT
 *   - up to NXINPUT_PADSET_MAX pads open at the same time, each admitted by
 *     the caller's authority (the C6 seam) through a callback -- this module
 *     never decides admission and never looks at a name, VID/PID or GUID;
 *   - the symbolic button state is the UNION of the pads, an axis is the
 *     largest deflection among them (a resting pad never cancels another);
 *   - the exit chord inputs are true only while a SINGLE instance holds
 *     SELECT and START together; SELECT here + START there is denied and
 *     reported once per occurrence through the caller's log callback;
 *   - hotplug removal closes only that instance and compacts the set.
 *
 * HOW IT STAYS TESTABLE
 *   Every SDL call goes through `nxinput_padset_sdl`, a vtable the caller
 *   fills from the SDL it already resolved (the firmware SDL, never a private
 *   one). Host gates inject fakes and prove union, max-axis, same-instance
 *   chord, cross-pad denial and compaction without a device.
 */
#ifndef NXINPUT_PADSET_H
#define NXINPUT_PADSET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_PADSET_MAX 4u
#define NXINPUT_PADSET_BUTTON_MAX 21u /* SDL_CONTROLLER_BUTTON_MAX (SDL 2.0.14+) */
#define NXINPUT_PADSET_BUTTON_BACK 4  /* SDL_CONTROLLER_BUTTON_BACK */
#define NXINPUT_PADSET_BUTTON_START 6 /* SDL_CONTROLLER_BUTTON_START */
#define NXINPUT_PADSET_MARKER "nxinput-padset/1"

typedef struct nxinput_padset_sdl {
  int (*num_joysticks)(void);
  int32_t (*instance_for_index)(int index);       /* SDL_JoystickGetDeviceInstanceID */
  int (*is_game_controller)(int index);           /* SDL_IsGameController */
  void *(*open)(int index);                       /* SDL_GameControllerOpen */
  void (*close)(void *controller);                /* SDL_GameControllerClose */
  void *(*get_joystick)(void *controller);        /* SDL_GameControllerGetJoystick */
  int32_t (*joystick_instance)(void *joystick);   /* SDL_JoystickInstanceID */
  void (*update)(void);                           /* SDL_GameControllerUpdate */
  uint8_t (*get_button)(void *controller, int button); /* SDL_GameControllerGetButton */
  int16_t (*get_axis)(void *controller, int axis);     /* SDL_GameControllerGetAxis */
} nxinput_padset_sdl;

/* Called for each SDL index not yet open. Return 1 to admit, 0 to refuse.
 * This is where the port runs its C6 admission (authority order). */
typedef int (*nxinput_padset_admit_fn)(int sdl_index, void *user);
/* Called once per newly opened pad (slot == 0 is the first). */
typedef void (*nxinput_padset_opened_fn)(int sdl_index, unsigned slot,
                                         void *controller, void *user);
/* Bounded log line (no newline needed). */
typedef void (*nxinput_padset_log_fn)(const char *line, void *user);

typedef struct nxinput_padset {
  const nxinput_padset_sdl *sdl;
  void *pads[NXINPUT_PADSET_MAX];
  int32_t instances[NXINPUT_PADSET_MAX];
  unsigned count;
  /* Result of the last sample(): */
  uint8_t buttons[NXINPUT_PADSET_BUTTON_MAX]; /* union */
  int chord_same_instance;                     /* one instance holds SELECT+START */
  int chord_cross_pad;                         /* SELECT and START on different pads only */
  int cross_pad_logged;
  nxinput_padset_log_fn log;
  void *log_user;
} nxinput_padset;

/* Initialise with the caller's SDL vtable (all pointers required). Returns
 * 0, or -1 when a pointer is missing (the caller must fail closed). */
int nxinput_padset_init(nxinput_padset *set, const nxinput_padset_sdl *sdl,
                        nxinput_padset_log_fn log, void *log_user);

/* Open every admitted controller not yet open. Returns the number of pads
 * opened by this call. */
unsigned nxinput_padset_open_all(nxinput_padset *set,
                                 nxinput_padset_admit_fn admit,
                                 nxinput_padset_opened_fn opened, void *user);

/* Close one instance (hotplug removal) and compact. Returns 1 if it was open. */
int nxinput_padset_remove_instance(nxinput_padset *set, int32_t instance);

void nxinput_padset_close_all(nxinput_padset *set);

/* First pad or NULL (compatibility for callers that expose "the" controller). */
void *nxinput_padset_first(const nxinput_padset *set);

/* Sample every pad once: union of buttons, same-instance chord, cross-pad
 * denial (logged once per occurrence). Call once per frame. */
void nxinput_padset_sample(nxinput_padset *set);

/* Largest deflection of `axis` among the pads (0 when none). */
int16_t nxinput_padset_axis(const nxinput_padset *set, int axis);

/* Exit-chord inputs: both are 1 only while ONE instance holds SELECT and
 * START together; otherwise both are 0 (a lone SELECT or START stays a
 * plain native button for the adapter's own state machine). */
void nxinput_padset_chord_inputs(const nxinput_padset *set, int *select_down,
                                 int *start_down);

const char *nxinput_padset_marker(void);

#ifdef __cplusplus
}
#endif
#endif /* NXINPUT_PADSET_H */
