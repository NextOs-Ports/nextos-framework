/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_input_sinks.h"

#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
      return -1;                                                               \
    }                                                                          \
  } while (0)

typedef struct sink_record {
  char action[NXANDROID_INPUT_SINK_ACTION_MAX];
  int pressed;
  float value;
} sink_record;

typedef struct sink_log {
  sink_record records[16];
  size_t count;
} sink_log;

static void recording_sink(void *userdata, const char *action, int pressed,
                           float value) {
  sink_log *log = (sink_log *)userdata;

  if (log->count < ARRAY_SIZE(log->records)) {
    snprintf(log->records[log->count].action,
             sizeof(log->records[log->count].action), "%s", action);
    log->records[log->count].pressed = pressed;
    log->records[log->count].value = value;
  }
  log->count += 1u;
}

/* Distinct function so duplicate detection can be probed by callback. */
static void secondary_sink(void *userdata, const char *action, int pressed,
                           float value) {
  recording_sink(userdata, action, pressed, value);
}

static void counting_sink(void *userdata, const char *action, int pressed,
                          float value) {
  (void)action;
  (void)pressed;
  (void)value;
  *(int *)userdata += 1;
}

static int test_multi_sink_delivery(void) {
  nxandroid_input_sink_registry registry;
  sink_log key_log;
  sink_log internal_log;

  memset(&key_log, 0, sizeof(key_log));
  memset(&internal_log, 0, sizeof(internal_log));
  nxandroid_input_sinks_init(&registry);

  /* Action Squad regression: one logical action, two backends. */
  CHECK(nxandroid_input_sinks_register(
            &registry, "ui.confirm", NXANDROID_SINK_ANDROID_KEY,
            recording_sink, &key_log,
            "fake KeyEvent sink") == NXANDROID_INPUT_OK);
  CHECK(nxandroid_input_sinks_register(
            &registry, "ui.confirm", NXANDROID_SINK_INTERNAL_API,
            secondary_sink, &internal_log,
            "fake engine-internal sink") == NXANDROID_INPUT_OK);

  CHECK(nxandroid_input_sinks_deliver(&registry, "ui.confirm", 1, 1.0f) == 2);
  CHECK(key_log.count == 1u);
  CHECK(internal_log.count == 1u);
  CHECK(key_log.records[0].pressed == 1);
  CHECK(internal_log.records[0].pressed == 1);
  CHECK(strcmp(key_log.records[0].action, "ui.confirm") == 0);
  CHECK(strcmp(internal_log.records[0].action, "ui.confirm") == 0);
  CHECK(key_log.records[0].value == 1.0f);

  CHECK(nxandroid_input_sinks_deliver(&registry, "ui.confirm", 0, 0.0f) == 2);
  CHECK(key_log.count == 2u);
  CHECK(internal_log.count == 2u);
  CHECK(key_log.records[1].pressed == 0);
  CHECK(internal_log.records[1].pressed == 0);
  CHECK(key_log.records[1].value == 0.0f);

  /* Unmapped action: zero invocations and untouched logs. */
  CHECK(nxandroid_input_sinks_deliver(&registry, "ui.cancel", 1, 1.0f) == 0);
  CHECK(key_log.count == 2u);
  CHECK(internal_log.count == 2u);

  /* Invalid delivery arguments are negative, never "0 sinks". */
  CHECK(nxandroid_input_sinks_deliver(NULL, "ui.confirm", 1, 1.0f) ==
        NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_deliver(&registry, NULL, 1, 1.0f) ==
        NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_deliver(&registry, "", 1, 1.0f) ==
        NXANDROID_INPUT_EINVAL);
  return 0;
}

