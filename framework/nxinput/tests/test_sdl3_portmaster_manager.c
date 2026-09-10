/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <linux/input.h>

#include "nxinput_sdl3_portmaster.h"

#define TEST_BITS_PER_LONG (8u * sizeof(unsigned long))
#define TEST_KEY_WORDS \
  ((KEY_MAX + TEST_BITS_PER_LONG - 1u) / TEST_BITS_PER_LONG)
#define TEST_DEVICE_MAX 8u
#define TEST_TRACE_MAX 128u

enum {
  TEST_MAPPING_PRIORITY_NONE = 0,
  TEST_MAPPING_PRIORITY_API = 1,
  TEST_MAPPING_PRIORITY_USER = 2,
};

static const char fixture_mapping[] =
    "19000000010000000100000000010000,Deeplay-keys,"
    "a:b4,b:b3,x:b5,y:b6,leftshoulder:b7,rightshoulder:b8,"
    "lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,"
    "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
    "volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,"
    "rightx:a2,righty:a3,rightstick:b15,platform:Linux,";

/* Same semantics already expressed in this fixture backend's SDL3 ordering.
 * Staging keeps it out of SDL_Init, so it must be installed once unchanged. */
static const char fixture_native_mapping[] =
    "19004ca6010000000100000000010000,Native-order fixture,"
    "a:b1,b:b0,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
    "lefttrigger:b10,righttrigger:b11,guide:b8,start:b7,back:b6,"
    "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
    "volumedown:b14,volumeup:b15,leftx:a0,lefty:a1,leftstick:b9,"
    "rightx:a2,righty:a3,rightstick:b12,platform:Linux,";

typedef struct test_device {
  SDL_JoystickID instance_id;
  unsigned char guid_tag;
  unsigned int extra_lower_key;
  unsigned int path_failures;
  unsigned int open_failures;
  unsigned int fstat_failures;
  unsigned int ioctl_failures;
  int effective_mapping_priority;
  char effective_mapping[NXINPUT_SDL3_PM_MAPPING_MAX];
} test_device;

typedef enum test_event_type {
  TEST_EVENT_ENUMERATE = 1,
  TEST_EVENT_SET_MAPPING,
  TEST_EVENT_CLASSIFY,
  TEST_EVENT_OPEN,
} test_event_type;

typedef struct test_event {
  test_event_type type;
  SDL_JoystickID instance_id;
} test_event;

static test_device test_devices[TEST_DEVICE_MAX];
static size_t test_device_count;
static SDL_JoystickID enumeration[TEST_DEVICE_MAX];
static size_t enumeration_count;
static test_event trace_events[TEST_TRACE_MAX];
static size_t trace_count;
static unsigned int set_mapping_calls;
static unsigned int set_mapping_failures;
static unsigned int classify_calls[TEST_DEVICE_MAX];
static unsigned int open_calls[TEST_DEVICE_MAX];
static SDL_InitFlags initialized_subsystems;

static void trace(test_event_type type, SDL_JoystickID instance_id) {
  assert(trace_count < TEST_TRACE_MAX);
  trace_events[trace_count].type = type;
  trace_events[trace_count].instance_id = instance_id;
  ++trace_count;
}

static test_device *find_device(SDL_JoystickID instance_id) {
  size_t index;
  for (index = 0; index < test_device_count; ++index) {
    if (test_devices[index].instance_id == instance_id)
      return &test_devices[index];
  }
  return NULL;
}

static size_t device_index(SDL_JoystickID instance_id) {
  size_t index;
  for (index = 0; index < test_device_count; ++index) {
    if (test_devices[index].instance_id == instance_id)
      return index;
  }
  assert(!"unknown fixture instance");
  return 0u;
}

