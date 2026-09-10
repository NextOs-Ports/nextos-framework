/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * c6_consumer -- V4-CONTROLLERS-03 / C6: a REAL SDL GameController / Gamepad
 * application, linked against the seam-carrying SDL under test.
 *
 * WHAT MAKES THIS EVIDENCE AND NOT A HARNESS
 * ------------------------------------------
 * Everything this program reports is an answer the SDL library itself gave,
 * in this process, through its public API, about pads the kernel really
 * created: SDL_IsGameController/SDL_IsGamepad for the classification,
 * SDL_GameControllerGetBindForButton/SDL_GetGamepadBindings for the binding
 * table, the event queue for the event path and
 * SDL_GameControllerGetButton/SDL_GetGamepadButton for the polling path.
 * The program contributes the questions and the transcript, not the answers.
 * It never re-derives a mapping and never decides an admission.
 *
 * ONE SOURCE, BOTH MAJORS. The SDL2 and SDL3 evidence has to mean the same
 * thing to be comparable at all, so the two builds differ only in the
 * spelling of the calls, which is confined to the shims below.
 *
 * The 18 V2 control groups are all reported, every run, whether or not the
 * mapping binds them -- a group nobody bound must be provably SILENT, which
 * is exactly what the A/B=null case turns on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef C6_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "nxinput_sdl_seam.h"

#define C6_MAX_PADS 4
#define C6_MAX_EVENTS 4096