static int test_registration_limits(void) {
  nxandroid_input_sink_registry registry;
  sink_log log;
  int counter = 0;
  char action[NXANDROID_INPUT_SINK_ACTION_MAX + 8];
  size_t index;

  memset(&log, 0, sizeof(log));
  nxandroid_input_sinks_init(&registry);

  /* Invalid arguments. */
  CHECK(nxandroid_input_sinks_register(NULL, "a", NXANDROID_SINK_ANDROID_KEY,
                                       recording_sink, &log,
                                       NULL) == NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_register(&registry, NULL,
                                       NXANDROID_SINK_ANDROID_KEY,
                                       recording_sink, &log,
                                       NULL) == NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_register(&registry, "",
                                       NXANDROID_SINK_ANDROID_KEY,
                                       recording_sink, &log,
                                       NULL) == NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_register(&registry, "a",
                                       NXANDROID_SINK_ANDROID_KEY, NULL, &log,
                                       NULL) == NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_register(&registry, "a",
                                       (nxandroid_input_sink_kind)0,
                                       recording_sink, &log,
                                       NULL) == NXANDROID_INPUT_EINVAL);
  memset(action, 'x', sizeof(action) - 1u);
  action[sizeof(action) - 1u] = '\0';
  CHECK(nxandroid_input_sinks_register(&registry, action,
                                       NXANDROID_SINK_ANDROID_KEY,
                                       recording_sink, &log,
                                       NULL) == NXANDROID_INPUT_EINVAL);

  /* Duplicate = same action+kind+callback; userdata does not matter, while a
   * different kind or callback for the same action is a legal second sink. */
  CHECK(nxandroid_input_sinks_register(&registry, "ui.confirm",
                                       NXANDROID_SINK_ANDROID_KEY,
                                       recording_sink, &log,
                                       "first") == NXANDROID_INPUT_OK);
  CHECK(nxandroid_input_sinks_register(&registry, "ui.confirm",
                                       NXANDROID_SINK_ANDROID_KEY,
                                       recording_sink, &counter,
                                       "dup") == NXANDROID_INPUT_EDUPLICATE);
  CHECK(nxandroid_input_sinks_register(&registry, "ui.confirm",
                                       NXANDROID_SINK_INTERNAL_API,
                                       recording_sink, &log,
                                       "other kind") == NXANDROID_INPUT_OK);
  CHECK(nxandroid_input_sinks_register(&registry, "ui.confirm",
                                       NXANDROID_SINK_ANDROID_KEY,
                                       secondary_sink, &log,
                                       "other fn") == NXANDROID_INPUT_OK);

  /* Fill to capacity, then expect EFULL. */
  for (index = 3u; index < NXANDROID_INPUT_SINK_MAX_ENTRIES; ++index) {
    snprintf(action, sizeof(action), "action.%02u", (unsigned)index);
    CHECK(nxandroid_input_sinks_register(&registry, action,
                                         NXANDROID_SINK_JNI_CALLBACK,
                                         counting_sink, &counter,
                                         NULL) == NXANDROID_INPUT_OK);
  }
  CHECK(registry.entry_count == NXANDROID_INPUT_SINK_MAX_ENTRIES);
  CHECK(nxandroid_input_sinks_register(&registry, "overflow",
                                       NXANDROID_SINK_ANDROID_MOTION,
                                       counting_sink, &counter,
                                       NULL) == NXANDROID_INPUT_EFULL);
  /* A duplicate is still reported as duplicate, not as full. */
  CHECK(nxandroid_input_sinks_register(&registry, "action.03",
                                       NXANDROID_SINK_JNI_CALLBACK,
                                       counting_sink, &counter,
                                       NULL) == NXANDROID_INPUT_EDUPLICATE);
  /* The full registry still delivers. */
  CHECK(nxandroid_input_sinks_deliver(&registry, "action.03", 1, 1.0f) == 1);
  CHECK(counter == 1);
  return 0;
}

static int check_touch_center(const nxandroid_touch_geometry *geometry) {
  int px = -1;
  int py = -1;
  int expected_x;
  int expected_y;
  char error[128];

  error[0] = '\0';
  CHECK(nxandroid_touch_resolve(geometry, 0.5f, 0.5f, &px, &py, error,
                                sizeof(error)) == NXANDROID_INPUT_OK);
  expected_x = geometry->safe_left +
               (geometry->width - geometry->safe_left - geometry->safe_right) /
                   2;
  expected_y = geometry->safe_top +
               (geometry->height - geometry->safe_top - geometry->safe_bottom) /
                   2;
  CHECK(px == expected_x);
  CHECK(py == expected_y);
  return 0;
}

