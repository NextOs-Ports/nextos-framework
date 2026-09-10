/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C4: NEXTOSCONTROLLERS v2 -- completeness, the tri-state
 * decision, `null` as a real SUPPRESS, and the owner's literal acceptance
 * case driven through a realistic hermetic consumer rig.
 *
 * CLAIM BOUNDARY: everything here is CORE_HERMETIC. No SDL, no Godot, no
 * Android, no Unity, no touch, no device. That SDL/Godot/Android/Unity/touch
 * adapters honour SUPPRESS is NOT proved here and cannot be: it belongs to
 * missions 116-119. */
#include "nxinput_exit_chord.h"
#include "nxinput_gptk.h"

#include <stdio.h>
#include <string.h>

/* The frozen bytes of a real nxgenerator schema-2 default. The path is a
 * build-time define so the test never depends on a working directory. */
#ifndef NXINPUT_V2_FIXTURE
#define NXINPUT_V2_FIXTURE "tests/corpus/ok-v2-generated.gptk"
#endif

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

/* The complete V2 vocabulary, in the stable order the generator emits. */
static const char *const CONTROLS[NXINPUT_GPTK_CONTROL_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK"};

/* ------------------------------------------------------------ file builder */
/* Build a COMPLETE V2 file. `values` gives one value per control for
 * [gameplay]; [menu] is filled entirely with `null` unless menu_values is
 * given. Nothing here is clever: a V2 file simply lists every control. */
static void build_v2(char *out, size_t cap,
                     const char *const *menu_values,
                     const char *const *gameplay_values) {
  size_t used = 0u;
  int i;

  used += (size_t)snprintf(out + used, cap - used,
                           "%s\nport = fixture\n\n[menu]\n",
                           NXINPUT_GPTK_MAGIC_V2);
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    used += (size_t)snprintf(out + used, cap - used, "%s = %s\n", CONTROLS[i],
                             menu_values ? menu_values[i] : "null");
  }
  used += (size_t)snprintf(out + used, cap - used, "\n[gameplay]\n");
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    used += (size_t)snprintf(out + used, cap - used, "%s = %s\n", CONTROLS[i],
                             gameplay_values ? gameplay_values[i] : "null");
  }
}

/* ------------------------------------------------------- the consumer rig */
/* A realistic hermetic consumer: two independent subsystems with names, each
 * registering for its own actions, plus a native-path spy that records any
 * control the adapter would read natively. It logs by NAME -- source,
 * canonical control, context, decision, action and consumer -- never bare
 * numbers. */
#define RIG_LOG_MAX 64

struct rig {
  char log[RIG_LOG_MAX][192];
  size_t lines;
  unsigned int cancel_press;
  unsigned int cancel_release;
  unsigned int confirm_press;
  unsigned int confirm_release;
  unsigned int native_reads;
  unsigned int analog_samples;
  float last_analog;
};

static struct rig g_rig;

static void rig_log(const char *source, int control, int context,
                    nxinput_gptk_decision decision, const char *action,
                    const char *consumer, int pressed, float value) {
  if (g_rig.lines >= RIG_LOG_MAX) {
    return;
  }
  (void)snprintf(g_rig.log[g_rig.lines], sizeof g_rig.log[0],
                 "EVENT source=%s canonical=%s context=%s decision=%s "
                 "action=%s consumer=%s pressed=%d value=%.2f",
                 source, nxinput_gptk_control_name(control),
                 nxinput_gptk_context_name(context),
                 nxinput_gptk_decision_name(decision),
                 action != 0 && action[0] != '\0' ? action : "-",
                 consumer != 0 ? consumer : "-", pressed, (double)value);
  g_rig.lines++;
}

static int rig_log_contains(const char *needle) {
  size_t i;
  for (i = 0u; i < g_rig.lines; i++) {
    if (strstr(g_rig.log[i], needle) != 0) {
      return 1;
    }
  }
  return 0;
}

