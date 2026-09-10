/* SPDX-License-Identifier: GPL-3.0-only */
/* TEARSCAPE-CONTROLS-LIVE host gate: proves that NEXTOSCONTROLLERS.gptk
 * REALLY governs which sink fires, through the exact runtime chain the
 * engine uses (nxinput_gptk_load_at -> live boundary -> ACK sink). */
#include "nxinput_gptk.h"
#include "nxinput_gptk_live.h"
#include "nxinput_gptk_loader.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const ALLOWED[] = {
  "menu.accept", "menu.back", "menu.navigate",
  "player.attack", "player.heal", "player.move", "player.open_map", "player.roll",
  "player.select_next", "player.select_previous", "player.switch_tool",
  "player.use_shield", "player.use_tool", "player.zoom_map",
  "system.pause", "system.quit",
};
#define ALLOWED_COUNT (sizeof(ALLOWED) / sizeof(ALLOWED[0]))

#define DEFAULT_MAP \
  "format = NEXTOS_CONTROLLERS/1\n" \
  "port = tearscape\n\n" \
  "[menu]\nA = menu.accept\nB = menu.back\nSTART = system.pause\n" \
  "LEFT_STICK = menu.navigate\n\n" \
  "[gameplay]\nA = player.roll\nB = player.switch_tool\nX = player.attack\n" \
  "Y = player.heal\nL1 = player.select_previous\n" \
  "R1 = player.select_next\nL2 = player.use_shield\nR2 = player.use_tool\n" \
  "SELECT = player.open_map\nSTART = system.pause\n" \
  "LEFT_STICK = player.move\n"

static char g_dir[256];
static int g_fail;

static void expect(int ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what);
    g_fail = 1;
  }
}

static void write_file(const char *name, const char *text) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", g_dir, name);
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  fputs(text, f);
  fclose(f);
}

static void copy_file(const char *name, const char *source) {
  char path[512];
  unsigned char buffer[4096];
  size_t count;
  FILE *input = fopen(source, "rb");
  snprintf(path, sizeof(path), "%s/%s", g_dir, name);
  FILE *output = fopen(path, "wb");
  if (!input || !output) { perror(!input ? source : path); exit(1); }
  while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0u) {
    if (fwrite(buffer, 1, count, output) != count) { perror(path); exit(1); }
  }
  if (ferror(input) || fclose(input) != 0 || fclose(output) != 0) {
    perror(source);
    exit(1);
  }
}

static void remove_file(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", g_dir, name);
  unlink(path);
}