static int check_touch_corners(const nxandroid_touch_geometry *geometry) {
  static const float corners[4][2] = {
      {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
  size_t index;

  for (index = 0u; index < ARRAY_SIZE(corners); ++index) {
    int px = -1;
    int py = -1;

    CHECK(nxandroid_touch_resolve(geometry, corners[index][0],
                                  corners[index][1], &px, &py, NULL,
                                  0u) == NXANDROID_INPUT_OK);
    CHECK(px >= geometry->safe_left);
    CHECK(px <= geometry->width - geometry->safe_right - 1);
    CHECK(py >= geometry->safe_top);
    CHECK(py <= geometry->height - geometry->safe_bottom - 1);
  }
  return 0;
}

static int test_touch_resolve(void) {
  static const int sizes[3][2] = {{640, 480}, {720, 720}, {1280, 720}};
  static const int rotations[4] = {0, 90, 180, 270};
  static const int insets[2][4] = {
      {0, 0, 0, 0}, {32, 20, 16, 8} /* left, top, right, bottom */};
  size_t size_index;
  size_t rotation_index;
  size_t inset_index;

  for (size_index = 0u; size_index < ARRAY_SIZE(sizes); ++size_index) {
    for (rotation_index = 0u; rotation_index < ARRAY_SIZE(rotations);
         ++rotation_index) {
      for (inset_index = 0u; inset_index < ARRAY_SIZE(insets); ++inset_index) {
        nxandroid_touch_geometry geometry;

        geometry.width = sizes[size_index][0];
        geometry.height = sizes[size_index][1];
        geometry.rotation_degrees = rotations[rotation_index];
        geometry.safe_left = insets[inset_index][0];
        geometry.safe_top = insets[inset_index][1];
        geometry.safe_right = insets[inset_index][2];
        geometry.safe_bottom = insets[inset_index][3];
        if (check_touch_center(&geometry) != 0)
          return -1;
        if (check_touch_corners(&geometry) != 0)
          return -1;
      }
    }
  }

  /* Landscape 90-degree spot check: guest top-right lands at drawable
   * top-left corner region after rotation. */
  {
    nxandroid_touch_geometry geometry = {1280, 720, 90, 0, 0, 0, 0};
    int px = -1;
    int py = -1;

    CHECK(nxandroid_touch_resolve(&geometry, 1.0f, 0.0f, &px, &py, NULL,
                                  0u) == NXANDROID_INPUT_OK);
    CHECK(px == 1279);
    CHECK(py == 719);
    CHECK(nxandroid_touch_resolve(&geometry, 0.0f, 0.0f, &px, &py, NULL,
                                  0u) == NXANDROID_INPUT_OK);
    CHECK(px == 1279);
    CHECK(py == 0);
  }
  return 0;
}

static int test_touch_resolve_fail_closed(void) {
  nxandroid_touch_geometry good = {640, 480, 0, 0, 0, 0, 0};
  nxandroid_touch_geometry bad;
  char error[128];
  int px = 111;
  int py = 222;
  size_t index;

  static const nxandroid_touch_geometry invalid[] = {
      {0, 480, 0, 0, 0, 0, 0},      /* zero width */
      {640, 0, 0, 0, 0, 0, 0},      /* zero height */
      {-640, 480, 0, 0, 0, 0, 0},   /* negative width */
      {640, -480, 0, 0, 0, 0, 0},   /* negative height */
      {640, 480, 45, 0, 0, 0, 0},   /* bad rotation */
      {640, 480, 360, 0, 0, 0, 0},  /* bad rotation */
      {640, 480, -90, 0, 0, 0, 0},  /* bad rotation */
      {640, 480, 0, -1, 0, 0, 0},   /* negative inset */
      {640, 480, 0, 320, 0, 320, 0},   /* safe area eats the width */
      {640, 480, 0, 0, 240, 0, 240},   /* safe area eats the height */
      {640, 480, 0, 700, 0, 0, 0},     /* inset beyond the drawable */
  };

  for (index = 0u; index < ARRAY_SIZE(invalid); ++index) {
    bad = invalid[index];
    error[0] = '\0';
    px = 111;
    py = 222;
    CHECK(nxandroid_touch_resolve(&bad, 0.5f, 0.5f, &px, &py, error,
                                  sizeof(error)) == NXANDROID_INPUT_EINVAL);
    CHECK(error[0] != '\0');
    CHECK(px == 111 && py == 222); /* outputs untouched on failure */
  }

  CHECK(nxandroid_touch_resolve(NULL, 0.5f, 0.5f, &px, &py, error,
                                sizeof(error)) == NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_touch_resolve(&good, 0.5f, 0.5f, NULL, &py, error,
                                sizeof(error)) == NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_touch_resolve(&good, 0.5f, 0.5f, &px, NULL, error,
                                sizeof(error)) == NXANDROID_INPUT_EINVAL);
  /* NULL/zero error buffer must be tolerated. */
  bad = invalid[0];
  CHECK(nxandroid_touch_resolve(&bad, 0.5f, 0.5f, &px, &py, NULL, 0u) ==
        NXANDROID_INPUT_EINVAL);
  /* Out-of-range normalized input clamps instead of failing. */
  CHECK(nxandroid_touch_resolve(&good, -0.5f, 2.0f, &px, &py, NULL, 0u) ==
        NXANDROID_INPUT_OK);
  CHECK(px == 0 && py == 479);
  return 0;
}

static int test_exclusive_flag(void) {
  nxandroid_input_sink_registry registry;

  nxandroid_input_sinks_init(&registry);
  CHECK(nxandroid_input_sinks_exclusive(&registry) == 0);
  CHECK(nxandroid_input_sinks_set_exclusive(&registry, 7) ==
        NXANDROID_INPUT_OK);
  CHECK(nxandroid_input_sinks_exclusive(&registry) == 1);
  CHECK(nxandroid_input_sinks_set_exclusive(&registry, 0) ==
        NXANDROID_INPUT_OK);
  CHECK(nxandroid_input_sinks_exclusive(&registry) == 0);
  CHECK(nxandroid_input_sinks_set_exclusive(NULL, 1) ==
        NXANDROID_INPUT_EINVAL);
  CHECK(nxandroid_input_sinks_exclusive(NULL) == 0);
  return 0;
}

static int test_result_strings(void) {
  CHECK(strcmp(nxandroid_input_result_string(NXANDROID_INPUT_OK), "ok") == 0);
  CHECK(nxandroid_input_result_string(NXANDROID_INPUT_EINVAL) != NULL);
  CHECK(nxandroid_input_result_string(NXANDROID_INPUT_EFULL) != NULL);
  CHECK(nxandroid_input_result_string(NXANDROID_INPUT_EDUPLICATE) != NULL);
  CHECK(nxandroid_input_result_string((nxandroid_input_result)-99) != NULL);
  return 0;
}

int main(void) {
  int failed = 0;

  failed |= test_multi_sink_delivery() != 0;
  failed |= test_registration_limits() != 0;
  failed |= test_touch_resolve() != 0;
  failed |= test_touch_resolve_fail_closed() != 0;
  failed |= test_exclusive_flag() != 0;
  failed |= test_result_strings() != 0;

  if (failed) {
    fprintf(stderr, "nxandroid_input_sink_tests=FAIL\n");
    return 1;
  }
  printf("nxandroid_input_sink_tests=PASS\n");
  printf("guest_code_executed=0 device_access=0 network_access=0 "
         "signals_used=0 jni_calls=0\n");
  return 0;
}
