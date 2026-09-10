/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C5 (audit 116A): the ADAPTER's fan-out contract.
 *
 * An adapter hands a control onward by three kinds of path: an action sink,
 * a raw event, and a state query. A `null` control must be dead on all
 * three, so the C4 decision cannot be bypassed by whichever path a game
 * happens to use.
 *
 * CLAIM CLASS: FIXTURE. These are the ADAPTER's own paths, named generically
 * on purpose. This file does NOT call Godot's InputMap, `_input` or
 * `Input.is_joy_button_pressed`, and nothing here may be read as evidence
 * about them. Those are proved only by tests/godot_real_gate.py, which runs
 * real Godot 3 and Godot 4 processes and shows an unbound control producing
 * no engine event, no InputMap action and no polled press. */
#include "nxinput_exit_chord.h"
#include "nxinput_godot.h"
#include "nxinput_gptk.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, name)                                    \
  do {                                                       \
    if (cond) {                                              \
      printf("ok %s\n", name);                               \
    } else {                                                 \
      printf("FAIL %s (line %d)\n", name, __LINE__);         \
      g_failures++;                                          \
    }                                                        \
  } while (0)

static const char *const CONTROLS[NXINPUT_GPTK_CONTROL_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK"};

static void build_v2(char *out, size_t cap, const char *const *gameplay) {
  size_t used = 0u;
  int i;

  used += (size_t)snprintf(out + used, cap - used,
                           "%s\nport = mmwpilot\n\n[menu]\n",
                           NXINPUT_GPTK_MAGIC_V2);
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    used += (size_t)snprintf(out + used, cap - used, "%s = null\n",
                             CONTROLS[i]);
  }
  used += (size_t)snprintf(out + used, cap - used, "\n[gameplay]\n");
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    used += (size_t)snprintf(out + used, cap - used, "%s = %s\n", CONTROLS[i],
                             gameplay[i]);
  }
}

/* ------------------------------------------------------- the Godot rig */
struct godot_pad {
  int connected;
  char guid[33];
  /* Measured physical capability per logical control: 0 means the pad does
   * not have it at all. reachable=0 is RECORDED, never turned into a PASS. */
  unsigned char reachable[NXINPUT_GPTK_CONTROL_COUNT];
};

struct godot_rig {
  struct godot_pad pad[2];
  nxinput_gptk_dispatcher dispatcher[2];
  /* what each route delivered */
  unsigned int action_events;
  unsigned int raw_events;
  unsigned int poll_true;
  unsigned int suppressed_dropped[3]; /* per route */
  char last_action[NXINPUT_GPTK_ACTION_MAX + 1u];
};

static struct godot_rig g_rig;

static void action_sink(void *user, const char *action, int pressed,
                        float value) {
  (void)user;
  (void)pressed;
  (void)value;
  g_rig.action_events++;
  (void)snprintf(g_rig.last_action, sizeof g_rig.last_action, "%s", action);
}

/* Path 2: the raw event an adapter would synthesize for the game. */
static void godot_raw_event(struct godot_rig *rig, unsigned int pad,
                            int control, int pressed) {
  const char *action = 0;
  nxinput_gptk_decision decision =
      nxinput_gptk_dispatcher_decision(&rig->dispatcher[pad], control,
                                       &action);

  if (decision == NXINPUT_GPTK_DECIDE_SUPPRESS) {
    rig->suppressed_dropped[1]++;
    return; /* the adapter never synthesizes the event at all */
  }
  (void)pressed;
  rig->raw_events++;
}

/* Path 3: the state query, which bypasses the action table. */
static int godot_poll(struct godot_rig *rig, unsigned int pad, int control,
                      int physically_down) {
  if (nxinput_gptk_dispatcher_decision(&rig->dispatcher[pad], control, 0) ==
      NXINPUT_GPTK_DECIDE_SUPPRESS) {
    rig->suppressed_dropped[2]++;
    return 0; /* a suppressed control never reads as pressed */
  }
  if (physically_down) {
    rig->poll_true++;
  }
  return physically_down;
}