static void add_device(SDL_JoystickID instance_id, unsigned char guid_tag,
                       unsigned int extra_lower_key) {
  test_device *device;
  assert(test_device_count < TEST_DEVICE_MAX);
  device = &test_devices[test_device_count++];
  memset(device, 0, sizeof *device);
  device->instance_id = instance_id;
  device->guid_tag = guid_tag;
  device->extra_lower_key = extra_lower_key;
}

static void seed_effective_mapping(SDL_JoystickID instance_id,
                                   const char *mapping, int priority) {
  test_device *device = find_device(instance_id);
  assert(device != NULL && mapping != NULL);
  assert(strlen(mapping) < sizeof device->effective_mapping);
  memcpy(device->effective_mapping, mapping, strlen(mapping) + 1u);
  device->effective_mapping_priority = priority;
}

static void set_enumeration(const SDL_JoystickID *ids, size_t count) {
  assert(count <= TEST_DEVICE_MAX);
  memcpy(enumeration, ids, count * sizeof *ids);
  enumeration_count = count;
}

static void reset_mocks(void) {
  assert(unsetenv("SDL_GAMECONTROLLERCONFIG") == 0);
  memset(test_devices, 0, sizeof test_devices);
  memset(enumeration, 0, sizeof enumeration);
  memset(trace_events, 0, sizeof trace_events);
  memset(classify_calls, 0, sizeof classify_calls);
  memset(open_calls, 0, sizeof open_calls);
  test_device_count = 0u;
  enumeration_count = 0u;
  trace_count = 0u;
  set_mapping_calls = 0u;
  set_mapping_failures = 0u;
  initialized_subsystems = 0;
}

static void set_key(unsigned long *bits, unsigned int code) {
  bits[code / TEST_BITS_PER_LONG] |=
      1ul << (code % TEST_BITS_PER_LONG);
}

static void populate_keys(const test_device *device, unsigned long *bits) {
  unsigned int code;
  set_key(bits, KEY_ESC);
  set_key(bits, KEY_VOLUMEDOWN);
  set_key(bits, KEY_VOLUMEUP);
  if (device->extra_lower_key != 0u)
    set_key(bits, device->extra_lower_key);
  /* Match the proved BB1 capability capture exactly: 0x130..0x13c. Keeping
   * synthetic BTN_THUMBL/BTN_THUMBR bits here would make the native-order
   * b14/b15 fixture a no-op for the wrong reason. */
  for (code = BTN_GAMEPAD; code <= BTN_MODE; ++code)
    set_key(bits, code);
}

/* Syscall seams. The production module still calls open/fstat/ioctl/close;
 * GNU ld --wrap keeps this host regression hermetic and device-free. */
int __wrap_open(const char *path, int flags, ...) {
  unsigned int parsed = 0u;
  test_device *device;
  (void)flags;
  if (path == NULL ||
      sscanf(path, "/dev/input/nxinput-fixture-%u", &parsed) != 1 ||
      (device = find_device((SDL_JoystickID)parsed)) == NULL) {
    errno = ENOENT;
    return -1;
  }
  if (device->open_failures != 0u) {
    --device->open_failures;
    errno = EACCES;
    return -1;
  }
  return 1000 + (int)parsed;
}

int __wrap_fstat(int fd, struct stat *metadata) {
  SDL_JoystickID instance_id = (SDL_JoystickID)(fd - 1000);
  test_device *device = find_device(instance_id);
  if (metadata == NULL || device == NULL) {
    errno = EBADF;
    return -1;
  }
  if (device->fstat_failures != 0u) {
    --device->fstat_failures;
    errno = EIO;
    return -1;
  }
  memset(metadata, 0, sizeof *metadata);
  metadata->st_mode = S_IFCHR | 0600;
  return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
  SDL_JoystickID instance_id = (SDL_JoystickID)(fd - 1000);
  test_device *device = find_device(instance_id);
  unsigned long *bits;
  va_list arguments;
  size_t bytes = _IOC_SIZE(request);
  if (device == NULL) {
    errno = EBADF;
    return -1;
  }
  if (device->ioctl_failures != 0u) {
    --device->ioctl_failures;
    errno = EAGAIN;
    return -1;
  }
  va_start(arguments, request);
  bits = va_arg(arguments, unsigned long *);
  va_end(arguments);
  assert(bits != NULL);
  assert(bytes >= TEST_KEY_WORDS * sizeof(unsigned long));
  memset(bits, 0, bytes);
  populate_keys(device, bits);
  return 0;
}

