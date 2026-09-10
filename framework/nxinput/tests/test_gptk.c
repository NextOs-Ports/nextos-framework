/* SPDX-License-Identifier: GPL-3.0-only */
/* Host unit tests for the NEXTOSCONTROLLERS.gptk core. Pure C99, no SDL, no
 * devices: everything runs against in-memory buffers and fake sinks. */
#include "nxinput_gptk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *expression, const char *file, int line) {
  (void)fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
  exit(1);
}

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression))                                                        \
      fail(#expression, __FILE__, __LINE__);                                  \
  } while (0)

static void announce(const char *name) {
  (void)printf("gptk: %s\n", name);
}

/* ---- sink call recorder ------------------------------------------------ */

#define LOG_MAX 64u

typedef struct sink_call {
  char sink_name[32];
  char action[NXINPUT_GPTK_ACTION_MAX + 1u];
  int pressed;
  float value;
} sink_call;

static sink_call call_log[LOG_MAX];
static size_t call_count;

static void log_reset(void) {
  memset(call_log, 0, sizeof call_log);
  call_count = 0u;
}

/* user points at a string literal naming the fake sink. */
static void recording_sink(void *user, const char *action, int pressed,
                           float value) {
  CHECK(call_count < LOG_MAX);
  (void)snprintf(call_log[call_count].sink_name,
                 sizeof call_log[call_count].sink_name, "%s",
                 (const char *)user);
  (void)snprintf(call_log[call_count].action,
                 sizeof call_log[call_count].action, "%s", action);
  call_log[call_count].pressed = pressed;
  call_log[call_count].value = value;
  call_count++;
}

static int call_matches(size_t index, const char *sink_name,
                        const char *action, int pressed) {
  return index < call_count &&
         strcmp(call_log[index].sink_name, sink_name) == 0 &&
         strcmp(call_log[index].action, action) == 0 &&
         call_log[index].pressed == pressed;
}

/* ---- fixtures ---------------------------------------------------------- */

static const char example_file[] =
    "# NEXTOSCONTROLLERS example from the spec\n"
    "format = NEXTOS_CONTROLLERS/1\n"
    "port = example_port\n"
    "\n"
    "[menu]\n"
    "A = ui.confirm\n"
    "B = ui.cancel\n"
    "RIGHT_STICK = cursor.move\n"
    "R3 = cursor.click\n"
    "\n"
    "[gameplay]\n"
    "A = player.jump\n"
    "B = player.action\n"
    "RIGHT_STICK = camera.move\n";

/* Same file with the A and B gameplay actions swapped. */
static const char swapped_file[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "[menu]\n"
    "A = ui.confirm\n"
    "B = ui.cancel\n"
    "[gameplay]\n"
    "A = player.action\n"
    "B = player.jump\n";

static void test_parse_example(void) {
  nxinput_gptk map;
  char error[128] = "";

  announce("parse example file and lookups");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  CHECK(strcmp(map.port, "example_port") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                                   NXINPUT_GPTK_A), "ui.confirm") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                                   NXINPUT_GPTK_B), "ui.cancel") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                                   NXINPUT_GPTK_RIGHT_STICK),
               "cursor.move") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                                   NXINPUT_GPTK_R3), "cursor.click") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                   NXINPUT_GPTK_A), "player.jump") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                   NXINPUT_GPTK_B), "player.action") == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                   NXINPUT_GPTK_RIGHT_STICK),
               "camera.move") == 0);
  /* Unmapped and out-of-range lookups are NULL, never garbage. */
  CHECK(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                            NXINPUT_GPTK_START) == 0);
  CHECK(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_CURSOR,
                            NXINPUT_GPTK_A) == 0);
  CHECK(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU, -1) == 0);
  CHECK(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                            (int)NXINPUT_GPTK_CONTROL_COUNT) == 0);
  /* Digital controls bridge to nxinput_button; analog ones do not. */
  CHECK(nxinput_gptk_control_button(NXINPUT_GPTK_A) == 0);
  CHECK(nxinput_gptk_control_button(NXINPUT_GPTK_SELECT) == 4);
  CHECK(nxinput_gptk_control_button(NXINPUT_GPTK_L2) == -1);
  CHECK(nxinput_gptk_control_button(NXINPUT_GPTK_RIGHT_STICK) == -1);
}

