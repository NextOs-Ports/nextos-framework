/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_android_gptk.h"
#include "nxinput_exit_chord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;

#define CHECK(condition)                                                      \
  do {                                                                        \
    checks++;                                                                 \
    if (!(condition)) {                                                       \
      fprintf(stderr, "android-gptk check failed at %s:%d: %s\n", __FILE__, \
              __LINE__, #condition);                                          \
      return 1;                                                               \
    }                                                                         \
  } while (0)

typedef struct bridge_log {
  size_t events;
  size_t controller_events;
} bridge_log;

typedef struct physical_pads {
  int select[2];
  int start[2];
  int l2[2];
  int r2[2];
  int guide[2];
} physical_pads;

static int accept_event(void *userdata, const nxandroid_android_event *event,
                        nxandroid_android_ack *ack) {
  bridge_log *log = (bridge_log *)userdata;
  log->events++;
  if (event->source != NXANDROID_ANDROID_SOURCE_LIFECYCLE)
    log->controller_events++;
  ack->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  ack->sequence = event->sequence;
  ack->handled = 1;
  ack->return_value = 1;
  return 0;
}

static int read_chord(void *userdata, size_t pad, int control) {
  physical_pads *pads = (physical_pads *)userdata;
  if (pad >= 2u)
    return 0;
  if (control == NXINPUT_GPTK_SELECT)
    return pads->select[pad];
  if (control == NXINPUT_GPTK_START)
    return pads->start[pad];
  return 0;
}

static const char mapping[] =
    "format = NEXTOS_CONTROLLERS/2\n"
    "port = nxandroid-c7-bridge\n"
    "[menu]\n"
    "A = null\nB = null\nX = game.fire\nY = native\n"
    "L1 = null\nR1 = null\nL2 = game.brake\nR2 = game.boost\n"
    "L3 = null\nR3 = null\nSTART = null\nSELECT = null\n"
    "UP = null\nDOWN = null\nLEFT = null\nRIGHT = null\n"
    "LEFT_STICK = null\nRIGHT_STICK = null\n"
    "[gameplay]\n"
    "A = null\nB = null\nX = game.fire\nY = native\n"
    "L1 = null\nR1 = null\nL2 = game.brake\nR2 = game.boost\n"
    "L3 = null\nR3 = null\nSTART = null\nSELECT = null\n"
    "UP = null\nDOWN = null\nLEFT = null\nRIGHT = null\n"
    "LEFT_STICK = null\nRIGHT_STICK = null\n";

int main(void) {
  static const nxandroid_android_route routes[] = {
      {"game.fire", NXANDROID_ANDROID_SINK_KEY_EVENT,
       NXANDROID_ANDROID_SIGNAL_BUTTON, 99, 0, 0.0f, 0.0f},
      {"game.brake", NXANDROID_ANDROID_SINK_MOTION_EVENT,
       NXANDROID_ANDROID_SIGNAL_AXIS, 17, 0, 0.0f, 0.0f},
      {"game.boost", NXANDROID_ANDROID_SINK_MOTION_EVENT,
       NXANDROID_ANDROID_SIGNAL_AXIS, 18, 0, 0.0f, 0.0f},
  };
  static const nxandroid_android_native_route native_routes[] = {
      {NXANDROID_ANDROID_Y, NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD,
       NXANDROID_ANDROID_SIGNAL_BUTTON, 100, 0},
  };
  nxinput_gptk parsed;
  nxandroid_android_authority authority;
  nxandroid_android_context context;
  nxandroid_android_profile profile;
  nxinput_exit_chord chord;
  bridge_log log;
  physical_pads pads;
  char error[256];
  size_t before;
  int poll;

  memset(&log, 0, sizeof(log));
  memset(&pads, 0, sizeof(pads));
  CHECK(nxinput_gptk_parse(mapping, strlen(mapping), &parsed, error,
                           sizeof(error)) == 0);
  CHECK(nxandroid_android_authority_from_gptk(&parsed, &authority, error,
                                              sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(authority.schema_version == 2u);
  CHECK(authority.decision[NXANDROID_ANDROID_MENU][NXANDROID_ANDROID_A] ==
        NXANDROID_ANDROID_DECIDE_SUPPRESS);
  CHECK(authority.decision[NXANDROID_ANDROID_GAMEPLAY][NXANDROID_ANDROID_Y] ==
        NXANDROID_ANDROID_DECIDE_NATIVE);
  CHECK(strcmp(authority.action[NXANDROID_ANDROID_GAMEPLAY]
                               [NXANDROID_ANDROID_L2],
               "game.brake") == 0);

  memset(&profile, 0, sizeof(profile));
  profile.consumer_id = "gptk-v2-real-bridge";
  profile.consumer_version = "1";
  profile.routes = routes;
  profile.route_count = sizeof(routes) / sizeof(routes[0]);
  profile.native_routes = native_routes;
  profile.native_route_count =
      sizeof(native_routes) / sizeof(native_routes[0]);
  profile.event = accept_event;
  profile.userdata = &log;
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_pad_connect(
            &context, 1, 101, "pad-a",
            NXANDROID_ANDROID_SOURCE_GET_CONTROLS, 1u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_pad_connect(
            &context, 2, 102, "pad-b",
            NXANDROID_ANDROID_SOURCE_CFW_GUID_DB, 2u) ==
        NXANDROID_ANDROID_OK);
  before = log.controller_events;
  CHECK(nxandroid_android_button(&context, 1, NXANDROID_ANDROID_SELECT, 1,
                                 3u) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 1, NXANDROID_ANDROID_START, 1,
                                 4u) == NXANDROID_ANDROID_OK);
  CHECK(log.controller_events == before);

  nxinput_exit_chord_init(&chord, 3u);
  pads.select[0] = 1;
  pads.start[0] = 1;
  for (poll = 0; poll < 2; ++poll)
    CHECK(nxinput_exit_chord_poll(&chord, read_chord, &pads, 2u) == 0);
  CHECK(nxinput_exit_chord_poll(&chord, read_chord, &pads, 2u) == 1);
  CHECK(nxinput_exit_chord_consume(&chord) == 1);
  CHECK(log.controller_events == before);

  memset(&pads, 0, sizeof(pads));
  CHECK(nxinput_exit_chord_poll(&chord, read_chord, &pads, 2u) == 0);
  pads.l2[0] = 1;
  pads.r2[0] = 1;
  for (poll = 0; poll < 4; ++poll)
    CHECK(nxinput_exit_chord_poll(&chord, read_chord, &pads, 2u) == 0);
  CHECK(nxandroid_android_axis(&context, 1, NXANDROID_ANDROID_L2, 1.0f, 5u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_axis(&context, 1, NXANDROID_ANDROID_R2, 1.0f, 6u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.controller_events == before + 2u);

  memset(&pads, 0, sizeof(pads));
  pads.guide[0] = 1;
  pads.start[0] = 1;
  for (poll = 0; poll < 4; ++poll)
    CHECK(nxinput_exit_chord_poll(&chord, read_chord, &pads, 2u) == 0);

  memset(&pads, 0, sizeof(pads));
  pads.select[0] = 1;
  pads.start[1] = 1;
  for (poll = 0; poll < 4; ++poll)
    CHECK(nxinput_exit_chord_poll(&chord, read_chord, &pads, 2u) == 0);

  parsed.schema_version = NXINPUT_GPTK_SCHEMA_V1;
  CHECK(nxandroid_android_authority_from_gptk(&parsed, &authority, error,
                                              sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);
  CHECK(authority.api_version == 0u);
  printf("nxandroid_android_gptk_bridge=PASS checks=%u\n", checks);
  printf("authority_reads=54 chord_same_pad=PASS l2_r2=NEGATIVE ");
  printf("guide_start=NEGATIVE cross_pad=NEGATIVE game_chord_events=0\n");
  return 0;
}