/* ------------------------------------------------------------- the shims */
#ifdef C6_SDL3
typedef SDL_Gamepad *c6_pad;
typedef SDL_JoystickID c6_id;
#define C6_API_NAME "sdl3"
#define C6_BTN_A SDL_GAMEPAD_BUTTON_SOUTH
#define C6_BTN_B SDL_GAMEPAD_BUTTON_EAST
#define C6_BTN_X SDL_GAMEPAD_BUTTON_WEST
#define C6_BTN_Y SDL_GAMEPAD_BUTTON_NORTH
#define C6_BTN_BACK SDL_GAMEPAD_BUTTON_BACK
#define C6_BTN_GUIDE SDL_GAMEPAD_BUTTON_GUIDE
#define C6_BTN_START SDL_GAMEPAD_BUTTON_START
#define C6_BTN_LSTICK SDL_GAMEPAD_BUTTON_LEFT_STICK
#define C6_BTN_RSTICK SDL_GAMEPAD_BUTTON_RIGHT_STICK
#define C6_BTN_LSHOULDER SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
#define C6_BTN_RSHOULDER SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
#define C6_BTN_UP SDL_GAMEPAD_BUTTON_DPAD_UP
#define C6_BTN_DOWN SDL_GAMEPAD_BUTTON_DPAD_DOWN
#define C6_BTN_LEFT SDL_GAMEPAD_BUTTON_DPAD_LEFT
#define C6_BTN_RIGHT SDL_GAMEPAD_BUTTON_DPAD_RIGHT
#define C6_AX_LEFTX SDL_GAMEPAD_AXIS_LEFTX
#define C6_AX_LEFTY SDL_GAMEPAD_AXIS_LEFTY
#define C6_AX_RIGHTX SDL_GAMEPAD_AXIS_RIGHTX
#define C6_AX_RIGHTY SDL_GAMEPAD_AXIS_RIGHTY
#define C6_AX_LTRIGGER SDL_GAMEPAD_AXIS_LEFT_TRIGGER
#define C6_AX_RTRIGGER SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
#define C6_EV_BTN_DOWN SDL_EVENT_GAMEPAD_BUTTON_DOWN
#define C6_EV_BTN_UP SDL_EVENT_GAMEPAD_BUTTON_UP
#define C6_EV_AXIS SDL_EVENT_GAMEPAD_AXIS_MOTION
#define C6_EV_ADDED SDL_EVENT_JOYSTICK_ADDED
#define C6_EV_REMOVED SDL_EVENT_JOYSTICK_REMOVED
#define C6_EV_KEYDOWN SDL_EVENT_KEY_DOWN
#define c6_get_button(p, b) (SDL_GetGamepadButton((p), (b)) ? 1 : 0)
#define c6_get_axis(p, a) SDL_GetGamepadAxis((p), (a))
#define c6_close(p) SDL_CloseGamepad(p)
#define c6_pad_id(p) SDL_GetGamepadID(p)
#define C6_BTNEV gbutton
#define C6_AXEV gaxis
#else
typedef SDL_GameController *c6_pad;
typedef SDL_JoystickID c6_id;
#define C6_API_NAME "sdl2"
#define C6_BTN_A SDL_CONTROLLER_BUTTON_A
#define C6_BTN_B SDL_CONTROLLER_BUTTON_B
#define C6_BTN_X SDL_CONTROLLER_BUTTON_X
#define C6_BTN_Y SDL_CONTROLLER_BUTTON_Y
#define C6_BTN_BACK SDL_CONTROLLER_BUTTON_BACK
#define C6_BTN_GUIDE SDL_CONTROLLER_BUTTON_GUIDE
#define C6_BTN_START SDL_CONTROLLER_BUTTON_START
#define C6_BTN_LSTICK SDL_CONTROLLER_BUTTON_LEFTSTICK
#define C6_BTN_RSTICK SDL_CONTROLLER_BUTTON_RIGHTSTICK
#define C6_BTN_LSHOULDER SDL_CONTROLLER_BUTTON_LEFTSHOULDER
#define C6_BTN_RSHOULDER SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
#define C6_BTN_UP SDL_CONTROLLER_BUTTON_DPAD_UP
#define C6_BTN_DOWN SDL_CONTROLLER_BUTTON_DPAD_DOWN
#define C6_BTN_LEFT SDL_CONTROLLER_BUTTON_DPAD_LEFT
#define C6_BTN_RIGHT SDL_CONTROLLER_BUTTON_DPAD_RIGHT
#define C6_AX_LEFTX SDL_CONTROLLER_AXIS_LEFTX
#define C6_AX_LEFTY SDL_CONTROLLER_AXIS_LEFTY
#define C6_AX_RIGHTX SDL_CONTROLLER_AXIS_RIGHTX
#define C6_AX_RIGHTY SDL_CONTROLLER_AXIS_RIGHTY
#define C6_AX_LTRIGGER SDL_CONTROLLER_AXIS_TRIGGERLEFT
#define C6_AX_RTRIGGER SDL_CONTROLLER_AXIS_TRIGGERRIGHT
#define C6_EV_BTN_DOWN SDL_CONTROLLERBUTTONDOWN
#define C6_EV_BTN_UP SDL_CONTROLLERBUTTONUP
#define C6_EV_AXIS SDL_CONTROLLERAXISMOTION
#define C6_EV_ADDED SDL_JOYDEVICEADDED
#define C6_EV_REMOVED SDL_JOYDEVICEREMOVED
#define C6_EV_KEYDOWN SDL_KEYDOWN
#define c6_get_button(p, b) (SDL_GameControllerGetButton((p), (b)) ? 1 : 0)
#define c6_get_axis(p, a) SDL_GameControllerGetAxis((p), (a))
#define c6_close(p) SDL_GameControllerClose(p)
#define c6_pad_id(p) \
  SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p))
#define C6_BTNEV cbutton
#define C6_AXEV caxis
#endif

/* The 18 V2 control groups, in the C2 order. LEFT_STICK and RIGHT_STICK are
 * axis pairs; everything else is a button. GUIDE is reported too, outside
 * the eighteen, because a mapping may bind it and silence must be provable
 * for it as well. */
struct group {
  const char *name;
  int is_axis_pair;
  int a; /* button id, or first axis id */
  int b; /* second axis id, or -1 */
};