static void test_ab_swap_reaches_sinks(void) {
  nxinput_gptk base;
  nxinput_gptk swapped;
  nxinput_gptk_dispatcher d;
  char error[128] = "";

  announce("A/B swap fixture changes what the sinks receive");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &base, error,
                           sizeof error) == 0);
  CHECK(nxinput_gptk_parse(swapped_file, strlen(swapped_file), &swapped,
                           error, sizeof error) == 0);

  /* Baseline: A in gameplay is player.jump. */
  nxinput_gptk_dispatcher_init(&d, &base);
  CHECK(nxinput_gptk_dispatcher_register(&d, "player.jump", recording_sink,
                                         (void *)"game") == 0);
  CHECK(nxinput_gptk_dispatcher_register(&d, "player.action", recording_sink,
                                         (void *)"game") == 0);
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
  log_reset();
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 2u);
  CHECK(call_matches(0u, "game", "player.jump", 1));
  CHECK(call_matches(1u, "game", "player.jump", 0));

  /* Swapped file, same physical presses: sinks now see player.action. */
  nxinput_gptk_dispatcher_init(&d, &swapped);
  CHECK(nxinput_gptk_dispatcher_register(&d, "player.jump", recording_sink,
                                         (void *)"game") == 0);
  CHECK(nxinput_gptk_dispatcher_register(&d, "player.action", recording_sink,
                                         (void *)"game") == 0);
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
  log_reset();
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 2u);
  CHECK(call_matches(0u, "game", "player.action", 1));
  CHECK(call_matches(1u, "game", "player.action", 0));
}

static void test_multi_sink_single_logical_press(void) {
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  char error[128] = "";

  announce("multi-sink: one press = one event per sink (Action Squad)");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  nxinput_gptk_dispatcher_init(&d, &map);
  CHECK(nxinput_gptk_dispatcher_register(&d, "ui.confirm", recording_sink,
                                         (void *)"ui-manager") == 0);
  CHECK(nxinput_gptk_dispatcher_register(&d, "ui.confirm", recording_sink,
                                         (void *)"gameplay-channel") == 0);
  /* Dispatcher starts in MENU already. */
  log_reset();
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  /* Exactly press,release per sink, no duplicated logical press. */
  CHECK(call_count == 4u);
  CHECK(call_matches(0u, "ui-manager", "ui.confirm", 1));
  CHECK(call_matches(1u, "gameplay-channel", "ui.confirm", 1));
  CHECK(call_matches(2u, "ui-manager", "ui.confirm", 0));
  CHECK(call_matches(3u, "gameplay-channel", "ui.confirm", 0));
}

static void test_latch_release_on_context_switch(void) {
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  char error[128] = "";

  announce("context switch releases latched controls first");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  nxinput_gptk_dispatcher_init(&d, &map);
  CHECK(nxinput_gptk_dispatcher_register(&d, "ui.confirm", recording_sink,
                                         (void *)"menu") == 0);
  CHECK(nxinput_gptk_dispatcher_register(&d, "player.jump", recording_sink,
                                         (void *)"game") == 0);
  log_reset();
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f); /* held */
  CHECK(call_count == 1u);
  CHECK(call_matches(0u, "menu", "ui.confirm", 1));

  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
  /* The menu action received its release before the switch completed. */
  CHECK(call_count == 2u);
  CHECK(call_matches(1u, "menu", "ui.confirm", 0));

  /* The physical release that follows must not leak into gameplay. */
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 2u);

  /* A fresh press now dispatches the gameplay action. */
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  CHECK(call_count == 3u);
  CHECK(call_matches(2u, "game", "player.jump", 1));
}

