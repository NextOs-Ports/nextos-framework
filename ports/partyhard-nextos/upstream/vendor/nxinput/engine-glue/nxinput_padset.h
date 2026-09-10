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
#include "nxinput_prerouter.h"

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

/* 0.11.1: monotonic clock (ns) for the pre-router window; NULL = CLOCK_MONOTONIC. */
typedef uint64_t (*nxinput_padset_clock_fn)(void);

typedef struct nxinput_padset {
  const nxinput_padset_sdl *sdl;
  void *pads[NXINPUT_PADSET_MAX];
  int32_t instances[NXINPUT_PADSET_MAX];
  unsigned count;
  /* 0.11.1 (D5/D7 wired): the sovereign START/SELECT pre-router runs INSIDE
   * the padset, per instance and device_instance_generation. A retained
   * edge is MASKED out of `buttons[]` (START never pauses before the chord
   * decides); a completed chord sets exit_pending and `chord_inputs` until
   * both buttons are physically released on that instance. */
  nxinput_prerouter pre;
  nxinput_padset_clock_fn clock;
  uint32_t generations[NXINPUT_PADSET_MAX];
  uint32_t next_generation;
  uint8_t raw_select[NXINPUT_PADSET_MAX], raw_start[NXINPUT_PADSET_MAX];
  uint8_t fwd_select[NXINPUT_PADSET_MAX], fwd_start[NXINPUT_PADSET_MAX];
  int exit_pending;
  int32_t exit_instance;
  unsigned exits;
  unsigned instance_races;   /* 0.11.1 (C5): opened instance != admitted instance */
  /* Result of the last sample(): */
  uint8_t buttons[NXINPUT_PADSET_BUTTON_MAX]; /* union */
  int chord_same_instance;                     /* one instance holds SELECT+START */
  int chord_cross_pad;                         /* SELECT and START on different pads only */
  int cross_pad_logged;
  nxinput_padset_log_fn log;
  int32_t primary_instance;   /* V5 (D2): 0 = first admitted */
  int primary_explicit;
  int32_t elected_instance;   /* 0.11.6: primary elected by activity in sample() */
  uint8_t elected_valid;
  unsigned overflowed;        /* V5 (D3): pads refused because the set was full */
  int overflow_logged;
  void *log_user;
} nxinput_padset;

/* Initialise with the caller's SDL vtable (all pointers required). Returns
 * 0, or -1 when a pointer is missing (the caller must fail closed). */
int nxinput_padset_init(nxinput_padset *set, const nxinput_padset_sdl *sdl,
                        nxinput_padset_log_fn log, void *log_user);
/* 0.11.1: inject the clock (tests); NULL restores CLOCK_MONOTONIC. */
void nxinput_padset_set_clock(nxinput_padset *set, nxinput_padset_clock_fn clock);
/* 0.11.1: the pre-router's exit request (one per completed chord). */
int nxinput_padset_exit_requested(const nxinput_padset *set);
unsigned nxinput_padset_instance_races(const nxinput_padset *set);
/* 0.11.1 (5.5 wired): the primary's whole vector, NORMALIZED once through
 * nxinput_axis_calib (SDL Sint16 domain, exact 0.0 at rest) and passed
 * through the radial deadzone `d` (0 <= d < 0.95; 0 = none). Returns 0 and
 * (0,0) when there is no primary, -1 on an invalid deadzone. The gesture
 * edge above this (nxinput_gptk_live neutral floor) sees a true zero. */
int nxinput_padset_vector_norm(const nxinput_padset *set, int axis_x, int axis_y,
                               float deadzone, float *x, float *y);

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
/* 0.11.4 (review 2, P2 -- universal): a TRIGGER of the primary pad, normalized
 * to [0,1] through the axis calibration (SDL Sint16 trigger domain 0..32767,
 * exact 0 at rest). Never a union across pads (the 3.3 defect of
 * nxinput_padset_axis). Pair it with nxinput_trigger_digital for the
 * hysteresis edge (6.5) instead of a per-port ENTER/EXIT pair. Returns 1
 * with *v01 set, 0 without a primary pad (v01 = 0), -1 on invalid input. */
int nxinput_padset_trigger_norm(const nxinput_padset *set, int axis, float *v01);

/*
 * V5 (D2): the PRIMARY pad and whole-vector sampling.
 *
 * `nxinput_padset_axis` above unions the largest magnitude PER AXIS across
 * every open pad, so X can come from one pad and Y from another (mission
 * 3.3 / 9bf3b3f negative path). It is kept for the legacy callers and for
 * the regression that proves the defect; new code uses the pair below.
 *
 * The primary is the pad that owns player 1: by default the first admitted
 * instance, or the instance explicitly set. Both components of a vector are
 * read from THAT instance only. A stick on any other pad never contributes.
 */
int nxinput_padset_set_primary(nxinput_padset *set, int32_t instance);
int32_t nxinput_padset_primary_instance(const nxinput_padset *set);
/* Returns 0 and writes (0,0) when there is no primary. */
int nxinput_padset_vector(const nxinput_padset *set, int axis_x, int axis_y,
                          int16_t *x, int16_t *y);
/* V5 (D3): pads that arrived when the set was full are COUNTED and logged
 * once, never silently dropped. */
unsigned nxinput_padset_overflowed(const nxinput_padset *set);

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