static const struct group GROUPS[] = {
    {"A", 0, C6_BTN_A, -1},
    {"B", 0, C6_BTN_B, -1},
    {"X", 0, C6_BTN_X, -1},
    {"Y", 0, C6_BTN_Y, -1},
    {"L1", 0, C6_BTN_LSHOULDER, -1},
    {"R1", 0, C6_BTN_RSHOULDER, -1},
    {"L2", 1, C6_AX_LTRIGGER, -1},
    {"R2", 1, C6_AX_RTRIGGER, -1},
    {"L3", 0, C6_BTN_LSTICK, -1},
    {"R3", 0, C6_BTN_RSTICK, -1},
    {"START", 0, C6_BTN_START, -1},
    {"SELECT", 0, C6_BTN_BACK, -1},
    {"DPAD_UP", 0, C6_BTN_UP, -1},
    {"DPAD_DOWN", 0, C6_BTN_DOWN, -1},
    {"DPAD_LEFT", 0, C6_BTN_LEFT, -1},
    {"DPAD_RIGHT", 0, C6_BTN_RIGHT, -1},
    {"LEFT_STICK", 1, C6_AX_LEFTX, C6_AX_LEFTY},
    {"RIGHT_STICK", 1, C6_AX_RIGHTX, C6_AX_RIGHTY},
    {"GUIDE", 0, C6_BTN_GUIDE, -1},
};
#define C6_GROUP_COUNT ((int)(sizeof(GROUPS) / sizeof(GROUPS[0])))

struct pad {
  int in_use;
  c6_id id;
  c6_pad handle;
  char guid[33];
  int is_gamepad;      /* what SDL classified it as */
  int opened;
  /* The chord state, per pad. SELECT+START must be the SAME pad. */
  int select_down;
  int start_down;
  int l2_down;
  int r2_down;
  int chord_fired;
};

static struct pad g_pads[C6_MAX_PADS];
static FILE *g_out;
static FILE *g_consumer_receipt;
static unsigned long g_deliveries;
static int g_keyboard_events;

static struct pad *pad_for(c6_id id) {
  int i;
  for (i = 0; i < C6_MAX_PADS; i++) {
    if (g_pads[i].in_use && g_pads[i].id == id) {
      return &g_pads[i];
    }
  }
  return NULL;
}

static struct pad *pad_free(void) {
  int i;
  for (i = 0; i < C6_MAX_PADS; i++) {
    if (!g_pads[i].in_use) {
      return &g_pads[i];
    }
  }
  return NULL;
}

/*
 * THE CONSUMER RECEIPT.
 *
 * It is written HERE, in the code that actually received the semantic state
 * and would act on it -- not by the framework and not by the driver. That is
 * the whole point: a counter incremented next to the engine proves nothing
 * about the engine having consumed anything. If this call is not reached, no
 * receipt exists, and the run says so.
 */
static void consume(const struct pad *p, const char *group, const char *path,
                    int pressed, int value) {
  g_deliveries++;
  if (g_consumer_receipt != NULL) {
    (void)fprintf(g_consumer_receipt,
                  "NXC6-CONSUMER api=%s instance=%ld group=%s path=%s "
                  "pressed=%d value=%d n=%lu\n",
                  C6_API_NAME, (long)p->id, group, path, pressed, value,
                  g_deliveries);
    (void)fflush(g_consumer_receipt);
  }
}

static const char *button_group(int button) {
  int i;
  for (i = 0; i < C6_GROUP_COUNT; i++) {
    if (!GROUPS[i].is_axis_pair && GROUPS[i].a == button) {
      return GROUPS[i].name;
    }
  }
  return NULL;
}

static const char *axis_group(int axis, int *which) {
  int i;
  for (i = 0; i < C6_GROUP_COUNT; i++) {
    if (GROUPS[i].is_axis_pair) {
      if (GROUPS[i].a == axis) {
        *which = 0;
        return GROUPS[i].name;
      }
      if (GROUPS[i].b == axis) {
        *which = 1;
        return GROUPS[i].name;
      }
    }
  }
  return NULL;
}

/* ------------------------------------------------------ enumeration shim */

static int c6_enumerate(c6_id *ids, int cap) {
#ifdef C6_SDL3
  int count = 0;
  int i;
  SDL_JoystickID *list = SDL_GetJoysticks(&count);
  if (list == NULL) {
    return 0;
  }
  if (count > cap) {
    count = cap;
  }
  for (i = 0; i < count; i++) {
    ids[i] = list[i];
  }
  SDL_free(list);
  return count;
#else
  int count = SDL_NumJoysticks();
  int i;
  if (count > cap) {
    count = cap;
  }
  for (i = 0; i < count; i++) {
    ids[i] = (c6_id)i; /* SDL2 addresses by device index at this stage */
  }
  return count;
#endif
}