static int load(nxinput_gptk *map, nxinput_gptk_load_receipt *receipt) {
  char defaults[512];
  snprintf(defaults, sizeof(defaults), "%s/defaults", g_dir);
  int owner_fd = open(g_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  int defaults_fd = open(defaults, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (owner_fd < 0 || defaults_fd < 0) { perror("dirs"); exit(1); }
  int rc = nxinput_gptk_load_at(owner_fd, defaults_fd, ALLOWED, ALLOWED_COUNT,
                                map, receipt);
  close(owner_fd);
  close(defaults_fd);
  return rc;
}

/* Recording sinks. */
static char g_events[64][80];
static size_t g_event_count;
static int record_sink(void *user, const char *action, int pressed,
                       float value) {
  (void)value;
  if (g_event_count < 64) {
    snprintf(g_events[g_event_count], sizeof(g_events[0]), "%s:%s:%d",
             (const char *)user, action, pressed);
    g_event_count++;
  }
  return 0;
}
static int record_vector(void *user, const char *action, float x, float y) {
  if (g_event_count < 64) {
    snprintf(g_events[g_event_count], sizeof(g_events[0]), "%s:%s:%.1f:%.1f",
             (const char *)user, action, x, y);
    g_event_count++;
  }
  return 0;
}
static int fail_sink(void *user, const char *action, int pressed, float value) {
  (void)user;
  (void)action;
  (void)pressed;
  (void)value;
  return -1;
}
static int fail_release_sink(void *user, const char *action, int pressed,
                             float value) {
  if (!pressed) {
    return -1;
  }
  return record_sink(user, action, pressed, value);
}
static void clear_events(void) { g_event_count = 0; memset(g_events, 0, sizeof(g_events)); }
static int saw_event(const char *needle) {
  for (size_t i = 0; i < g_event_count; i++) {
    if (strcmp(g_events[i], needle) == 0) { return 1; }
  }
  return 0;
}

static int is_vector_action(const char *action) {
  return strcmp(action, "menu.navigate") == 0 ||
         strcmp(action, "player.move") == 0;
}

static void prepare_live(nxinput_gptk_live *live, const nxinput_gptk *map) {
  char error[160] = { 0 };
  nxinput_gptk_live_init(live, map);
  for (size_t i = 0; i < ALLOWED_COUNT; i++) {
    int rc = is_vector_action(ALLOWED[i])
                 ? nxinput_gptk_live_register_vector(
                       live, ALLOWED[i], record_vector, (void *)"vector")
                 : nxinput_gptk_live_register(
                       live, ALLOWED[i], record_sink, (void *)"sink");
    expect(rc == 0, "live sink registration succeeds");
  }
  expect(nxinput_gptk_live_register_vector(
             live, "player.zoom_map", record_vector, (void *)"vector") == 0,
         "legacy right-stick zoom compatibility sink registers");
  expect(nxinput_gptk_live_seal(live, error, sizeof(error)) == 0,
         "live boundary seals with complete sink coverage");
}

struct generated_case {
  nxinput_gptk_context context;
  int control;
  const char *control_name;
  const char *action;
  const char *sink;
  int vector;
};

static const struct generated_case GENERATED_CASES[] = {
  { NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_A, "A", "menu.accept",
    "engine.ui_accept", 0 },
  { NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_B, "B", "menu.back",
    "engine.ui_cancel", 0 },
  { NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_START, "START", "system.pause",
    "engine.input.pause", 0 },
  { NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_LEFT_STICK, "LEFT_STICK",
    "menu.navigate", "engine.ui_direction", 1 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_A, "A", "player.roll",
    "engine.input.roll", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_B, "B", "player.switch_tool",
    "engine.input.switch_tool", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_X, "X", "player.attack",
    "engine.input.attack", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_Y, "Y", "player.heal",
    "engine.input.heal", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_L1, "L1",
    "player.select_previous", "engine.input.select_prev", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_R1, "R1",
    "player.select_next", "engine.input.select_next", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_L2, "L2",
    "player.use_shield", "engine.input.use_shield", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_R2, "R2", "player.use_tool",
    "engine.input.use_tool", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_SELECT, "SELECT",
    "player.open_map", "engine.input.open_map", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_START, "START",
    "system.pause", "engine.input.pause", 0 },
  { NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_LEFT_STICK, "LEFT_STICK",
    "player.move", "engine.input.move", 1 },
};

static const struct generated_case *find_generated_case(
    nxinput_gptk_context context, int control) {
  for (size_t i = 0; i < sizeof(GENERATED_CASES) / sizeof(GENERATED_CASES[0]); i++) {
    if (GENERATED_CASES[i].context == context &&
        GENERATED_CASES[i].control == control) {
      return &GENERATED_CASES[i];
    }
  }
  return NULL;
}

static void prove_generated_default(const char *source) {
  nxinput_gptk map;
  nxinput_gptk_load_receipt receipt;
  nxinput_gptk_live live;
  char expected[80];
  nxinput_gptk_context current = NXINPUT_GPTK_CONTEXT_COUNT;

  remove_file("NEXTOSCONTROLLERS.gptk");
  copy_file("defaults/NEXTOSCONTROLLERS.gptk", source);
  expect(load(&map, &receipt) == 0, "generated default map loads");
  expect(receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING,
         "generated map is the exact default authority");
  prepare_live(&live, &map);

  for (int context = 0; context < (int)NXINPUT_GPTK_CONTEXT_COUNT; context++) {
    for (int control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
      const struct generated_case *expected = find_generated_case(
          (nxinput_gptk_context)context, control);
      const char *action = NULL;
      nxinput_gptk_decision decision = nxinput_gptk_decide(
          &map, (nxinput_gptk_context)context, control, &action);
      if (expected) {
        expect(decision == NXINPUT_GPTK_DECIDE_ACTION && action &&
                   strcmp(action, expected->action) == 0,
               "generated default binding differs from the exact closure");
      } else {
        /* Schema 3 generates the FULL tri-state: every control the closure
         * does not bind is an explicit `null` (SUPPRESS, governed nothing)
         * or a declared `native` passthrough (the D-pad: the engine's own
         * discrete handling, one press = one step). Neither delivers to a
         * sink; only ACTION here would be an undeclared binding. */
        expect(decision == NXINPUT_GPTK_DECIDE_NONE ||
                   decision == NXINPUT_GPTK_DECIDE_SUPPRESS ||
                   decision == NXINPUT_GPTK_DECIDE_NATIVE,
               "generated default contains an undeclared extra binding");
      }
    }
  }

  clear_events();
  expect(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
             NXINPUT_GPTK_LIVE_PASSTHROUGH && g_event_count == 0,
         "generated map remains native before context proof");
  for (size_t i = 0; i < sizeof(GENERATED_CASES) / sizeof(GENERATED_CASES[0]); i++) {
    const struct generated_case *item = &GENERATED_CASES[i];
    if (item->context != current) {
      int context_ok;
      const char *context_name;
      const char *context_source;
      current = item->context;
      context_name = current == NXINPUT_GPTK_CONTEXT_MENU
                         ? "menu" : "gameplay";
      context_source = current == NXINPUT_GPTK_CONTEXT_MENU
                           ? "scene:ui" : "scene:level";
      context_ok = nxinput_gptk_live_set_context(
                       &live, current, context_source) == 0;
      expect(context_ok,
             "generated map context becomes proven");
      if (context_ok) {
        printf("NXGPTK_PROOF\tCONTEXT\t%s\t%s\n",
               context_name, context_source);
      }
    }
    clear_events();
    if (item->vector) {
      int delivery_ok = nxinput_gptk_live_feed_vector(
                            &live, item->control, 0.75f, -0.25f) ==
                        NXINPUT_GPTK_LIVE_DELIVERED;
      int callback_ok;
      expect(delivery_ok,
             "generated vector binding is delivered once");
      snprintf(expected, sizeof(expected), "vector:%s:", item->action);
      callback_ok = g_event_count == 1 &&
                    strncmp(g_events[0], expected, strlen(expected)) == 0;
      expect(callback_ok,
             "generated vector reaches the declared action sink");
      if (delivery_ok && callback_ok) {
        printf("NXGPTK_PROOF\tCASE\t%s\t%s\t%s\tmotion\t%s\t%s\t1\n",
               current == NXINPUT_GPTK_CONTEXT_MENU ? "menu" : "gameplay",
               current == NXINPUT_GPTK_CONTEXT_MENU ? "scene:ui" : "scene:level",
               item->control_name, item->action, item->sink);
      }
      (void)nxinput_gptk_live_feed_vector(&live, item->control, 0.0f, 0.0f);
    } else {
      int delivery_ok = nxinput_gptk_live_feed(
                            &live, item->control, 1, 1.0f) ==
                        NXINPUT_GPTK_LIVE_DELIVERED;
      int callback_ok;
      expect(delivery_ok,
             "generated button binding is delivered once");
      snprintf(expected, sizeof(expected), "sink:%s:1", item->action);
      callback_ok = g_event_count == 1 && saw_event(expected);
      expect(callback_ok,
             "generated button reaches the declared action sink");
      if (delivery_ok && callback_ok) {
        printf("NXGPTK_PROOF\tCASE\t%s\t%s\t%s\tpress\t%s\t%s\t1\n",
               current == NXINPUT_GPTK_CONTEXT_MENU ? "menu" : "gameplay",
               current == NXINPUT_GPTK_CONTEXT_MENU ? "scene:ui" : "scene:level",
               item->control_name, item->action, item->sink);
      }
      (void)nxinput_gptk_live_feed(&live, item->control, 0, 0.0f);
    }
  }
  printf("generated GPTK closure: PASS cases=%zu source=exact-scaffold-default\n",
         sizeof(GENERATED_CASES) / sizeof(GENERATED_CASES[0]));
}

int main(int argc, char **argv) {
	if (argc > 2) {
		fprintf(stderr, "usage: %s [generated-NEXTOSCONTROLLERS.gptk]\n", argv[0]);
		return 2;
	}
  snprintf(g_dir, sizeof(g_dir), "/tmp/nxgptk-host-XXXXXX");
  if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
  char defaults[512];
  snprintf(defaults, sizeof(defaults), "%s/defaults", g_dir);
  mkdir(defaults, 0755);
  write_file("defaults/NEXTOSCONTROLLERS.gptk", DEFAULT_MAP);

  nxinput_gptk map;
  nxinput_gptk_load_receipt receipt;
  nxinput_gptk_live live;

  /* 1. Owner absent -> default selected. */
  expect(load(&map, &receipt) == 0, "default map loads");
  expect(receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING,
         "owner missing selects the default");

  /* 2. UNPROVEN is native-safe; then default semantics are A=accept/roll
   * and B=back/attack in their proved contexts. */
  prepare_live(&live, &map);
  clear_events();
  {
    int unknown_ok = nxinput_gptk_live_feed(
                         &live, NXINPUT_GPTK_A, 1, 1.0f) ==
                     NXINPUT_GPTK_LIVE_PASSTHROUGH && g_event_count == 0;
    expect(unknown_ok, "unproven context passes through without a sink");
    if (unknown_ok) {
      printf("NXGPTK_PROOF\tSAFETY\tunknown_context\tPASSTHROUGH\t0\n");
    }
  }
  expect(nxinput_gptk_live_set_context(
             &live, NXINPUT_GPTK_CONTEXT_MENU, "scene:ui") == 0,
         "menu context is proven from scene state");
  clear_events();
  expect(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
             NXINPUT_GPTK_LIVE_DELIVERED,
         "menu A is consumed only after the real sink ACKs");
  nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 0, 0.0f);
  expect(saw_event("sink:menu.accept:1") && saw_event("sink:menu.accept:0"),
         "menu A fires menu.accept");
  expect(!saw_event("sink:menu.back:1"), "menu A does not fire menu.back");
  expect(nxinput_gptk_live_set_context(
             &live, NXINPUT_GPTK_CONTEXT_GAMEPLAY, "scene:map") == 0,
         "gameplay context is proven from scene state");
  clear_events();
  nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_live_feed(&live, NXINPUT_GPTK_B, 1, 1.0f);
  expect(saw_event("sink:player.roll:1"), "gameplay A fires player.roll");
  expect(saw_event("sink:player.switch_tool:1"),
         "gameplay B fires player.switch_tool");

  /* 3. Context switch releases latched actions in the old context. */
  clear_events();
  nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_MENU, "scene:ui");
  expect(saw_event("sink:player.roll:0") && saw_event("sink:player.switch_tool:0"),
         "context switch releases latched gameplay actions");

  /* 4. OWNER EDIT REALLY GOVERNS: swap A/B in both contexts. */
  write_file("NEXTOSCONTROLLERS.gptk",
             "format = NEXTOS_CONTROLLERS/1\n"
             "[menu]\nA = menu.back\nB = menu.accept\n"
             "[gameplay]\nA = player.attack\nB = player.roll\n");
  expect(load(&map, &receipt) == 0, "swapped owner map loads");
  expect(receipt.source == NXINPUT_GPTK_LOAD_OWNER, "owner map selected");
  prepare_live(&live, &map);
  nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_MENU, "scene:ui");
  clear_events();
  nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f);
  expect(saw_event("sink:menu.back:1") && !saw_event("sink:menu.accept:1"),
         "EDITED FILE: menu A now fires menu.back (swap live)");
  nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_GAMEPLAY, "scene:map");
  clear_events();
  nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_live_feed(&live, NXINPUT_GPTK_B, 1, 1.0f);
  expect(saw_event("sink:player.attack:1"), "EDITED: gameplay A attacks");
  expect(saw_event("sink:player.roll:1"), "EDITED: gameplay B rolls");

  /* 5. Sticks stay vector/analog decisions (glue feeds them analog). */
  const char *action = NULL;
  write_file("NEXTOSCONTROLLERS.gptk",
             "format = NEXTOS_CONTROLLERS/1\n"
             "[menu]\nLEFT_STICK = menu.navigate\n"
             "[gameplay]\nLEFT_STICK = player.move\n");
  expect(load(&map, &receipt) == 0, "stick map loads");
  expect(nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                             NXINPUT_GPTK_LEFT_STICK, &action) ==
                 NXINPUT_GPTK_DECIDE_ACTION &&
             action && strcmp(action, "player.move") == 0,
         "stick decision is the vector action");
  expect(nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                             NXINPUT_GPTK_RIGHT_STICK, &action) ==
             NXINPUT_GPTK_DECIDE_NONE,
         "unmapped stick stays native (analog untouched)");
  prepare_live(&live, &map);
  nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                "scene:level");
  clear_events();
  expect(nxinput_gptk_live_feed_vector(
             &live, NXINPUT_GPTK_LEFT_STICK, 0.8f, -0.4f) ==
             NXINPUT_GPTK_LIVE_DELIVERED,
         "mapped stick reaches and is ACKed by its vector sink");
  expect(saw_event("vector:player.move:0.8:-0.4"),
         "player.move retains both analog components");
  expect(nxinput_gptk_live_feed_vector(
             &live, NXINPUT_GPTK_RIGHT_STICK, 0.5f, 0.5f) ==
             NXINPUT_GPTK_LIVE_PASSTHROUGH,
         "unmapped stick passes through natively");

  /* 6. A map cannot become authoritative with an absent real sink. */
  {
    nxinput_gptk_live incomplete;
    char error[160] = { 0 };
    nxinput_gptk_live_init(&incomplete, &map);
    expect(nxinput_gptk_live_seal(&incomplete, error, sizeof(error)) != 0,
           "missing sink coverage prevents seal");
    int missing_ok = nxinput_gptk_live_feed(
                         &incomplete, NXINPUT_GPTK_A, 1, 1.0f) ==
                     NXINPUT_GPTK_LIVE_PASSTHROUGH;
    expect(missing_ok, "unsealed runtime cannot suppress native input");
    if (missing_ok) {
      printf("NXGPTK_PROOF\tSAFETY\tmissing_sink\tPASSTHROUGH\t0\n");
    }
  }

  /* 7. A sink that fails after invocation is fatal for that edge and is not
   * falsely reported as delivered. */
  {
    nxinput_gptk_live failing;
    char error[160] = { 0 };
    remove_file("NEXTOSCONTROLLERS.gptk");
    expect(load(&map, &receipt) == 0, "failed-ACK fixture loads");
    nxinput_gptk_live_init(&failing, &map);
    for (size_t i = 0; i < ALLOWED_COUNT; i++) {
      int rc;
      if (is_vector_action(ALLOWED[i])) {
        rc = nxinput_gptk_live_register_vector(
            &failing, ALLOWED[i], record_vector, (void *)"vector");
      } else {
        rc = nxinput_gptk_live_register(
            &failing, ALLOWED[i],
            strcmp(ALLOWED[i], "menu.accept") == 0 ? fail_sink : record_sink,
            (void *)"sink");
      }
      expect(rc == 0, "failed-ACK sink coverage registers");
    }
    expect(nxinput_gptk_live_seal(&failing, error, sizeof(error)) == 0,
           "failed-ACK fixture seals before invocation");
    expect(nxinput_gptk_live_set_context(
               &failing, NXINPUT_GPTK_CONTEXT_MENU, "scene:ui") == 0,
           "failed-ACK fixture proves menu context");
    int failed_ack_ok = nxinput_gptk_live_feed(
                            &failing, NXINPUT_GPTK_A, 1, 1.0f) ==
                        NXINPUT_GPTK_LIVE_FATAL;
    expect(failed_ack_ok,
           "failed sink ACK becomes fatal without a false delivery");
    if (failed_ack_ok) {
      printf("NXGPTK_PROOF\tSAFETY\tfailed_ack\tFATAL\t0\n");
    }
  }

  /* 8. A release ACK can fail during a context transition, outside feed(). The
   * checked API must expose that failure so the Godot lifecycle exits nonzero
   * instead of publishing health or silently returning to native input. */
  {
    nxinput_gptk_live failing_release;
    char error[160] = { 0 };
    remove_file("NEXTOSCONTROLLERS.gptk");
    expect(load(&map, &receipt) == 0, "failed-release fixture loads");
    nxinput_gptk_live_init(&failing_release, &map);
    for (size_t i = 0; i < ALLOWED_COUNT; i++) {
      int rc;
      if (is_vector_action(ALLOWED[i])) {
        rc = nxinput_gptk_live_register_vector(
            &failing_release, ALLOWED[i], record_vector, (void *)"vector");
      } else {
        rc = nxinput_gptk_live_register(
            &failing_release, ALLOWED[i],
            strcmp(ALLOWED[i], "menu.accept") == 0
                ? fail_release_sink : record_sink,
            (void *)"sink");
      }
      expect(rc == 0, "failed-release sink coverage registers");
    }
    expect(nxinput_gptk_live_seal(
               &failing_release, error, sizeof(error)) == 0,
           "failed-release fixture seals");
    expect(nxinput_gptk_live_set_context(
               &failing_release, NXINPUT_GPTK_CONTEXT_MENU, "scene:ui") == 0,
           "failed-release fixture proves menu context");
    expect(nxinput_gptk_live_feed(
               &failing_release, NXINPUT_GPTK_A, 1, 1.0f) ==
               NXINPUT_GPTK_LIVE_DELIVERED,
           "failed-release fixture first ACKs its press");
    expect(nxinput_gptk_live_clear_context_checked(&failing_release) != 0 &&
               nxinput_gptk_live_is_fatal(&failing_release),
           "release ACK failure is observable and sticky fatal");
  }

  /* 9. Invalid owner file: preserved byte-for-byte, default selected. */
  const char *broken = "format = WRONG/9\n[menu]\nA = menu.accept\n";
  write_file("NEXTOSCONTROLLERS.gptk", broken);
  expect(load(&map, &receipt) == 0, "load still succeeds via default");
  expect(receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED,
         "invalid owner rejected, default selected");
  {
    char path[512];
    char buffer[256] = { 0 };
    snprintf(path, sizeof(path), "%s/NEXTOSCONTROLLERS.gptk", g_dir);
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buffer, 1, sizeof(buffer) - 1, f) : 0;
    if (f) { fclose(f); }
    expect(n == strlen(broken) && strcmp(buffer, broken) == 0,
           "owner copy preserved byte-for-byte");
  }

  /* 10. Owner action outside the adapter allowlist is rejected. */
  write_file("NEXTOSCONTROLLERS.gptk",
             "format = NEXTOS_CONTROLLERS/1\n"
             "[menu]\nA = hacker.action\n[gameplay]\nA = player.roll\n");
  expect(load(&map, &receipt) == 0, "allowlist violation falls to default");
  expect(receipt.source == NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED,
         "unknown action rejected by the exact allowlist");

  if (argc == 2) {
    prove_generated_default(argv[1]);
  }

  remove_file("NEXTOSCONTROLLERS.gptk");
  remove_file("defaults/NEXTOSCONTROLLERS.gptk");
  rmdir(defaults);
  rmdir(g_dir);
  if (g_fail) {
    fprintf(stderr, "controls-runtime host gate: FAIL\n");
    return 1;
  }
  printf("controls-runtime host gate: PASS (load/live-ACK/swap/context/vector/fail-closed)\n");
  return 0;
}