int __wrap_close(int fd) {
  assert(find_device((SDL_JoystickID)(fd - 1000)) != NULL);
  return 0;
}

/* SDL3 seams. */
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) {
  return flags == 0 ? initialized_subsystems
                    : initialized_subsystems & flags;
}

SDL_JoystickID *SDL_GetJoysticks(int *count) {
  SDL_JoystickID *ids =
      malloc((enumeration_count + 1u) * sizeof *ids);
  assert(ids != NULL);
  memcpy(ids, enumeration, enumeration_count * sizeof *ids);
  ids[enumeration_count] = 0;
  if (count != NULL) *count = (int)enumeration_count;
  trace(TEST_EVENT_ENUMERATE, 0);
  return ids;
}

const char *SDL_GetJoystickPathForID(SDL_JoystickID instance_id) {
  static char path[64];
  test_device *device = find_device(instance_id);
  if (device == NULL) return NULL;
  if (device->path_failures != 0u) {
    --device->path_failures;
    return NULL;
  }
  snprintf(path, sizeof path, "/dev/input/nxinput-fixture-%u",
           (unsigned int)instance_id);
  return path;
}

SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id) {
  SDL_GUID guid = {{0}};
  test_device *device = find_device(instance_id);
  if (device != NULL) guid.data[0] = device->guid_tag;
  return guid;
}

void SDL_GUIDToString(SDL_GUID guid, char *text, int text_size) {
  char formatted[33];
  int written = snprintf(formatted, sizeof formatted,
                         "%02x004ca6010000000100000000010000",
                         (unsigned int)guid.data[0]);
  assert(written == 32);
  assert(text != NULL && text_size >= 33);
  memcpy(text, formatted, 33u);
}

bool SDL_SetGamepadMapping(SDL_JoystickID instance_id, const char *mapping) {
  test_device *device = find_device(instance_id);
  assert(device != NULL && mapping != NULL);
  trace(TEST_EVENT_SET_MAPPING, instance_id);
  ++set_mapping_calls;
  if (set_mapping_failures != 0u) {
    --set_mapping_failures;
    errno = EAGAIN;
    return false;
  }
  /* Reproduce SDL3's real priority rule. Critically, the public API reports
   * success even when a pre-existing USER mapping refuses this API update. */
  if (device->effective_mapping_priority <= TEST_MAPPING_PRIORITY_API) {
    assert(strlen(mapping) < sizeof device->effective_mapping);
    memcpy(device->effective_mapping, mapping, strlen(mapping) + 1u);
    device->effective_mapping_priority = TEST_MAPPING_PRIORITY_API;
  }
  return true;
}

char *SDL_GetGamepadMappingForID(SDL_JoystickID instance_id) {
  test_device *device = find_device(instance_id);
  if (device == NULL || device->effective_mapping[0] == '\0') {
    return NULL;
  }
  return strdup(device->effective_mapping);
}

void SDL_free(void *memory) {
  free(memory);
}

bool SDL_IsGamepad(SDL_JoystickID instance_id) {
  size_t index = device_index(instance_id);
  ++classify_calls[index];
  trace(TEST_EVENT_CLASSIFY, instance_id);
  return true;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id) {
  size_t index = device_index(instance_id);
  ++open_calls[index];
  trace(TEST_EVENT_OPEN, instance_id);
  return (SDL_Gamepad *)(uintptr_t)(0x10000u + (uint32_t)instance_id);
}