static void expect_reject(const char *text, size_t length, int code,
                          const char *what) {
  nxinput_gptk map;
  char error[160] = "";
  char prefix[16];
  int result = nxinput_gptk_parse(text, length, &map, error, sizeof error);

  (void)printf("gptk: reject %s -> %d (%s)\n", what, result, error);
  CHECK(result == code);
  (void)snprintf(prefix, sizeof prefix, "NXI%04d", code);
  CHECK(strncmp(error, prefix, strlen(prefix)) == 0);
  /* Fail closed: nothing from a rejected file may survive in the result. */
  CHECK(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_MENU,
                            NXINPUT_GPTK_A) == 0);
}

static void test_negatives(void) {
  announce("negative parses fail closed with stable NXI codes");

  {
    /* C4: the magic alone selects the schema, with no "looks like v2"
     * promotion. /4 is unknown and refused outright. */
    static const char wrong_magic[] =
        "format = NEXTOS_CONTROLLERS/4\n[menu]\nA = ui.confirm\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(wrong_magic, strlen(wrong_magic),
                  NXINPUT_GPTK_ERR_BAD_MAGIC, "wrong magic version");
  }
  {
    /* 0.10.0: /3 is a real schema, but it REQUIRES exactly one lowercase
     * FACE_LAYOUT line; a /3 body without it fails closed as malformed,
     * never silently downgrades to V2 semantics. */
    static const char v3_missing_layout[] =
        "format = NEXTOS_CONTROLLERS/3\n[menu]\nA = ui.confirm\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(v3_missing_layout, strlen(v3_missing_layout),
                  NXINPUT_GPTK_ERR_MALFORMED, "v3 without FACE_LAYOUT");
  }
  {
    /* A V2 magic with an INCOMPLETE section is a V2 error, not a magic
     * error: the owner must see every control. */
    static const char short_v2[] =
        "format = NEXTOS_CONTROLLERS/2\n[menu]\nA = ui.confirm\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(short_v2, strlen(short_v2),
                  NXINPUT_GPTK_ERR_MALFORMED, "incomplete V2 section");
  }
  {
    /* null/native are V2-only: a V1 file must not gain them silently. */
    static const char v1_null[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = null\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(v1_null, strlen(v1_null),
                  NXINPUT_GPTK_ERR_MALFORMED, "null in a V1 file");
  }
  {
    /* gptokeyb-style file: no magic, keyboard-key mapping. This format is
     * NextOS-own and never maps controls to keyboard keys. */
    static const char gptokeyb[] =
        "back = esc\nstart = enter\na = z\nb = x\n";
    expect_reject(gptokeyb, strlen(gptokeyb), NXINPUT_GPTK_ERR_BAD_MAGIC,
                  "gptokeyb-style file");
  }
  {
    /* Chrono Trigger evdev regression: raw numeric codes must never be
     * accepted as controls. */
    static const char numeric[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\n304 = ui.confirm\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(numeric, strlen(numeric), NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                  "numeric evdev control token");
  }
  {
    static const char duplicate[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
        "A = ui.cancel\n[gameplay]\nA = player.jump\n";
    expect_reject(duplicate, strlen(duplicate), NXINPUT_GPTK_ERR_DUPLICATE,
                  "duplicate control in section");
  }
  {
    static const char bad_action[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = UPPER.Case\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(bad_action, strlen(bad_action),
                  NXINPUT_GPTK_ERR_MALFORMED, "uppercase action name");
  }
  {
    static const char no_dot[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = confirm\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(no_dot, strlen(no_dot), NXINPUT_GPTK_ERR_MALFORMED,
                  "action without a namespace dot");
  }
  {
    static const char unknown_section[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
        "[keyboard]\n[gameplay]\nA = player.jump\n";
    expect_reject(unknown_section, strlen(unknown_section),
                  NXINPUT_GPTK_ERR_UNKNOWN_NAME, "unknown section");
  }
  {
    static const char missing_gameplay[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n";
    expect_reject(missing_gameplay, strlen(missing_gameplay),
                  NXINPUT_GPTK_ERR_MALFORMED, "missing gameplay section");
  }
  {
    /* 513 non-comment lines: magic + fill. Build in a static buffer. */
    static char big[16384];
    size_t offset = 0u;
    size_t line;

    offset += (size_t)snprintf(big + offset, sizeof big - offset,
                               "format = NEXTOS_CONTROLLERS/1\n[menu]\n");
    for (line = 0u; line < 511u; line++) {
      offset += (size_t)snprintf(big + offset, sizeof big - offset, "\n");
    }
    offset += (size_t)snprintf(big + offset, sizeof big - offset,
                               "A = ui.confirm\n");
    CHECK(offset < sizeof big);
    expect_reject(big, offset, NXINPUT_GPTK_ERR_TOO_LARGE,
                  "more than 512 lines");
  }
  {
    /* Larger than 65536 bytes. Length alone must trip the limit. */
    static char huge[NXINPUT_GPTK_MAX_BYTES + 2u];

    memset(huge, '#', sizeof huge);
    expect_reject(huge, sizeof huge, NXINPUT_GPTK_ERR_TOO_LARGE,
                  "input larger than 65536 bytes");
  }
  {
    static const char with_nul[] =
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.con\0firm\n"
        "[gameplay]\nA = player.jump\n";
    expect_reject(with_nul, sizeof with_nul - 1u,
                  NXINPUT_GPTK_ERR_BAD_BYTES, "embedded NUL byte");
  }
  {
    static const char bad_utf8[] =
        "format = NEXTOS_CONTROLLERS/1\n# bad \xC0\xAF\n[menu]\n"
        "A = ui.confirm\n[gameplay]\nA = player.jump\n";
    expect_reject(bad_utf8, sizeof bad_utf8 - 1u,
                  NXINPUT_GPTK_ERR_BAD_BYTES, "invalid UTF-8 sequence");
  }
}

/* ---- V3 tuning --------------------------------------------------------- */

static int near_f(float a, float b) {
  float d = a - b;

  return d > -0.0005f && d < 0.0005f;
}

/* The exact tuning block from the V3 spec, on top of a valid skeleton. */
static const char tuning_file[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "[menu]\n"
    "A = ui.confirm\n"
    "[gameplay]\n"
    "A = player.jump\n"
    "[cursor]\n"
    "LEFT_STICK = cursor.move\n"
    "speed = 1.00\n"
    "deadzone = 0.15\n"
    "response_curve = 1.60\n"
    "acceleration = 0.35\n"
    "smoothing_ms = 70\n"
    "\n"
    "[camera]\n"
    "sensitivity_x = 1.25\n"
    "sensitivity_y = 0.85\n"
    "deadzone = 0.20\n"
    "response_curve = 1.00\n"
    "invert_x = false\n"
    "invert_y = true\n"
    "authority = native\n";

static void test_tuning_parse(void) {
  nxinput_gptk map;
  nxinput_gptk_cursor_tuning cursor;
  nxinput_gptk_camera_tuning camera;
  char error[160] = "";

  announce("tuning: spec block parses, getters return the values + flags");
  CHECK(nxinput_gptk_parse(tuning_file, strlen(tuning_file), &map, error,
                           sizeof error) == 0);
  /* Mapping lines still coexist with tuning keys in [cursor]. */
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_CURSOR,
                                   NXINPUT_GPTK_LEFT_STICK),
               "cursor.move") == 0);
  CHECK(map.camera_present == 1);

  nxinput_gptk_cursor_tuning_get(&map, &cursor);
  CHECK(near_f(cursor.speed, 1.00f) && cursor.speed_set == 1u);
  CHECK(near_f(cursor.deadzone, 0.15f) && cursor.deadzone_set == 1u);
  CHECK(near_f(cursor.response_curve, 1.60f) &&
        cursor.response_curve_set == 1u);
  CHECK(near_f(cursor.acceleration, 0.35f) && cursor.acceleration_set == 1u);
  CHECK(near_f(cursor.smoothing_ms, 70.0f) && cursor.smoothing_ms_set == 1u);

  nxinput_gptk_camera_tuning_get(&map, &camera);
  CHECK(near_f(camera.sensitivity_x, 1.25f) && camera.sensitivity_x_set == 1u);
  CHECK(near_f(camera.sensitivity_y, 0.85f) && camera.sensitivity_y_set == 1u);
  CHECK(near_f(camera.deadzone, 0.20f) && camera.deadzone_set == 1u);
  CHECK(near_f(camera.response_curve, 1.00f) &&
        camera.response_curve_set == 1u);
  CHECK(camera.invert_x == 0u && camera.invert_x_set == 1u);
  CHECK(camera.invert_y == 1u && camera.invert_y_set == 1u);
  CHECK(camera.authority == (uint8_t)NXINPUT_GPTK_AUTHORITY_NATIVE &&
        camera.authority_set == 1u);
}

static void test_tuning_defaults(void) {
  nxinput_gptk map;
  nxinput_gptk_cursor_tuning cursor;
  nxinput_gptk_camera_tuning camera;
  char error[160] = "";

  announce("tuning: file without tuning keys gets defaults, flags all 0");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  nxinput_gptk_cursor_tuning_get(&map, &cursor);
  CHECK(near_f(cursor.speed, 1.0f) && cursor.speed_set == 0u);
  CHECK(near_f(cursor.deadzone, 0.15f) && cursor.deadzone_set == 0u);
  CHECK(near_f(cursor.response_curve, 1.0f) &&
        cursor.response_curve_set == 0u);
  CHECK(near_f(cursor.acceleration, 0.0f) && cursor.acceleration_set == 0u);
  CHECK(near_f(cursor.smoothing_ms, 0.0f) && cursor.smoothing_ms_set == 0u);

  nxinput_gptk_camera_tuning_get(&map, &camera);
  CHECK(near_f(camera.sensitivity_x, 1.0f) && camera.sensitivity_x_set == 0u);
  CHECK(near_f(camera.sensitivity_y, 1.0f) && camera.sensitivity_y_set == 0u);
  CHECK(near_f(camera.deadzone, 0.15f) && camera.deadzone_set == 0u);
  CHECK(near_f(camera.response_curve, 1.0f) &&
        camera.response_curve_set == 0u);
  CHECK(camera.invert_x == 0u && camera.invert_y == 0u);
  CHECK(camera.authority == (uint8_t)NXINPUT_GPTK_AUTHORITY_NEXTOS &&
        camera.authority_set == 0u);
  CHECK(map.camera_present == 0);

  /* NULL map: getters still hand out fully-populated defaults. */
  nxinput_gptk_cursor_tuning_get(0, &cursor);
  CHECK(near_f(cursor.speed, 1.0f));
  nxinput_gptk_camera_tuning_get(0, &camera);
  CHECK(near_f(camera.sensitivity_x, 1.0f));
}

/* Wrap one bad line in an otherwise valid file and expect a rejection. */
static void expect_tuning_reject(const char *section, const char *bad_line,
                                 int code, const char *what) {
  char text[512];
  int written = snprintf(text, sizeof text,
                         "format = NEXTOS_CONTROLLERS/1\n"
                         "[menu]\nA = ui.confirm\n"
                         "[gameplay]\nA = player.jump\n"
                         "[%s]\n%s\n",
                         section, bad_line);

  CHECK(written > 0 && (size_t)written < sizeof text);
  expect_reject(text, (size_t)written, code, what);
}

static void test_tuning_negatives(void) {
  announce("tuning negatives: strict numbers, bounds, enums, duplicates");

  expect_tuning_reject("cursor", "speed = nan", NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning nan");
  expect_tuning_reject("cursor", "speed = inf", NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning inf");
  expect_tuning_reject("cursor", "speed = 0x1p2",
                       NXINPUT_GPTK_ERR_MALFORMED, "tuning hex float");
  expect_tuning_reject("cursor", "speed = 1e2", NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning exponent");
  expect_tuning_reject("cursor", "speed =", NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning empty value");
  expect_tuning_reject("cursor", "speed = 99", NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning speed out of bounds");
  expect_tuning_reject("cursor", "speed = -1", NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning negative speed");
  expect_tuning_reject("cursor", "deadzone = 0.95",
                       NXINPUT_GPTK_ERR_MALFORMED,
                       "tuning deadzone out of bounds");
  expect_tuning_reject("cursor", "speed = 1.0\nspeed = 2.0",
                       NXINPUT_GPTK_ERR_DUPLICATE, "duplicate speed");
  expect_tuning_reject("cursor", "warp = 1.0",
                       NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                       "unknown cursor tuning key");
  expect_tuning_reject("camera", "invert_x = maybe",
                       NXINPUT_GPTK_ERR_MALFORMED, "invert_x=maybe");
  expect_tuning_reject("camera", "invert_x = TRUE",
                       NXINPUT_GPTK_ERR_MALFORMED, "invert_x=TRUE");
  expect_tuning_reject("camera", "authority = root",
                       NXINPUT_GPTK_ERR_MALFORMED, "authority=root");
  expect_tuning_reject("camera", "sensitivity_x = 0.01",
                       NXINPUT_GPTK_ERR_MALFORMED,
                       "sensitivity out of bounds");
  expect_tuning_reject("camera", "speed = 1.0",
                       NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                       "cursor key inside [camera]");
  expect_tuning_reject("camera", "A = ui.confirm",
                       NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                       "mapping line inside [camera]");
  /* Tuning keys never leak into the mapping contexts. */
  expect_tuning_reject("menu", "speed = 1.00",
                       NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                       "tuning key inside [menu]");
  expect_tuning_reject("gameplay", "deadzone = 0.15",
                       NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                       "tuning key inside [gameplay]");
}

static void test_validate_actions(void) {
  nxinput_gptk map;
  char error[160] = "";
  static const char *const small_allowlist[] = {"ui.confirm", "ui.cancel"};
  static const char *const full_allowlist[] = {
      "ui.confirm", "ui.cancel", "cursor.move", "cursor.click",
      "player.jump", "player.action", "camera.move"};

  announce("validate_actions against adapter allowlist");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  CHECK(nxinput_gptk_validate_actions(&map, full_allowlist, 7u, error,
                                      sizeof error) == 0);
  CHECK(nxinput_gptk_validate_actions(&map, small_allowlist, 2u, error,
                                      sizeof error) ==
        NXINPUT_GPTK_ERR_UNKNOWN_NAME);
  CHECK(strncmp(error, "NXI1001", 7u) == 0);
}

static void test_edge_semantics(void) {
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  char error[128] = "";

  announce("edge semantics: repeats and stray releases emit nothing");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  nxinput_gptk_dispatcher_init(&d, &map);
  CHECK(nxinput_gptk_dispatcher_register(&d, "ui.confirm", recording_sink,
                                         (void *)"ui") == 0);
  log_reset();

  /* Release without any latch: nothing. */
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 0u);

  /* Repeated pressed=1 emits exactly once. */
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  CHECK(call_count == 1u);
  CHECK(call_matches(0u, "ui", "ui.confirm", 1));

  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 2u);
  CHECK(call_matches(1u, "ui", "ui.confirm", 0));

  /* Second release: nothing again. */
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 2u);

  /* Analog value is clamped into 0..1 on the way through. */
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 3.5f);
  CHECK(call_count == 3u);
  CHECK(call_log[2].value <= 1.0f && call_log[2].value >= 0.0f);
}

static void test_source_authority_and_delivery_count(void) {
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  nxinput_gptk_source_guard guard;
  char error[128] = "";
  uint32_t a_bit = UINT32_C(1) << (unsigned)NXINPUT_GPTK_A;

  announce("primary/fallback authority: one press = one delivery");
  CHECK(nxinput_gptk_parse(example_file, strlen(example_file), &map, error,
                           sizeof error) == 0);
  nxinput_gptk_dispatcher_init(&d, &map);
  nxinput_gptk_source_guard_init(&guard, &d);
  CHECK(nxinput_gptk_dispatcher_register(&d, "ui.confirm", recording_sink,
                                         (void *)"ui") == 0);
  CHECK(nxinput_gptk_dispatcher_register(&d, "ui.cancel", recording_sink,
                                         (void *)"ui") == 0);
  nxinput_gptk_dispatcher_set_primary_mask(
      &d, &guard, a_bit | UINT32_C(0x80000000));
  CHECK(nxinput_gptk_dispatcher_primary_mask(&guard) == a_bit);
  log_reset();

  /* A is owned by the complete SDL/PortMaster source: fallback is muted. */
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 0u);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_PRIMARY, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_PRIMARY, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_PRIMARY, NXINPUT_GPTK_A, 0, 0.0f);
  CHECK(call_count == 2u);
  CHECK(call_matches(0u, "ui", "ui.confirm", 1));
  CHECK(call_matches(1u, "ui", "ui.confirm", 0));

  /* B has no primary authority bit. If SDL and evdev both observe it, their
   * states are ORed: exactly one press, no premature release, one release. */
  log_reset();
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_B, 1, 1.0f);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_PRIMARY, NXINPUT_GPTK_B, 1, 1.0f);
  CHECK(call_count == 1u);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_B, 0, 0.0f);
  CHECK(call_count == 1u);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_PRIMARY, NXINPUT_GPTK_B, 0, 0.0f);
  CHECK(call_count == 2u);
  CHECK(call_matches(0u, "ui", "ui.cancel", 1));
  CHECK(call_matches(1u, "ui", "ui.cancel", 0));

  /* An authority hand-off releases an active fallback latch, never sticks. */
  log_reset();
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_B, 1, 1.0f);
  nxinput_gptk_dispatcher_set_primary_mask(&d, &guard, a_bit |
                                                   (UINT32_C(1) <<
                                                    (unsigned)NXINPUT_GPTK_B));
  CHECK(call_count == 2u);
  CHECK(call_matches(0u, "ui", "ui.cancel", 1));
  CHECK(call_matches(1u, "ui", "ui.cancel", 0));

  /* Focus/hot-unplug reset preserves authority but releases guard-owned
   * state once; the later physical release cannot duplicate it. */
  nxinput_gptk_dispatcher_set_primary_mask(&d, &guard, a_bit);
  log_reset();
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_B, 1, 1.0f);
  nxinput_gptk_source_guard_reset(&d, &guard);
  nxinput_gptk_dispatcher_feed_source(
      &d, &guard, NXINPUT_GPTK_SOURCE_FALLBACK, NXINPUT_GPTK_B, 0, 0.0f);
  CHECK(call_count == 2u);
  CHECK(call_matches(0u, "ui", "ui.cancel", 1));
  CHECK(call_matches(1u, "ui", "ui.cancel", 0));
  CHECK(nxinput_gptk_dispatcher_primary_mask(&guard) == a_bit);
}

int main(void) {
  test_parse_example();
  test_ab_swap_reaches_sinks();
  test_multi_sink_single_logical_press();
  test_latch_release_on_context_switch();
  test_negatives();
  test_tuning_parse();
  test_tuning_defaults();
  test_tuning_negatives();
  test_validate_actions();
  test_edge_semantics();
  test_source_authority_and_delivery_count();
  (void)puts("gptk tests: ok");
  return 0;
}