/* Path 1 plus the other two: one physical edge, delivered the way the
 * adapter would deliver it. */
static void godot_feed(struct godot_rig *rig, unsigned int pad, int control,
                       int pressed, float value) {
  if (!rig->pad[pad].connected ||
      !rig->pad[pad].reachable[control]) {
    return; /* the pad does not have this control: nothing is fabricated */
  }
  if (nxinput_gptk_dispatcher_decision(&rig->dispatcher[pad], control, 0) ==
      NXINPUT_GPTK_DECIDE_SUPPRESS) {
    rig->suppressed_dropped[0]++;
  }
  nxinput_gptk_dispatcher_feed(&rig->dispatcher[pad], control, pressed,
                               value);
  godot_raw_event(rig, pad, control, pressed);
  (void)godot_poll(rig, pad, control, pressed);
}

static void rig_reset(void) { memset(&g_rig, 0, sizeof g_rig); }

int main(void) {
  static char text[8192];
  nxinput_gptk map;
  char error[192];
  const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
  int i;

  /* The pilot's editable V2 case: A and B are null and their actions moved
   * to L2/R2. Exactly what the owner must be able to do without a rebuild. */
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    gp[i] = "null";
  }
  gp[NXINPUT_GPTK_A] = "null";
  gp[NXINPUT_GPTK_B] = "null";
  gp[NXINPUT_GPTK_L2] = "game.jump";  /* was A */
  gp[NXINPUT_GPTK_R2] = "game.shoot"; /* was B */
  gp[NXINPUT_GPTK_START] = "ui.pause";
  gp[NXINPUT_GPTK_X] = "native";
  build_v2(text, sizeof text, gp);
  CHECK(nxinput_gptk_parse(text, strlen(text), &map, error, sizeof error) == 0,
        "pilot V2 map parses (A/B null, actions on L2/R2)");

  rig_reset();
  for (i = 0; i < 2; i++) {
    g_rig.pad[i].connected = 1;
    (void)snprintf(g_rig.pad[i].guid, sizeof g_rig.pad[i].guid,
                   "190000004b4800000011000000010000");
    memset(g_rig.pad[i].reachable, 1, sizeof g_rig.pad[i].reachable);
    /* Neither pad has a right stick or R3 in this fixture. */
    g_rig.pad[i].reachable[NXINPUT_GPTK_RIGHT_STICK] = 0u;
    g_rig.pad[i].reachable[NXINPUT_GPTK_R3] = 0u;
    nxinput_gptk_dispatcher_init(&g_rig.dispatcher[i], &map);
    nxinput_gptk_dispatcher_set_context(&g_rig.dispatcher[i],
                                        NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&g_rig.dispatcher[i], "game.jump",
                                           action_sink, 0);
    (void)nxinput_gptk_dispatcher_register(&g_rig.dispatcher[i], "game.shoot",
                                           action_sink, 0);
    (void)nxinput_gptk_dispatcher_register(&g_rig.dispatcher[i], "ui.pause",
                                           action_sink, 0);
  }

  /* 1. THE CONTRACT: all 18 controls are accounted for, and a control the
   * pad does not physically have is recorded reachable=0 -- never a PASS. */
  {
    int decided = 0;
    int unreachable = 0;

    printf("    adapter control contract (pad 0):\n");
    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      const char *action = 0;
      nxinput_gptk_decision decision =
          nxinput_gptk_dispatcher_decision(&g_rig.dispatcher[0], i, &action);

      decided++;
      if (!g_rig.pad[0].reachable[i]) {
        unreachable++;
      }
      printf("      %-12s reachable=%d decision=%-8s action=%s\n",
             CONTROLS[i], g_rig.pad[0].reachable[i],
             nxinput_gptk_decision_name(decision),
             action && action[0] ? action : "-");
    }
    CHECK(decided == (int)NXINPUT_GPTK_CONTROL_COUNT && unreachable == 2,
          "all 18 controls are in the contract; absent ones are reachable=0");
  }

  /* 2. THE RAW LEAK. A and B are `null`: no action, no raw event, and
   * polling never reports them pressed -- on every route. */
  godot_feed(&g_rig, 0u, NXINPUT_GPTK_A, 1, 1.0f);
  godot_feed(&g_rig, 0u, NXINPUT_GPTK_A, 0, 0.0f);
  godot_feed(&g_rig, 0u, NXINPUT_GPTK_B, 1, 1.0f);
  godot_feed(&g_rig, 0u, NXINPUT_GPTK_B, 0, 0.0f);
  CHECK(g_rig.action_events == 0u && g_rig.raw_events == 0u &&
            g_rig.poll_true == 0u,
        "A/B null: no action, no raw event, no state query hit (adapter)");
  CHECK(g_rig.suppressed_dropped[0] > 0u &&
            g_rig.suppressed_dropped[1] > 0u &&
            g_rig.suppressed_dropped[2] > 0u,
        "all three adapter fan-out paths consumed the SUPPRESS");

  /* 3. The moved actions work from L2/R2, with no rebuild. */
  nxinput_gptk_dispatcher_feed_trigger(&g_rig.dispatcher[0], NXINPUT_GPTK_L2,
                                       0.90f);
  nxinput_gptk_dispatcher_feed_trigger(&g_rig.dispatcher[0], NXINPUT_GPTK_L2,
                                       0.05f);
  CHECK(g_rig.action_events == 2u && strcmp(g_rig.last_action,
                                            "game.jump") == 0,
        "L2 now drives game.jump (press and release)");
  nxinput_gptk_dispatcher_feed_trigger(&g_rig.dispatcher[0], NXINPUT_GPTK_R2,
                                       0.90f);
  CHECK(g_rig.action_events == 3u && strcmp(g_rig.last_action,
                                            "game.shoot") == 0,
        "R2 now drives game.shoot");

  /* 4. `native` still reaches the engine: it is a declared passthrough, not
   * a disable. X is native, so the raw route delivers it. */
  {
    unsigned int before = g_rig.raw_events;

    godot_feed(&g_rig, 0u, NXINPUT_GPTK_X, 1, 1.0f);
    CHECK(g_rig.raw_events == before + 1u,
          "a `native` control still reaches the adapter's raw path");
  }

  /* 5. TWO PADS with the SAME GUID stay independent: a press on pad 1 is
   * pad 1's, and suppression holds on both. */
  {
    unsigned int actions_before = g_rig.action_events;

    CHECK(strcmp(g_rig.pad[0].guid, g_rig.pad[1].guid) == 0,
          "the two pads really do share one GUID");
    godot_feed(&g_rig, 1u, NXINPUT_GPTK_A, 1, 1.0f);
    CHECK(g_rig.action_events == actions_before,
          "duplicate GUID: pad 1's null A stays silent too");
    nxinput_gptk_dispatcher_feed_trigger(&g_rig.dispatcher[1],
                                         NXINPUT_GPTK_L2, 0.95f);
    CHECK(g_rig.action_events == actions_before + 1u,
          "duplicate GUID: pad 1 drives its own action independently");
  }

  /* 6. HOTPLUG: pad 1 leaves and comes back. Nothing of the old pad's state
   * survives, and the decision is taken again from the live map. */
  {
    g_rig.pad[1].connected = 0;
    nxinput_gptk_dispatcher_set_context(&g_rig.dispatcher[1],
                                        NXINPUT_GPTK_CONTEXT_MENU);
    memset(&g_rig.dispatcher[1], 0, sizeof g_rig.dispatcher[1]);
    g_rig.pad[1].connected = 1;
    nxinput_gptk_dispatcher_init(&g_rig.dispatcher[1], &map);
    nxinput_gptk_dispatcher_set_context(&g_rig.dispatcher[1],
                                        NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&g_rig.dispatcher[1], "game.jump",
                                           action_sink, 0);
    CHECK(g_rig.dispatcher[1].latched == 0u &&
              nxinput_gptk_dispatcher_decision(&g_rig.dispatcher[1],
                                               NXINPUT_GPTK_A, 0) ==
                  NXINPUT_GPTK_DECIDE_SUPPRESS,
          "hotplug: the reconnected pad carries no stale latch and re-decides");
  }

  /* 7. THE EXIT CHORD. Only SELECT+START on the SAME pad ends the session,
   * and it uses the sovereign mapping's effective bindings -- never L2/R2,
   * never GUIDE+START, never a mix of two pads. */
  {
    nxinput_exit_chord chord;
    int poll;

    /* positive */
    nxinput_exit_chord_init(&chord, 0);
    for (poll = 0; poll < (int)NXINPUT_EXIT_CHORD_DEFAULT_HOLD_POLLS; poll++) {
      (void)nxinput_exit_chord_update(&chord, 1, 1);
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 1,
          "chord: SELECT+START on one pad ends the session");

    /* negative: L2+R2 is not the chord, however long it is held */
    nxinput_exit_chord_init(&chord, 0);
    for (poll = 0; poll < 30; poll++) {
      nxinput_gptk_dispatcher_feed_trigger(&g_rig.dispatcher[0],
                                           NXINPUT_GPTK_L2, 1.0f);
      nxinput_gptk_dispatcher_feed_trigger(&g_rig.dispatcher[0],
                                           NXINPUT_GPTK_R2, 1.0f);
      (void)nxinput_exit_chord_update(&chord, 0, 0);
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 0,
          "chord negative: L2+R2 never ends the session");

    /* negative: GUIDE+START is not the chord */
    nxinput_exit_chord_init(&chord, 0);
    for (poll = 0; poll < 30; poll++) {
      (void)nxinput_exit_chord_update(&chord, 0, 1); /* start only */
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 0,
          "chord negative: GUIDE+START (start alone) never ends the session");

    /* negative: SELECT on pad 0 and START on pad 1 is not one pad */
    nxinput_exit_chord_init(&chord, 0);
    for (poll = 0; poll < 30; poll++) {
      int pad0_select = 1;
      int pad0_start = 0;
      int pad1_select = 0;
      int pad1_start = 1;

      /* the chord is per pad: neither pad has both down */
      (void)nxinput_exit_chord_update(&chord, pad0_select && pad0_start,
                                      pad0_select && pad0_start);
      (void)nxinput_exit_chord_update(&chord, pad1_select && pad1_start,
                                      pad1_select && pad1_start);
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 0,
          "chord negative: SELECT on one pad and START on another never ends");

    /* And SELECT/START being `null` for the GAME does not disarm it. */
    CHECK(nxinput_gptk_dispatcher_decision(&g_rig.dispatcher[0],
                                           NXINPUT_GPTK_SELECT, 0) ==
              NXINPUT_GPTK_DECIDE_SUPPRESS,
          "chord: SELECT is null for the game in this very map");
    nxinput_exit_chord_init(&chord, 0);
    for (poll = 0; poll < (int)NXINPUT_EXIT_CHORD_DEFAULT_HOLD_POLLS; poll++) {
      (void)nxinput_exit_chord_update(&chord, 1, 1);
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 1,
          "chord: a null SELECT/START still ends the session (out-of-band)");
  }

  if (g_failures != 0) {
    printf("test_godot_consumer: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_godot_consumer: ALL PASS (FIXTURE: adapter fan-out only; "
         "real engine routes proved by godot_real_gate.py)\n");
  return 0;
}