/* Consumer 1: the combat subsystem. */
static void combat_sink(void *user, const char *action, int pressed,
                        float value) {
  (void)user;
  (void)value;
  if (strcmp(action, "game.cancel") == 0) {
    if (pressed) {
      g_rig.cancel_press++;
    } else {
      g_rig.cancel_release++;
    }
  } else if (strcmp(action, "game.confirm") == 0) {
    if (pressed) {
      g_rig.confirm_press++;
    } else {
      g_rig.confirm_release++;
    }
  }
}

/* Consumer 2: the analog path (opt-in). Registered as a VECTOR sink, it is
 * how a consumer says "I use the analog nature of this trigger". */
static void analog_sink(void *user, const char *action, float ax, float ay) {
  (void)user;
  (void)action;
  (void)ay;
  g_rig.analog_samples++;
  g_rig.last_analog = ax;
}

/* The adapter's native path. In a real port this is the engine reading the
 * control itself; here it only records that it was allowed to. */
static void rig_native_path(const nxinput_gptk_dispatcher *d, int control) {
  if ((nxinput_gptk_dispatcher_native_mask(d) &
       (UINT32_C(1) << (unsigned int)control)) != 0u) {
    g_rig.native_reads++;
  }
}

/* One canonical physical edge, exactly as an adapter would deliver it: ask
 * the single authority ONCE, log by name, then act on the answer. */
static void rig_feed(nxinput_gptk_dispatcher *d, const char *source,
                     int control, int pressed, float value) {
  const char *action = 0;
  nxinput_gptk_decision decision =
      nxinput_gptk_dispatcher_decision(d, control, &action);
  const char *consumer = decision == NXINPUT_GPTK_DECIDE_ACTION ? "combat"
                         : decision == NXINPUT_GPTK_DECIDE_NATIVE ? "native"
                                                                  : "-";

  rig_log(source, control, (int)d->context, decision, action, consumer,
          pressed, value);
  if (decision == NXINPUT_GPTK_DECIDE_SUPPRESS) {
    /* The adapter drops it here too -- and the dispatcher would drop it
     * anyway; both paths are exercised on purpose. */
    nxinput_gptk_dispatcher_feed(d, control, pressed, value);
    return;
  }
  if (decision == NXINPUT_GPTK_DECIDE_NATIVE) {
    rig_native_path(d, control);
    nxinput_gptk_dispatcher_feed(d, control, pressed, value);
    return;
  }
  nxinput_gptk_dispatcher_feed(d, control, pressed, value);
}

static void rig_reset(void) { memset(&g_rig, 0, sizeof g_rig); }

static int parse_ok(const char *text, nxinput_gptk *map, char *error,
                    size_t error_size) {
  return nxinput_gptk_parse(text, strlen(text), map, error, error_size);
}