static nxinput_sdl3_pm_context *stage_context(const char *mapping) {
  nxinput_sdl3_pm_context *context;

  assert(initialized_subsystems == 0);
  if (mapping != NULL) {
    assert(setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);
  } else {
    assert(unsetenv("SDL_GAMECONTROLLERCONFIG") == 0);
  }
  context = nxinput_sdl3_pm_stage_before_sdl_init();
  assert(context != NULL);
  assert(getenv("SDL_GAMECONTROLLERCONFIG") == NULL);
  initialized_subsystems = SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD;
  return context;
}

static void test_preinit_staging_is_mandatory_and_fail_closed(void) {
  nxinput_sdl3_pm_context *context;
  char oversized[NXINPUT_SDL3_PM_MAPPING_MAX + 1u];

  reset_mocks();
  assert(setenv("SDL_GAMECONTROLLERCONFIG", fixture_mapping, 1) == 0);
  initialized_subsystems = SDL_INIT_JOYSTICK;
  errno = 0;
  context = nxinput_sdl3_pm_stage_before_sdl_init();
  assert(context == NULL);
  assert(errno == EBUSY);
  assert(strcmp(getenv("SDL_GAMECONTROLLERCONFIG"), fixture_mapping) == 0);

  reset_mocks();
  memset(oversized, 'x', sizeof oversized - 1u);
  oversized[sizeof oversized - 1u] = '\0';
  assert(setenv("SDL_GAMECONTROLLERCONFIG", oversized, 1) == 0);
  errno = 0;
  context = nxinput_sdl3_pm_stage_before_sdl_init();
  assert(context == NULL);
  assert(errno == E2BIG);
  assert(strcmp(getenv("SDL_GAMECONTROLLERCONFIG"), oversized) == 0);

  reset_mocks();
  context = stage_context(fixture_mapping);
  nxinput_sdl3_pm_destroy(context);
}

static void test_native_order_and_consumer_receipt(void) {
  const SDL_JoystickID ids_in[] = {101};
  SDL_JoystickID *ids;
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;
  int count = -1;

  reset_mocks();
  add_device(101, 0x19, 0u);
  set_enumeration(ids_in, 1u);
  context = stage_context(fixture_mapping);

  ids = nxinput_sdl3_pm_get_joysticks(context, &count);
  assert(ids != NULL && count == 1 && ids[0] == 101);
  free(ids);
  assert(trace_count == 2u);
  assert(trace_events[0].type == TEST_EVENT_ENUMERATE);
  assert(trace_events[1].type == TEST_EVENT_SET_MAPPING);
  assert(trace_events[1].instance_id == 101);

  /* Mapping proof alone is not permission to claim gameplay delivery. */
  assert(nxinput_sdl3_pm_record_consumer_delivery(
             context, 101, 0u, true, &receipt) == NXINPUT_SDL3_PM_ERROR);
  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) == 0);
  assert(receipt.effective_mapping_verified == 1u);
  assert(receipt.consumer_delivery_count == 0u);

  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(nxinput_sdl3_pm_open_gamepad(context, 101) != NULL);
  assert(trace_count == 4u);
  assert(trace_events[2].type == TEST_EVENT_CLASSIFY);
  assert(trace_events[3].type == TEST_EVENT_OPEN);
  assert(set_mapping_calls == 1u); /* classification/open reused proof */

  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) == 0);
  assert(receipt.classification_observed == 1u);
  assert(receipt.classification_result == 1u);
  assert(receipt.open_observed == 1u);
  assert(receipt.open_result == 1u);
  assert(receipt.consumer_delivery_count == 0u);
  assert(receipt.consumer_pressed_mask == 0u);
  assert(receipt.consumer_released_mask == 0u);

  /* Merely enumerating, classifying and opening is not gameplay evidence.
   * Only this adapter-owned call confirms that the guest getter/callback
   * actually received the value. */
  assert(nxinput_sdl3_pm_record_consumer_delivery(
             context, 101, 0u, true, &receipt) >= 0);
  assert(receipt.consumer_delivery_count == 1u);
  assert(receipt.consumer_pressed_mask == 1u);
  assert(receipt.consumer_released_mask == 0u);
  assert(nxinput_sdl3_pm_record_consumer_delivery(
             context, 101, 0u, false, &receipt) >= 0);
  assert(receipt.consumer_delivery_count == 2u);
  assert(receipt.consumer_pressed_mask == 1u);
  assert(receipt.consumer_released_mask == 1u);
  assert(nxinput_sdl3_pm_record_consumer_delivery(
             context, 101, NXINPUT_SDL3_PM_CONSUMER_BUTTON_MAX, true,
             &receipt) == NXINPUT_SDL3_PM_ERROR);
  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) == 0);
  assert(receipt.consumer_delivery_count == 2u);

  nxinput_sdl3_pm_destroy(context);
}