static void c6_guid_string(c6_id which, char *out, size_t cap) {
#ifdef C6_SDL3
  SDL_GUID guid = SDL_GetJoystickGUIDForID(which);
  SDL_GUIDToString(guid, out, (int)cap);
#else
  SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID((int)which);
  SDL_JoystickGetGUIDString(guid, out, (int)cap);
#endif
}

static int c6_is_gamepad(c6_id which) {
#ifdef C6_SDL3
  return SDL_IsGamepad(which) ? 1 : 0;
#else
  return SDL_IsGameController((int)which) ? 1 : 0;
#endif
}

static c6_pad c6_open(c6_id which) {
#ifdef C6_SDL3
  return SDL_OpenGamepad(which);
#else
  return SDL_GameControllerOpen((int)which);
#endif
}

/* The binding table SDL itself resolved. This is a READBACK of the engine's
 * own state, not a re-derivation of the mapping text. */
static void dump_bindings(struct pad *p) {
  int i;
  (void)fprintf(g_out, "  \"bindings\": {");
  for (i = 0; i < C6_GROUP_COUNT; i++) {
    const struct group *g = &GROUPS[i];
    char detail[96];
    detail[0] = '\0';
#ifdef C6_SDL3
    {
      int count = 0;
      SDL_GamepadBinding **binds = SDL_GetGamepadBindings(p->handle, &count);
      int k;
      if (binds != NULL) {
        for (k = 0; k < count; k++) {
          const SDL_GamepadBinding *b = binds[k];
          if (!g->is_axis_pair &&
              b->output_type == SDL_GAMEPAD_BINDTYPE_BUTTON &&
              b->output.button == g->a) {
            (void)snprintf(detail, sizeof detail, "in%d", (int)b->input_type);
          } else if (g->is_axis_pair &&
                     b->output_type == SDL_GAMEPAD_BINDTYPE_AXIS &&
                     b->output.axis.axis == g->a) {
            (void)snprintf(detail, sizeof detail, "in%d", (int)b->input_type);
          }
        }
        SDL_free(binds);
      }
    }
#else
    {
      SDL_GameControllerButtonBind b;
      if (g->is_axis_pair) {
        b = SDL_GameControllerGetBindForAxis(p->handle,
                                             (SDL_GameControllerAxis)g->a);
      } else {
        b = SDL_GameControllerGetBindForButton(p->handle,
                                               (SDL_GameControllerButton)g->a);
      }
      if (b.bindType != SDL_CONTROLLER_BINDTYPE_NONE) {
        (void)snprintf(detail, sizeof detail, "in%d", (int)b.bindType);
      }
    }
#endif
    (void)fprintf(g_out, "%s\"%s\": %s%s%s", i ? ", " : "", g->name,
                  detail[0] ? "\"" : "", detail[0] ? detail : "null",
                  detail[0] ? "\"" : "");
  }
  (void)fprintf(g_out, "},\n");
}

/* The polled state of all 18 groups RIGHT NOW, straight from SDL. This is
 * the polling path, kept strictly separate from the event path: a game that
 * polls must see the same control the event path reported, and a group
 * nobody bound must read as silent in BOTH. */
static void dump_poll(struct pad *p, const char *tag) {
  int i;
  (void)fprintf(g_out, "  {\"kind\": \"poll\", \"tag\": \"%s\", "
                       "\"instance\": %ld, \"state\": {",
                tag, (long)p->id);
  for (i = 0; i < C6_GROUP_COUNT; i++) {
    const struct group *g = &GROUPS[i];
    if (g->is_axis_pair) {
      if (g->b >= 0) {
        (void)fprintf(g_out, "%s\"%s\": [%d, %d]", i ? ", " : "", g->name,
                      (int)c6_get_axis(p->handle, g->a),
                      (int)c6_get_axis(p->handle, g->b));
      } else {
        (void)fprintf(g_out, "%s\"%s\": %d", i ? ", " : "", g->name,
                      (int)c6_get_axis(p->handle, g->a));
      }
    } else {
      (void)fprintf(g_out, "%s\"%s\": %d", i ? ", " : "", g->name,
                    c6_get_button(p->handle, g->a));
    }
  }
  (void)fprintf(g_out, "}},\n");
}