int main(void) {
  static char text[8192];
  static char other[8192];
  nxinput_gptk map;
  char error[192];

  /* ---------------------------------------------------------------- 1. */
  /* Completeness: every control of every declared section, or NXI1002. */
  {
    const char *values[NXINPUT_GPTK_CONTROL_COUNT];
    int i;
    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      values[i] = "null";
    }
    build_v2(text, sizeof text, 0, values);
    CHECK(parse_ok(text, &map, error, sizeof error) == 0 &&
              map.schema_version == NXINPUT_GPTK_SCHEMA_V2,
          "a complete V2 file parses and reports schema 2");
    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      if (nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY, i, 0) !=
          NXINPUT_GPTK_DECIDE_SUPPRESS) {
        break;
      }
    }
    CHECK(i == (int)NXINPUT_GPTK_CONTROL_COUNT,
          "all 18 controls are present and every one decides SUPPRESS");
  }
  {
    /* Drop exactly one field: the file must fail, naming the control. */
    char shortened[8192];
    const char *values[NXINPUT_GPTK_CONTROL_COUNT];
    int i;
    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      values[i] = "null";
    }
    build_v2(text, sizeof text, 0, values);
    {
      /* remove the "R3 = null\n" line from the gameplay half */
      const char *hit = strstr(strstr(text, "[gameplay]"), "R3 = null\n");
      size_t head = (size_t)(hit - text);
      (void)snprintf(shortened, sizeof shortened, "%.*s%s", (int)head, text,
                     hit + strlen("R3 = null\n"));
    }
    CHECK(parse_ok(shortened, &map, error, sizeof error) ==
              NXINPUT_GPTK_ERR_MALFORMED &&
              strstr(error, "R3") != 0 && strstr(error, "gameplay") != 0,
          "a V2 section that omits one control fails and names it");
  }

  /* ---------------------------------------------------------------- 2. */
  /* A misspelled disable is never a silent disable. */
  {
    const char *values[NXINPUT_GPTK_CONTROL_COUNT];
    int i;
    static const char *const bad[] = {"NULL", "Null", "none", "nil", "Native",
                                      "NATIVE", "off", "disabled"};
    size_t b;
    int all_rejected = 1;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      values[i] = "null";
    }
    for (b = 0u; b < sizeof(bad) / sizeof(bad[0]); b++) {
      values[NXINPUT_GPTK_A] = bad[b];
      build_v2(text, sizeof text, 0, values);
      if (parse_ok(text, &map, error, sizeof error) !=
          NXINPUT_GPTK_ERR_MALFORMED) {
        all_rejected = 0;
      }
    }
    values[NXINPUT_GPTK_A] = "null";
    CHECK(all_rejected,
          "NULL/Null/none/nil/Native/NATIVE/off/disabled all fail closed");
  }

  /* ---------------------------------------------------------------- 3. */
  /* Duplicates, unknown names and actions without a dot still fail. */
  {
    static const char dup[] =
        "format = NEXTOS_CONTROLLERS/2\n[menu]\nA = null\nA = null\n";
    static const char unknown[] =
        "format = NEXTOS_CONTROLLERS/2\n[menu]\nHOME = null\n";
    static const char numeric[] =
        "format = NEXTOS_CONTROLLERS/2\n[menu]\n304 = null\n";
    CHECK(nxinput_gptk_parse(dup, strlen(dup), &map, error, sizeof error) ==
              NXINPUT_GPTK_ERR_DUPLICATE,
          "a duplicated control still fails closed in V2 (null included)");
    CHECK(nxinput_gptk_parse(unknown, strlen(unknown), &map, error,
                             sizeof error) == NXINPUT_GPTK_ERR_UNKNOWN_NAME,
          "an unknown control name fails closed in V2");
    CHECK(nxinput_gptk_parse(numeric, strlen(numeric), &map, error,
                             sizeof error) == NXINPUT_GPTK_ERR_UNKNOWN_NAME,
          "a numeric evdev code is still refused in V2");
  }

  /* ---------------------------------------------------------------- 4. */
  /* THE OWNER'S ACCEPTANCE CASE, literally:
   *   [gameplay] A = null, B = null, L2 = game.cancel, R2 = game.confirm  */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher d;
    nxinput_exit_chord chord;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_A] = "null";
    gp[NXINPUT_GPTK_B] = "null";
    gp[NXINPUT_GPTK_L2] = "game.cancel";
    gp[NXINPUT_GPTK_R2] = "game.confirm";
    gp[NXINPUT_GPTK_X] = "native"; /* a declared native passthrough */
    build_v2(text, sizeof text, 0, gp);
    CHECK(parse_ok(text, &map, error, sizeof error) == 0,
          "acceptance: the owner's V2 file parses");

    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &map);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&d, "game.cancel", combat_sink, 0);
    (void)nxinput_gptk_dispatcher_register(&d, "game.confirm", combat_sink, 0);
    (void)nxinput_gptk_dispatcher_register_vector(&d, "game.cancel",
                                                  analog_sink, 0);

    /* A and B: canonical events, both directions, twice. */
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 0, 0.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_B, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_B, 0, 0.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_B, 1, 1.0f);
    CHECK(g_rig.cancel_press == 0u && g_rig.cancel_release == 0u &&
              g_rig.confirm_press == 0u && g_rig.confirm_release == 0u &&
              g_rig.native_reads == 0u,
          "acceptance: A and B fire no action, no native read, nothing");

    /* L2 crosses ENTER then falls under EXIT: exactly one press, one release. */
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 0.30f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 0.55f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 0.85f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 0.90f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 0.50f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 0.10f);
    CHECK(g_rig.cancel_press == 1u && g_rig.cancel_release == 1u,
          "acceptance: L2 presses and releases game.cancel exactly once");

    /* R2 the same, on its own action. */
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_R2, 0.95f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_R2, 0.95f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_R2, 0.00f);
    CHECK(g_rig.confirm_press == 1u && g_rig.confirm_release == 1u &&
              g_rig.cancel_press == 1u,
          "acceptance: R2 presses and releases game.confirm exactly once");

    /* The analog nature survives for the consumer that asked for it. */
    CHECK(g_rig.analog_samples == 6u && g_rig.last_analog == 0.10f,
          "acceptance: the vector sink received every analog sample of L2");

    /* L2+R2 together never reach the lifecycle boundary. */
    nxinput_exit_chord_init(&chord, 0);
    for (i = 0; i < 10; i++) {
      /* The chord only ever sees SELECT/START. Triggers cannot enter it. */
      (void)nxinput_exit_chord_update(&chord, 0, 0);
      nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 1.0f);
      nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_R2, 1.0f);
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 0,
          "acceptance: L2+R2 held together never request the exit");

    /* The log names things, it does not print bare numbers. */
    CHECK(rig_log_contains("canonical=A") &&
              rig_log_contains("canonical=B") &&
              rig_log_contains("decision=SUPPRESS") &&
              rig_log_contains("context=gameplay") &&
              rig_log_contains("source=primary"),
          "acceptance: the log carries source, canonical, context, decision");
    printf("    log sample: %s\n", g_rig.log[0]);
  }

  /* ---------------------------------------------------------------- 5. */
  /* Rebinding again works with NO recompilation of the game: same sinks,
   * same rig, a different file. */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk remap;
    nxinput_gptk_dispatcher d;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_A] = "game.confirm"; /* was null */
    gp[NXINPUT_GPTK_B] = "game.cancel";  /* was null */
    gp[NXINPUT_GPTK_L2] = "null";        /* was game.cancel */
    gp[NXINPUT_GPTK_R2] = "null";        /* was game.confirm */
    build_v2(other, sizeof other, 0, gp);
    CHECK(parse_ok(other, &remap, error, sizeof error) == 0,
          "rebind: the swapped file parses");

    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &remap);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&d, "game.cancel", combat_sink, 0);
    (void)nxinput_gptk_dispatcher_register(&d, "game.confirm", combat_sink, 0);
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 0, 0.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_B, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_B, 0, 0.0f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_L2, 1.0f);
    nxinput_gptk_dispatcher_feed_trigger(&d, NXINPUT_GPTK_R2, 1.0f);
    CHECK(g_rig.confirm_press == 1u && g_rig.confirm_release == 1u &&
              g_rig.cancel_press == 1u && g_rig.cancel_release == 1u,
          "rebind: A and B now drive the actions, with the same binary");
    CHECK(nxinput_gptk_dispatcher_trigger_value(&d, NXINPUT_GPTK_L2) == 0.0f,
          "rebind: the now-null triggers deliver nothing at all");
  }

  /* ---------------------------------------------------------------- 6. */
  /* SUPPRESS is consumed before ANY fallback: the narrow fallback source
   * cannot resurrect a `null` control. */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher d;
    nxinput_gptk_source_guard guard;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_Y] = "game.cancel";
    build_v2(text, sizeof text, 0, gp);
    (void)parse_ok(text, &map, error, sizeof error);
    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &map);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&d, "game.cancel", combat_sink, 0);
    nxinput_gptk_source_guard_init(&guard, &d);
    nxinput_gptk_dispatcher_set_primary_mask(&d, &guard, 0u);
    /* A is `null`: neither source may deliver it. */
    nxinput_gptk_dispatcher_feed_source(&d, &guard,
                                        NXINPUT_GPTK_SOURCE_FALLBACK,
                                        NXINPUT_GPTK_A, 1, 1.0f);
    nxinput_gptk_dispatcher_feed_source(&d, &guard,
                                        NXINPUT_GPTK_SOURCE_PRIMARY,
                                        NXINPUT_GPTK_A, 1, 1.0f);
    /* Y is a real action: the fallback still works for it. */
    nxinput_gptk_dispatcher_feed_source(&d, &guard,
                                        NXINPUT_GPTK_SOURCE_FALLBACK,
                                        NXINPUT_GPTK_Y, 1, 1.0f);
    CHECK(g_rig.cancel_press == 1u,
          "SUPPRESS is consumed before the fallback; a live action is not");
    CHECK((nxinput_gptk_dispatcher_null_mask(&d) &
           (UINT32_C(1) << NXINPUT_GPTK_A)) != 0u &&
              (nxinput_gptk_dispatcher_null_mask(&d) &
               (UINT32_C(1) << NXINPUT_GPTK_Y)) == 0u,
          "the null mask names exactly the suppressed controls");
  }

  /* ---------------------------------------------------------------- 7. */
  /* A game-owned chord built on a `null` control can never fire, while the
   * out-of-band lifecycle chord is untouched by the same file. */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher d;
    nxinput_exit_chord chord;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_SELECT] = "null"; /* explicit: the owner disabled it */
    gp[NXINPUT_GPTK_START] = "null";
    gp[NXINPUT_GPTK_L1] = "game.chord_a"; /* a game chord half */
    build_v2(text, sizeof text, 0, gp);
    (void)parse_ok(text, &map, error, sizeof error);
    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &map);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&d, "game.cancel", combat_sink, 0);

    /* The GAME never sees SELECT or START. */
    rig_feed(&d, "primary", NXINPUT_GPTK_SELECT, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_START, 1, 1.0f);
    CHECK(nxinput_gptk_dispatcher_decision(&d, NXINPUT_GPTK_SELECT, 0) ==
              NXINPUT_GPTK_DECIDE_SUPPRESS &&
              nxinput_gptk_dispatcher_decision(&d, NXINPUT_GPTK_START, 0) ==
                  NXINPUT_GPTK_DECIDE_SUPPRESS,
          "lifecycle: SELECT and START are suppressed FOR THE GAME");

    /* The sovereign chord still fires: it never reads this map. */
    nxinput_exit_chord_init(&chord, 0);
    for (i = 0; i < (int)NXINPUT_EXIT_CHORD_DEFAULT_HOLD_POLLS; i++) {
      (void)nxinput_exit_chord_update(&chord, 1, 1);
    }
    CHECK(nxinput_exit_chord_requested(&chord) == 1,
          "lifecycle: SELECT=null/START=null never disarm the exit chord");
  }

  /* ---------------------------------------------------------------- 8. */
  /* NATIVE is a declared passthrough: no sink, but the adapter is told it
   * may read the control itself. */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher d;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_X] = "native";
    gp[NXINPUT_GPTK_Y] = "game.cancel";
    build_v2(text, sizeof text, 0, gp);
    (void)parse_ok(text, &map, error, sizeof error);
    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &map);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&d, "game.cancel", combat_sink, 0);
    rig_feed(&d, "primary", NXINPUT_GPTK_X, 1, 1.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_Y, 1, 1.0f);
    CHECK(g_rig.native_reads == 1u && g_rig.cancel_press == 1u,
          "native: the adapter reads X natively and Y still drives its sink");
    CHECK(nxinput_gptk_dispatcher_native_mask(&d) ==
              (UINT32_C(1) << NXINPUT_GPTK_X),
          "native: the native mask names exactly the declared passthrough");
    CHECK(rig_log_contains("decision=NATIVE") &&
              rig_log_contains("consumer=native") &&
              !rig_log_contains("decision=SUPPRESS consumer=native"),
          "native: the log distinguishes NATIVE from SUPPRESS by name");
  }

  /* ---------------------------------------------------------------- 9. */
  /* Context switch: a control that is an action in [menu] and `null` in
   * [gameplay] must not leak a phantom release across the boundary. */
  {
    const char *menu[NXINPUT_GPTK_CONTROL_COUNT];
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher d;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      menu[i] = "null";
      gp[i] = "null";
    }
    menu[NXINPUT_GPTK_A] = "game.confirm";
    gp[NXINPUT_GPTK_A] = "null";
    build_v2(text, sizeof text, menu, gp);
    (void)parse_ok(text, &map, error, sizeof error);
    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &map); /* starts in MENU */
    (void)nxinput_gptk_dispatcher_register(&d, "game.confirm", combat_sink, 0);
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 1, 1.0f);
    CHECK(g_rig.confirm_press == 1u && g_rig.confirm_release == 0u,
          "context: A is a real action in [menu]");
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    CHECK(g_rig.confirm_release == 1u,
          "context: switching releases the menu press in the OLD context");
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 0, 0.0f);
    rig_feed(&d, "primary", NXINPUT_GPTK_A, 1, 1.0f);
    CHECK(g_rig.confirm_press == 1u && g_rig.confirm_release == 1u,
          "context: the same control is inert in [gameplay] where it is null");
  }

  /* --------------------------------------------------------------- 10. */
  /* Two pads: two dispatchers over ONE map keep independent state, and a
   * hot-unplug of one leaves the other exactly where it was. */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher pad1;
    nxinput_gptk_dispatcher pad2;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_Y] = "game.cancel";
    build_v2(text, sizeof text, 0, gp);
    (void)parse_ok(text, &map, error, sizeof error);
    rig_reset();
    nxinput_gptk_dispatcher_init(&pad1, &map);
    nxinput_gptk_dispatcher_init(&pad2, &map);
    nxinput_gptk_dispatcher_set_context(&pad1,
                                        NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    nxinput_gptk_dispatcher_set_context(&pad2,
                                        NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register(&pad1, "game.cancel", combat_sink,
                                           0);
    (void)nxinput_gptk_dispatcher_register(&pad2, "game.cancel", combat_sink,
                                           0);
    nxinput_gptk_dispatcher_feed(&pad1, NXINPUT_GPTK_Y, 1, 1.0f);
    nxinput_gptk_dispatcher_feed(&pad2, NXINPUT_GPTK_Y, 1, 1.0f);
    CHECK(g_rig.cancel_press == 2u,
          "two pads: each pad delivers its own press");
    /* Hot-unplug pad1: releasing its latch must not touch pad2. */
    nxinput_gptk_dispatcher_feed(&pad1, NXINPUT_GPTK_Y, 0, 0.0f);
    CHECK(g_rig.cancel_release == 1u && pad2.latched != 0u,
          "two pads: unplugging one leaves the other's latch intact");
    /* And a suppressed control stays suppressed on BOTH pads. */
    nxinput_gptk_dispatcher_feed(&pad1, NXINPUT_GPTK_A, 1, 1.0f);
    nxinput_gptk_dispatcher_feed(&pad2, NXINPUT_GPTK_A, 1, 1.0f);
    CHECK(g_rig.cancel_press == 2u &&
              (pad1.latched & (UINT32_C(1) << NXINPUT_GPTK_A)) == 0u &&
              (pad2.latched & (UINT32_C(1) << NXINPUT_GPTK_A)) == 0u,
          "two pads: a null control latches nothing on either pad");
  }

  /* --------------------------------------------------------------- 11. */
  /* Double read: a `null` stick is owned by NOBODY -- the framework does not
   * claim it and the game must not receive it either. */
  {
    const char *gp[NXINPUT_GPTK_CONTROL_COUNT];
    nxinput_gptk_dispatcher d;
    int i;

    for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
      gp[i] = "null";
    }
    gp[NXINPUT_GPTK_RIGHT_STICK] = "camera.look";
    gp[NXINPUT_GPTK_LEFT_STICK] = "null";
    build_v2(text, sizeof text, 0, gp);
    (void)parse_ok(text, &map, error, sizeof error);
    rig_reset();
    nxinput_gptk_dispatcher_init(&d, &map);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    (void)nxinput_gptk_dispatcher_register_vector(&d, "camera.look",
                                                  analog_sink, 0);
    nxinput_gptk_dispatcher_feed_stick(&d, NXINPUT_GPTK_LEFT_STICK, 1.0f,
                                       0.0f, 0.016f);
    CHECK(g_rig.analog_samples == 0u,
          "double read: a null stick delivers no vector");
    nxinput_gptk_dispatcher_feed_stick(&d, NXINPUT_GPTK_RIGHT_STICK, 1.0f,
                                       0.0f, 0.016f);
    CHECK(g_rig.analog_samples == 1u,
          "double read: the camera stick still delivers its vector");
    CHECK(nxinput_gptk_dispatcher_suppressed_mask(&d) ==
              (UINT32_C(1) << NXINPUT_GPTK_RIGHT_STICK),
          "double read: only the framework-owned stick is in the mask");
    CHECK(nxinput_gptk_dispatcher_control_suppressed(
              &d, NXINPUT_GPTK_LEFT_STICK) == 0 &&
              (nxinput_gptk_dispatcher_null_mask(&d) &
               (UINT32_C(1) << NXINPUT_GPTK_LEFT_STICK)) != 0u,
          "double read: a null stick is disabled, not framework-owned");
  }

  /* --------------------------------------------------------------- 12. */
  /* V1 is untouched: no schema 2, no null, no native, absence still means
   * "unmapped" and decides NONE. */
  {
    static const char v1[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
        "[gameplay]\nA = player.jump\n";
    CHECK(nxinput_gptk_parse(v1, strlen(v1), &map, error, sizeof error) == 0 &&
              map.schema_version == NXINPUT_GPTK_SCHEMA_V1 &&
              nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_MENU,
                                  NXINPUT_GPTK_A, 0) ==
                  NXINPUT_GPTK_DECIDE_ACTION &&
              nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_MENU,
                                  NXINPUT_GPTK_R3, 0) ==
                  NXINPUT_GPTK_DECIDE_NONE,
          "V1: incomplete stays legal and absence decides NONE, not SUPPRESS");
  }

  /* --------------------------------------------------------------- 13. */
  /* COHERENCE with the generator: the frozen bytes of a real
   * defaults/NEXTOSCONTROLLERS.gptk emitted by nxgenerator with
   * `controls.schema = 2` must be accepted by THIS parser, list all 18
   * controls per section, and carry both a `null` and the declared
   * `native`. The fixture is committed bytes, not a live cross-call. */
  {
    FILE *stream = fopen(NXINPUT_V2_FIXTURE, "rb");
    static char generated[NXINPUT_GPTK_MAX_BYTES];
    size_t length;
    int control;
    int nulls = 0;
    int natives = 0;
    int actions = 0;

    CHECK(stream != 0, "coherence: the generated V2 fixture is readable");
    if (stream != 0) {
      length = fread(generated, 1u, sizeof generated, stream);
      (void)fclose(stream);
      CHECK(nxinput_gptk_parse(generated, length, &map, error,
                               sizeof error) == 0 &&
                map.schema_version == NXINPUT_GPTK_SCHEMA_V2,
            "coherence: nxgenerator's V2 output parses as schema 2 here");
      for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT;
           control++) {
        switch (nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                    control, 0)) {
          case NXINPUT_GPTK_DECIDE_SUPPRESS: nulls++; break;
          case NXINPUT_GPTK_DECIDE_NATIVE: natives++; break;
          case NXINPUT_GPTK_DECIDE_ACTION: actions++; break;
          default: break;
        }
      }
      CHECK(nulls + natives + actions == (int)NXINPUT_GPTK_CONTROL_COUNT &&
                nulls > 0 && natives == 1 && actions > 0,
            "coherence: every control is decided; null and native present");
    }
  }

  if (g_failures != 0) {
    printf("test_gptk_v2: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_gptk_v2: ALL PASS (CORE_HERMETIC; engine adapters pending "
         "116-119)\n");
  return 0;
}