static void test_transient_failure_retries_before_classification(void) {
  const SDL_JoystickID ids_in[] = {101};
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;
  SDL_JoystickID *ids;
  int count = 0;

  reset_mocks();
  add_device(101, 0x19, 0u);
  test_devices[0].ioctl_failures = 2u;
  set_enumeration(ids_in, 1u);
  context = stage_context(fixture_mapping);

  ids = nxinput_sdl3_pm_get_joysticks(context, &count);
  assert(ids != NULL && count == 1);
  free(ids);
  assert(set_mapping_calls == 0u);
  assert(classify_calls[0] == 0u);

  /* The failed ioctl was not frozen as a stable no-op. Classification retries
   * preparation but preserves the BB1 native flow even when that retry is
   * still transient. */
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(set_mapping_calls == 0u);
  assert(classify_calls[0] == 1u);
  assert(trace_count == 2u);
  assert(trace_events[0].type == TEST_EVENT_ENUMERATE);
  assert(trace_events[1].type == TEST_EVENT_CLASSIFY);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_OPEN, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.cache_hit == 0u);
  assert(receipt.effective_mapping_verified == 1u);
  assert(set_mapping_calls == 1u);

  nxinput_sdl3_pm_destroy(context);
}

static void test_path_open_and_fstat_failures_are_retryable(void) {
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;

  reset_mocks();
  add_device(101, 0x19, 0u);
  test_devices[0].path_failures = 1u;
  test_devices[0].open_failures = 1u;
  test_devices[0].fstat_failures = 1u;
  context = stage_context(fixture_mapping);

  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_NO_DEVICE_PATH);
  assert(receipt.error_number == ENOENT);
  assert(receipt.cache_hit == 0u);
  assert(set_mapping_calls == 0u);

  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_CLASSIFICATION, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_DEVICE_OPEN_FAILED);
  assert(receipt.error_number == EACCES);
  assert(receipt.cache_hit == 0u);
  assert(set_mapping_calls == 0u);

  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_OPEN, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_DEVICE_STAT_FAILED);
  assert(receipt.error_number == EIO);
  assert(receipt.cache_hit == 0u);
  assert(set_mapping_calls == 0u);

  /* None of the three probe failures became a stable cache entry. The next
   * boundary measures the same instance again and reaches registration. */
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_REWRITTEN);
  assert(receipt.cache_hit == 0u);
  assert(receipt.effective_mapping_verified == 1u);
  assert(set_mapping_calls == 1u);

  nxinput_sdl3_pm_destroy(context);
}