/* ---------------------------------------------------------------- chord */
/*
 * SELECT+START, on the SAME pad, is the exit chord. Two boundaries matter and
 * both are checked here rather than asserted in a document:
 *   - L2+R2 must NEVER fire it, whatever the mapping does with the triggers;
 *   - SELECT on one pad and START on another must NEVER fire it either.
 * The per-pad state above is what makes the second one structural instead of
 * a hopeful comparison.
 */
static void chord_update(struct pad *p, const char *group, int pressed) {
  if (strcmp(group, "SELECT") == 0) {
    p->select_down = pressed;
  } else if (strcmp(group, "START") == 0) {
    p->start_down = pressed;
  } else if (strcmp(group, "L2") == 0) {
    p->l2_down = pressed;
  } else if (strcmp(group, "R2") == 0) {
    p->r2_down = pressed;
  }
  if (p->select_down && p->start_down && !p->chord_fired) {
    p->chord_fired = 1;
    (void)fprintf(g_out,
                  "  {\"kind\": \"chord\", \"instance\": %ld, "
                  "\"reason\": \"select+start\"},\n",
                  (long)p->id);
  }
}

int main(int argc, char **argv) {
  const char *out_path = NULL;
  const char *scenario = "matrix";
  const char *consumer_path = getenv("NXC6_CONSUMER_RECEIPT");
  char staged[NXINPUT_SOVEREIGN_LINE_MAX * 8];
  size_t staged_len = 0;
  int seconds = 8;
  int i;
  int stage_rc;
  Uint64 deadline;
  c6_id ids[C6_MAX_PADS];
  int enumerated;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      scenario = argv[++i];
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = atoi(argv[++i]);
    }
  }
  if (out_path == NULL) {
    (void)fprintf(stderr, "c6_consumer: --out is required\n");
    return 2;
  }
  g_out = fopen(out_path, "w");
  if (g_out == NULL) {
    return 2;
  }
  if (consumer_path != NULL && *consumer_path != '\0') {
    g_consumer_receipt = fopen(consumer_path, "a");
  }

  (void)fprintf(g_out, "{\n  \"api\": \"%s\",\n  \"scenario\": \"%s\",\n",
                C6_API_NAME, scenario);

  /* BOUNDARY 1: staging, BEFORE SDL_Init. Left in the environment, SDL
   * imports SDL_GAMECONTROLLERCONFIG at USER priority during init and
   * outranks anything the seam installs later. */
  {
    nxinput_sdl_seam_env_ops env;
    memset(&env, 0, sizeof env);
    env.api_version = NXINPUT_SDL_SEAM_API_VERSION;
    env.struct_size = sizeof env;
    env.getenv_fn = NULL;
    /* Small local shims so the module stays free of libc policy. */
    {
      extern const char *c6_env_get(void *, const char *);
      extern int c6_env_unset(void *, const char *);
      extern int c6_was_init(void *);
      env.getenv_fn = c6_env_get;
      env.unsetenv_fn = c6_env_unset;
      env.sdl_was_init_fn = c6_was_init;
    }
    stage_rc = nxinput_sdl_seam_stage_before_init(&env, staged, sizeof staged,
                                                  &staged_len);
    if (stage_rc == 0 && staged_len > 0u) {
      (void)setenv("NXC6_STAGED_MAPPING", staged, 1);
    }
  }
  (void)fprintf(g_out,
                "  \"staging\": {\"rc\": %d, \"len\": %lu, "
                "\"env_still_set\": %s},\n",
                stage_rc, (unsigned long)staged_len,
                getenv("SDL_GAMECONTROLLERCONFIG") != NULL ? "true" : "false");

