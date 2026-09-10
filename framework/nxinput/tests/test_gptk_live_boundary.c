/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_gptk_live.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int scalar_calls;
static int vector_calls;
static int fail_ack;
static char last_action[80];

#define CHECK(expr, message)                                                   \
  do {                                                                         \
    if (!(expr)) {                                                             \
      (void)fprintf(stderr, "gptk-live-boundary FAIL: %s\n", (message));       \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static int scalar_sink(void *user, const char *action, int pressed,
                       float value) {
  (void)user;
  (void)pressed;
  (void)value;
  scalar_calls++;
  (void)snprintf(last_action, sizeof last_action, "%s", action);
  return fail_ack ? -1 : 0;
}

static int vector_sink(void *user, const char *action, float x, float y) {
  (void)user;
  (void)x;
  (void)y;
  vector_calls++;
  (void)snprintf(last_action, sizeof last_action, "%s", action);
  return fail_ack ? -1 : 0;
}

static void action(nxinput_gptk *map, int context, int control,
                   const char *name) {
  map->kind[context][control] = (uint8_t)NXINPUT_GPTK_BINDING_ACTION;
  (void)snprintf(map->action[context][control],
                 sizeof map->action[context][control], "%s", name);
}

int main(void) {
  nxinput_gptk map;
  nxinput_gptk_live live;
  char error[128];

  memset(&map, 0, sizeof map);
  map.api_version = NXINPUT_GPTK_API_VERSION;
  map.schema_version = NXINPUT_GPTK_SCHEMA_V2;
  map.context_present[NXINPUT_GPTK_CONTEXT_MENU] = 1;
  map.context_present[NXINPUT_GPTK_CONTEXT_GAMEPLAY] = 1;
  action(&map, NXINPUT_GPTK_CONTEXT_MENU, NXINPUT_GPTK_A, "menu.accept");
  map.kind[NXINPUT_GPTK_CONTEXT_MENU][NXINPUT_GPTK_B] =
      (uint8_t)NXINPUT_GPTK_BINDING_NULL;
  action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_A,
         "player.roll");
  action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY, NXINPUT_GPTK_RIGHT_STICK,
         "player.look");

  nxinput_gptk_live_init(&live, &map);
  CHECK(!nxinput_gptk_live_context_proven(&live),
        "init must start with context UNPROVEN");
  CHECK(!nxinput_gptk_live_should_consume(&live, NXINPUT_GPTK_A),
        "UNPROVEN must never authorize native suppression");
  CHECK(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
            NXINPUT_GPTK_LIVE_PASSTHROUGH,
        "UNPROVEN event must pass through natively");

  CHECK(nxinput_gptk_live_register(&live, "menu.accept", scalar_sink, 0) == 0,
        "register menu sink");
  error[0] = '\0';
  CHECK(nxinput_gptk_live_seal(&live, error, sizeof error) != 0,
        "missing gameplay/vector sinks must block seal");
  CHECK(!nxinput_gptk_live_should_consume(&live, NXINPUT_GPTK_B),
        "unsealed null must not suppress");

  CHECK(nxinput_gptk_live_register(&live, "player.roll", scalar_sink, 0) == 0,
        "register gameplay sink");
  CHECK(nxinput_gptk_live_register_vector(&live, "player.look", vector_sink,
                                           0) == 0,
        "register vector sink");
  CHECK(nxinput_gptk_live_seal(&live, error, sizeof error) == 0,
        "complete ACK sink coverage seals");
  CHECK(nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_MENU,
                                      "scene:main_menu") == 0,
        "real menu state proves context");
  CHECK(nxinput_gptk_live_should_consume(&live, NXINPUT_GPTK_A),
        "known mapped action may consume");
  CHECK(nxinput_gptk_live_should_consume(&live, NXINPUT_GPTK_B),
        "known null may suppress");
  /* 0.11.6: A was pressed natively while the context was UNPROVEN (above);
   * its release arrives after the proof. The action never latched it, so
   * the release belongs to the native path -- DELIVERED here made the
   * adapter drop the native release and the engine kept A down. */
  CHECK(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 0, 0.0f) ==
            NXINPUT_GPTK_LIVE_PASSTHROUGH && scalar_calls == 0,
        "MUTANT killed: release of a control the action never latched reported DELIVERED (native release dropped, button stuck)");
  CHECK(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
            NXINPUT_GPTK_LIVE_DELIVERED &&
            strcmp(last_action, "menu.accept") == 0 && scalar_calls == 1,
        "event -> menu decision -> real ACK sink");
  CHECK(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_B, 1, 1.0f) ==
            NXINPUT_GPTK_LIVE_SUPPRESSED,
        "null suppresses only after context proof");

  CHECK(nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                      "scene:world") == 0,
        "gameplay context transition");
  CHECK(scalar_calls == 2,
        "context transition ACKs release in the old context");
  CHECK(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
            NXINPUT_GPTK_LIVE_DELIVERED &&
            strcmp(last_action, "player.roll") == 0,
        "same physical event reaches gameplay action after proof");
  CHECK(nxinput_gptk_live_feed_vector(&live, NXINPUT_GPTK_RIGHT_STICK,
                                      0.5f, -0.5f) ==
            NXINPUT_GPTK_LIVE_DELIVERED && vector_calls == 1 &&
            strcmp(last_action, "player.look") == 0,
        "vector event reaches ACK vector sink");

  nxinput_gptk_live_clear_context(&live);
  CHECK(!nxinput_gptk_live_should_consume(&live, NXINPUT_GPTK_A) &&
            nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
                NXINPUT_GPTK_LIVE_PASSTHROUGH,
        "lost context restores native passthrough");
  CHECK(nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_MENU,
                                      "unknown context") != 0 &&
            !nxinput_gptk_live_context_proven(&live),
        "unbounded/unverifiable context source is rejected safely");

  CHECK(nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_MENU,
                                      "scene:main_menu") == 0,
        "context can be reproved");
  fail_ack = 1;
  CHECK(nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
            NXINPUT_GPTK_LIVE_FATAL &&
            !nxinput_gptk_live_context_proven(&live) &&
            !nxinput_gptk_live_should_consume(&live, NXINPUT_GPTK_A),
        "failed real sink invalidates runtime without native replay");

  /* A release failure used to disappear behind the void clear API. */
  fail_ack = 0;
  nxinput_gptk_live_init(&live, &map);
  CHECK(nxinput_gptk_live_register(&live, "menu.accept", scalar_sink, 0) == 0 &&
            nxinput_gptk_live_register(&live, "player.roll", scalar_sink, 0) == 0 &&
            nxinput_gptk_live_register_vector(&live, "player.look", vector_sink, 0) == 0 &&
            nxinput_gptk_live_seal(&live, error, sizeof error) == 0 &&
            nxinput_gptk_live_set_context(&live, NXINPUT_GPTK_CONTEXT_MENU,
                                          "scene:main_menu") == 0 &&
            nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) ==
                NXINPUT_GPTK_LIVE_DELIVERED,
        "release-failure fixture is latched");
  fail_ack = 1;
  CHECK(nxinput_gptk_live_clear_context_checked(&live) != 0 &&
            nxinput_gptk_live_is_fatal(&live),
        "checked clear exposes a release ACK failure as terminal");

  CHECK(strcmp(nxinput_gptk_runtime_marker(), "nxinput-gptk-runtime/3") == 0,
        "runtime marker version");
  CHECK(strcmp(nxinput_gptk_event_evidence_schema(),
               "nxinput-gptk-event-evidence/1") == 0,
        "event evidence schema token");

  if (failures != 0) {
    return 1;
  }
  (void)printf("gptk live boundary: PASS (UNPROVEN -> passthrough; "
               "event -> decision -> sink -> ACK; checked release fatal)\n");
  return 0;
}