static void test_native_mapping_is_verified_and_absent_mapping_is_passthrough(void) {
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;

  reset_mocks();
  add_device(101, 0x19, 0u);
  context = stage_context(fixture_native_mapping);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_NOT_APPLICABLE);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_NATIVE_OR_UNSUPPORTED);
  assert(receipt.cache_hit == 0u);
  assert(receipt.effective_mapping_verified == 1u);
  assert(set_mapping_calls == 1u);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_CLASSIFICATION, &receipt) ==
         NXINPUT_SDL3_PM_NOT_APPLICABLE);
  assert(receipt.cache_hit == 1u);
  assert(receipt.effective_mapping_verified == 1u);
  assert(set_mapping_calls == 1u);
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(set_mapping_calls == 1u);
  nxinput_sdl3_pm_destroy(context);

  reset_mocks();
  add_device(101, 0x19, 0u);
  context = stage_context(NULL);
  for (SDL_JoystickID instance_id = 1u; instance_id <= 64u; ++instance_id) {
    assert(nxinput_sdl3_pm_prepare(
               context, instance_id, NXINPUT_SDL3_PM_STAGE_ENUMERATION,
               &receipt) == NXINPUT_SDL3_PM_NOT_APPLICABLE);
    assert(receipt.reason == NXINPUT_SDL3_PM_REASON_NO_MAPPING);
  }
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_NOT_APPLICABLE);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_NO_MAPPING);
  assert(set_mapping_calls == 0u);
  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(set_mapping_calls == 0u);
  nxinput_sdl3_pm_destroy(context);
}

/* V4-CONTROLLERS-02: a real SDL_GAMECONTROLLERCONFIG is a heterogeneous list.
 * The entry that belongs to THIS device must be selected by GUID, wherever it
 * sits; a list with no entry for this device must stay a passthrough instead
 * of translating a foreign pad's ordinals onto it. */
static void test_heterogeneous_config_selects_by_guid(void) {
  static const char kForeign[] =
      "030000005e0400008e02000010010000,Xbox 360 Controller,"
      "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
      "lefttrigger:b6,righttrigger:b7,guide:b8,start:b9,back:b10,"
      "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
      "volumedown:b11,volumeup:b12,leftx:a0,lefty:a1,leftstick:b13,"
      "rightx:a2,righty:a3,rightstick:b14,platform:Linux,";
  static const char kLegacyForThisDevice[] =
      "19004ca6010000000100000000010000,Deeplay-keys,"
      "a:b4,b:b3,x:b5,y:b6,leftshoulder:b7,rightshoulder:b8,"
      "lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,"
      "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
      "volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,"
      "rightx:a2,righty:a3,rightstick:b15,platform:Linux,";
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;
  char list[4096];

  /* The entry for this device is LAST, behind a comment and a blank line. */
  reset_mocks();
  add_device(101, 0x19, 0u);
  snprintf(list, sizeof list, "# CFW list\n%s\n\n%s\n", kForeign,
           kLegacyForThisDevice);
  context = stage_context(list);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_REWRITTEN);
  assert(receipt.staged_entry_count == 2u);
  assert(receipt.effective_mapping_verified == 1u);
  assert(set_mapping_calls == 1u);
  nxinput_sdl3_pm_destroy(context);

  /* A list that knows nothing about this device is a passthrough: the guest
   * still classifies and opens, and no foreign ordinals are installed. */
  reset_mocks();
  add_device(101, 0x19, 0u);
  snprintf(list, sizeof list, "%s\n%s\n", kForeign, kForeign);
  context = stage_context(list);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_NOT_APPLICABLE);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_NO_MAPPING);
  assert(receipt.staged_entry_count == 2u);
  assert(set_mapping_calls == 0u);
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(nxinput_sdl3_pm_open_gamepad(context, 101) != NULL);
  assert(set_mapping_calls == 0u);
  nxinput_sdl3_pm_destroy(context);

  /* Two divergent entries for the same GUID: order must never decide. */
  reset_mocks();
  add_device(101, 0x19, 0u);
  {
    char divergent[NXINPUT_SDL3_PM_MAPPING_MAX];
    snprintf(divergent, sizeof divergent, "%s", kLegacyForThisDevice);
    divergent[strlen(divergent) - 2u] = 'X';
    snprintf(list, sizeof list, "%s\n%s\n", kLegacyForThisDevice, divergent);
  }
  context = stage_context(list);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_GUID_COLLISION);
  assert(set_mapping_calls == 0u);
  /* The guest is never blocked by our refusal. */
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  nxinput_sdl3_pm_destroy(context);
}