#ifdef C6_SDL3
  if (!SDL_Init(SDL_INIT_GAMEPAD)) {
#else
  if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
#endif
    (void)fprintf(g_out, "  \"init_error\": \"%s\"\n}\n", SDL_GetError());
    (void)fclose(g_out);
    return 3;
  }

  /* Let the backend settle: detection is asynchronous in both majors. */
  for (i = 0; i < 40; i++) {
    SDL_PumpEvents();
    SDL_Delay(25);
  }

  enumerated = c6_enumerate(ids, C6_MAX_PADS);
  /*
   * The JOYSTICK-level count, reported separately and on purpose.
   *
   * SDL_Joystick is a different API from SDL_GameController/SDL_Gamepad, and
   * a game may use it directly. The seam sits before the announce, so a
   * device it refuses never enters the joystick list either -- and that is
   * measurable right here, in the real library, rather than argued. What it
   * does NOT give is per-control suppression on that API: an unbound `a` is
   * a GameController/Gamepad concept, and a raw joystick button has no such
   * notion. C6 reports the first and refuses to claim the second.
   */
  (void)fprintf(g_out, "  \"joysticks_visible\": %d,\n", enumerated);
  (void)fprintf(g_out, "  \"enumerated\": %d,\n  \"devices\": [\n",
                enumerated);
  for (i = 0; i < enumerated; i++) {
    struct pad *p = pad_free();
    char guid[33];
    int classified;

    memset(guid, 0, sizeof guid);
    c6_guid_string(ids[i], guid, sizeof guid);
    /* Classification is asked of SDL, and the seam already ran: a device the
     * seam refused is not in this list at all. */
    classified = c6_is_gamepad(ids[i]);
    (void)fprintf(g_out,
                  "    {\"guid\": \"%s\", \"is_gamepad\": %s, \"opened\": ",
                  guid, classified ? "true" : "false");
    if (p == NULL || !classified) {
      (void)fprintf(g_out, "false}%s\n", i + 1 < enumerated ? "," : "");
      continue;
    }
    p->handle = c6_open(ids[i]);
    if (p->handle == NULL) {
      (void)fprintf(g_out, "false, \"open_error\": \"%s\"}%s\n",
                    SDL_GetError(), i + 1 < enumerated ? "," : "");
      continue;
    }
    p->in_use = 1;
    p->id = c6_pad_id(p->handle);
    p->is_gamepad = classified;
    p->opened = 1;
    memcpy(p->guid, guid, sizeof p->guid);
    (void)fprintf(g_out, "true, \"instance\": %ld}%s\n", (long)p->id,
                  i + 1 < enumerated ? "," : "");
  }
  (void)fprintf(g_out, "  ],\n");

  for (i = 0; i < C6_MAX_PADS; i++) {
    if (g_pads[i].in_use) {
      dump_bindings(&g_pads[i]);
      break;
    }
  }

  (void)fprintf(g_out, "  \"trace\": [\n");
  for (i = 0; i < C6_MAX_PADS; i++) {
    if (g_pads[i].in_use) {
      dump_poll(&g_pads[i], "baseline");
    }
  }

  deadline = SDL_GetTicks() + (Uint64)seconds * 1000u;
  while (SDL_GetTicks() < deadline) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == C6_EV_KEYDOWN) {
        /* A pad must never arrive as a keyboard. Counted, never ignored. */
        g_keyboard_events++;
        (void)fprintf(g_out,
                      "  {\"kind\": \"keyboard\", \"note\": "
                      "\"synthetic keyboard event observed\"},\n");
      } else if (e.type == C6_EV_BTN_DOWN || e.type == C6_EV_BTN_UP) {
        struct pad *p = pad_for(e.C6_BTNEV.which);
        const char *group = button_group((int)e.C6_BTNEV.button);
        int pressed = (e.type == C6_EV_BTN_DOWN) ? 1 : 0;
        if (p != NULL && group != NULL) {
          (void)fprintf(g_out,
                        "  {\"kind\": \"event\", \"path\": \"button\", "
                        "\"instance\": %ld, \"group\": \"%s\", "
                        "\"pressed\": %d, \"poll\": %d},\n",
                        (long)p->id, group, pressed,
                        c6_get_button(p->handle, GROUPS[0].a));
          consume(p, group, "button", pressed, pressed);
          chord_update(p, group, pressed);
          dump_poll(p, pressed ? "after-press" : "after-release");
        }
      } else if (e.type == C6_EV_AXIS) {
        struct pad *p = pad_for(e.C6_AXEV.which);
        int which = 0;
        const char *group = axis_group((int)e.C6_AXEV.axis, &which);
        if (p != NULL && group != NULL) {
          int value = (int)e.C6_AXEV.value;
          (void)fprintf(g_out,
                        "  {\"kind\": \"event\", \"path\": \"axis\", "
                        "\"instance\": %ld, \"group\": \"%s\", "
                        "\"half\": %d, \"value\": %d, \"poll\": %d},\n",
                        (long)p->id, group, which, value,
                        (int)c6_get_axis(p->handle, GROUPS[6].a));
          consume(p, group, "axis", value != 0, value);
          /* A trigger is a control too: past the threshold it is "down", and
           * L2+R2 together must still not be the chord. */
          if (strcmp(group, "L2") == 0 || strcmp(group, "R2") == 0) {
            chord_update(p, group, value > 16384 ? 1 : 0);
          }
        }
      } else if (e.type == C6_EV_ADDED) {
        /* A pad that arrives after startup gets the SAME treatment as one
         * that was there first: classified by SDL, then opened. Without this
         * a reconnected pad would look silent for the trivial reason that
         * nobody ever opened it, and the hotplug case would prove nothing. */
        c6_id which;
        struct pad *p;
        char guid[33];
        int classified;
#ifdef C6_SDL3
        which = e.jdevice.which;
#else
        which = (c6_id)e.jdevice.which; /* SDL2 reports a DEVICE INDEX here */
#endif
        memset(guid, 0, sizeof guid);
        c6_guid_string(which, guid, sizeof guid);
        classified = c6_is_gamepad(which);
        p = pad_free();
        (void)fprintf(g_out,
                      "  {\"kind\": \"added\", \"guid\": \"%s\", "
                      "\"is_gamepad\": %s, \"opened\": ",
                      guid, classified ? "true" : "false");
        if (p != NULL && classified) {
          p->handle = c6_open(which);
          if (p->handle != NULL) {
            p->in_use = 1;
            p->id = c6_pad_id(p->handle);
            p->is_gamepad = 1;
            p->opened = 1;
            memcpy(p->guid, guid, sizeof p->guid);
            (void)fprintf(g_out, "true, \"instance\": %ld},\n",
                          (long)p->id);
          } else {
            (void)fprintf(g_out, "false},\n");
          }
        } else {
          (void)fprintf(g_out, "false},\n");
        }
      } else if (e.type == C6_EV_REMOVED) {
        struct pad *p = pad_for(
#ifdef C6_SDL3
            e.jdevice.which
#else
            (c6_id)e.jdevice.which
#endif
        );
        (void)fprintf(g_out, "  {\"kind\": \"removed\", \"instance\": %ld},\n",
                      p != NULL ? (long)p->id : -1L);
        if (p != NULL) {
          if (p->handle != NULL) {
            c6_close(p->handle);
          }
          memset(p, 0, sizeof *p);
        }
      }
    }
    SDL_Delay(4);
  }

  for (i = 0; i < C6_MAX_PADS; i++) {
    if (g_pads[i].in_use) {
      dump_poll(&g_pads[i], "final");
    }
  }
  (void)fprintf(g_out, "  {\"kind\": \"end\"}\n  ],\n");
  (void)fprintf(g_out,
                "  \"deliveries\": %lu,\n  \"keyboard_events\": %d,\n"
                "  \"chord_fired\": %d\n}\n",
                g_deliveries, g_keyboard_events,
                (g_pads[0].chord_fired || g_pads[1].chord_fired ||
                 g_pads[2].chord_fired || g_pads[3].chord_fired));
  (void)fclose(g_out);
  if (g_consumer_receipt != NULL) {
    (void)fclose(g_consumer_receipt);
  }
  for (i = 0; i < C6_MAX_PADS; i++) {
    if (g_pads[i].in_use && g_pads[i].handle != NULL) {
      c6_close(g_pads[i].handle);
    }
  }
  SDL_Quit();
  return 0;
}

const char *c6_env_get(void *userdata, const char *name) {
  (void)userdata;
  return getenv(name);
}

int c6_env_unset(void *userdata, const char *name) {
  (void)userdata;
  return unsetenv(name);
}

int c6_was_init(void *userdata) {
  (void)userdata;
  return SDL_WasInit(0) != 0 ? 1 : 0;
}