static void test_hotplug_and_multiple_instances(void) {
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;
  int result;

  reset_mocks();
  add_device(101, 0x19, 0u);
  context = stage_context(fixture_mapping);
  result = nxinput_sdl3_pm_prepare(
      context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt);
  assert(result == NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.cache_hit == 0u);
  assert(set_mapping_calls == 1u);
  result = nxinput_sdl3_pm_prepare(
      context, 101, NXINPUT_SDL3_PM_STAGE_CLASSIFICATION, &receipt);
  assert(result == NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.cache_hit == 1u);
  assert(set_mapping_calls == 1u);
  nxinput_sdl3_pm_remove(context, 101);
  result = nxinput_sdl3_pm_prepare(
      context, 101, NXINPUT_SDL3_PM_STAGE_OPEN, &receipt);
  assert(result == NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.cache_hit == 0u);
  assert(set_mapping_calls == 2u);
  nxinput_sdl3_pm_destroy(context);

  /* Two different GUIDs are measured and cached independently. */
  reset_mocks();
  add_device(101, 0x19, 0u);
  add_device(201, 0x29, 0u);
  context = stage_context(fixture_mapping);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(nxinput_sdl3_pm_prepare(
             context, 201, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(set_mapping_calls == 2u);
  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) == 0);
  assert(nxinput_sdl3_pm_get_receipt(context, 201, &receipt) == 0);
  nxinput_sdl3_pm_destroy(context);
}

static void test_same_guid_divergence_fails_closed(void) {
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;

  reset_mocks();
  add_device(101, 0x19, 0u);
  /* An extra lower EV_KEY changes the SDL3 ordinals while the legacy volume
   * markers still identify the source dialect. Same GUID + divergent result
   * cannot be isolated because SDL stores mappings by GUID. */
  add_device(102, 0x19, KEY_POWER);
  context = stage_context(fixture_mapping);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(nxinput_sdl3_pm_prepare(
             context, 102, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_GUID_COLLISION);
  assert(set_mapping_calls == 1u); /* collision detected before registration */
  assert(nxinput_sdl3_pm_is_gamepad(context, 102));
  assert(classify_calls[1] == 1u); /* native guest flow is never suppressed */

  /* Once the original live owner is removed, the remaining instance can
   * establish the GUID mapping afresh instead of inheriting stale cache. */
  nxinput_sdl3_pm_remove(context, 101);
  assert(nxinput_sdl3_pm_prepare(
             context, 102, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(set_mapping_calls == 2u);
  nxinput_sdl3_pm_destroy(context);
}

static void test_registration_failure_retries_and_preserves_guest(void) {
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;

  reset_mocks();
  add_device(101, 0x19, 0u);
  context = stage_context(fixture_mapping);
  set_mapping_failures = 1u;
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason == NXINPUT_SDL3_PM_REASON_REGISTRATION_FAILED);
  assert(receipt.cache_hit == 0u);
  assert(set_mapping_calls == 1u);
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(classify_calls[0] == 1u);
  assert(set_mapping_calls == 2u); /* registration error was not cached */
  assert(nxinput_sdl3_pm_open_gamepad(context, 101) != NULL);
  assert(open_calls[0] == 1u);
  assert(set_mapping_calls == 2u); /* successful rewrite is now cached */
  nxinput_sdl3_pm_destroy(context);
}

static void test_user_priority_false_success_is_rejected_and_retried(void) {
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;

  reset_mocks();
  add_device(101, 0x19, 0u);
  context = stage_context(fixture_mapping);

  /* Model an integration bug that allowed the legacy environment mapping to
   * enter SDL at USER priority. SDL_SetGamepadMapping reports success, but the
   * API-priority rewrite does not become effective. Readback must catch it. */
  seed_effective_mapping(101, fixture_mapping, TEST_MAPPING_PRIORITY_USER);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);
  assert(receipt.reason ==
         NXINPUT_SDL3_PM_REASON_EFFECTIVE_MAPPING_MISMATCH);
  assert(receipt.error_number == EPROTO);
  assert(receipt.effective_mapping_verified == 0u);
  assert(set_mapping_calls == 1u);
  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) ==
         NXINPUT_SDL3_PM_ERROR);

  /* A mismatch is retryable. Once the stale USER owner is absent, the next
   * native boundary installs, reads back and caches the corrected mapping. */
  test_devices[0].effective_mapping_priority = TEST_MAPPING_PRIORITY_NONE;
  test_devices[0].effective_mapping[0] = '\0';
  assert(nxinput_sdl3_pm_is_gamepad(context, 101));
  assert(classify_calls[0] == 1u);
  assert(set_mapping_calls == 2u);
  assert(nxinput_sdl3_pm_get_receipt(context, 101, &receipt) == 0);
  assert(receipt.effective_mapping_verified == 1u);
  assert(receipt.classification_observed == 1u);
  nxinput_sdl3_pm_destroy(context);
}

static void test_semantic_readback_ignores_name_and_field_order(void) {
  static const char equivalent_mapping[] =
      "19004ca6010000000100000000010000,Rendered by SDL,"
      "platform:Linux,rightstick:b12,righty:a3,rightx:a2,leftstick:b9,"
      "lefty:a1,leftx:a0,volumeup:b15,volumedown:b14,dpdown:h0.4,"
      "dpright:h0.2,dpleft:h0.8,dpup:h0.1,back:b6,start:b7,guide:b8,"
      "righttrigger:b11,lefttrigger:b10,rightshoulder:b5,"
      "leftshoulder:b4,y:b3,x:b2,b:b0,a:b1,crc:1234,";
  nxinput_sdl3_pm_context *context;
  nxinput_sdl3_pm_receipt receipt;

  reset_mocks();
  add_device(101, 0x19, 0u);
  context = stage_context(fixture_mapping);
  seed_effective_mapping(101, equivalent_mapping, TEST_MAPPING_PRIORITY_USER);

  /* The setter is refused by USER priority, but the mapping that is actually
   * active has identical semantic bindings. Metadata, name and field order do
   * not create a false mismatch. */
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_ENUMERATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(set_mapping_calls == 1u);
  assert(receipt.effective_mapping_verified == 1u);
  assert(receipt.cache_hit == 0u);
  assert(nxinput_sdl3_pm_prepare(
             context, 101, NXINPUT_SDL3_PM_STAGE_CLASSIFICATION, &receipt) ==
         NXINPUT_SDL3_PM_REWRITTEN);
  assert(receipt.cache_hit == 1u);
  assert(set_mapping_calls == 1u);
  nxinput_sdl3_pm_destroy(context);
}

int main(void) {
  assert(strlen(fixture_mapping) == 315u);
  test_preinit_staging_is_mandatory_and_fail_closed();
  test_native_order_and_consumer_receipt();
  test_transient_failure_retries_before_classification();
  test_path_open_and_fstat_failures_are_retryable();
  test_native_mapping_is_verified_and_absent_mapping_is_passthrough();
  test_heterogeneous_config_selects_by_guid();
  test_hotplug_and_multiple_instances();
  test_same_guid_divergence_fails_closed();
  test_registration_failure_retries_and_preserves_guest();
  test_user_priority_false_success_is_rejected_and_retried();
  test_semantic_readback_ignores_name_and_field_order();
  reset_mocks();
  puts("nxinput_sdl3_portmaster_manager: PASS");
  return 0;
}
