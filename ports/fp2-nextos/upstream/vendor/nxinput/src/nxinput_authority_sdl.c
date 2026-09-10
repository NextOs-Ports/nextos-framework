/* SPDX-License-Identifier: GPL-3.0-only */
/* The SDL backing of the nxinput_authority runtime seam.
 *
 * This file contains EFFECTS ONLY. It decides nothing: it does not read a
 * device name, a CFW name, a VID/PID or a layout, and it never chooses
 * between two mappings. The single decision lives in nxinput_sovereign,
 * driven by nxinput_authority. It is also the ONLY place in the library that
 * may call SDL_GameControllerAddMapping* (the static gate enforces it). */
#include "nxinput_authority_sdl.h"

#include <stdio.h>
#include <string.h>

static const char *sdl_getenv(void *userdata, const char *name) {
  (void)userdata;
  return SDL_getenv(name);
}

static int sdl_read_text(void *userdata, const char *path, char *out,
                         size_t cap) {
  SDL_RWops *stream;
  Sint64 size;
  size_t read;

  (void)userdata;
  if (path == NULL || out == NULL || cap == 0u) {
    return -1;
  }
  out[0] = '\0';
  stream = SDL_RWFromFile(path, "rb");
  if (stream == NULL) {
    return -1;
  }
  size = SDL_RWsize(stream);
  if (size < 0 || (Uint64)size >= (Uint64)cap) {
    (void)SDL_RWclose(stream);
    return -1;
  }
  read = SDL_RWread(stream, out, 1u, (size_t)size);
  (void)SDL_RWclose(stream);
  if (read != (size_t)size) {
    return -1;
  }
  out[read] = '\0';
  return 0;
}

static int sdl_device_guid(void *userdata, int device_index, char *out,
                           size_t cap) {
  SDL_JoystickGUID guid;
  (void)userdata;
  if (out == NULL || cap < 33u) {
    return -1;
  }
  guid = SDL_JoystickGetDeviceGUID(device_index);
  out[0] = '\0';
  SDL_JoystickGetGUIDString(guid, out, (int)cap);
  return out[0] != '\0' ? 0 : -1;
}

static int sdl_device_caps(void *userdata, int device_index, int *buttons,
                           int *axes, int *hats) {
  SDL_Joystick *joystick;
  (void)userdata;
  if (buttons == NULL || axes == NULL || hats == NULL) {
    return -1;
  }
  joystick = SDL_JoystickOpen(device_index);
  if (joystick == NULL) {
    return -1;
  }
  *buttons = SDL_JoystickNumButtons(joystick);
  *axes = SDL_JoystickNumAxes(joystick);
  *hats = SDL_JoystickNumHats(joystick);
  SDL_JoystickClose(joystick);
  return (*buttons >= 0 && *axes >= 0 && *hats >= 0) ? 0 : -1;
}

static int sdl_apply_mapping(void *userdata, const char *line) {
  (void)userdata;
  if (line == NULL || line[0] == '\0') {
    return -1;
  }
  return SDL_GameControllerAddMapping(line) >= 0 ? 0 : -1;
}

static int sdl_mapping_for_guid(void *userdata, const char *guid, char *out,
                                size_t cap) {
  SDL_JoystickGUID parsed;
  char *effective;

  (void)userdata;
  if (guid == NULL || out == NULL || cap == 0u) {
    return -1;
  }
  out[0] = '\0';
  parsed = SDL_JoystickGetGUIDFromString(guid);
  effective = SDL_GameControllerMappingForGUID(parsed);
  if (effective == NULL) {
    return -1;
  }
  (void)snprintf(out, cap, "%s", effective);
  SDL_free(effective);
  return 0;
}

int nxinput_authority_runtime_sdl(nxinput_authority_runtime *runtime,
                                  int consumer_accepts_raw) {
  if (runtime == NULL) {
    return -1;
  }
  memset(runtime, 0, sizeof(*runtime));
  runtime->api_version = NXINPUT_AUTHORITY_API_VERSION;
  runtime->struct_size = sizeof(*runtime);
  runtime->userdata = NULL;
  runtime->getenv_fn = sdl_getenv;
  runtime->read_text_fn = sdl_read_text;
  runtime->device_guid_fn = sdl_device_guid;
  runtime->device_caps_fn = sdl_device_caps;
  runtime->apply_mapping_fn = sdl_apply_mapping;
  runtime->mapping_for_guid_fn = sdl_mapping_for_guid;
  /* SDL ships its own mapping database: authority 4 exists here. */
  runtime->runtime_has_builtin = 1;
  runtime->consumer_accepts_raw = consumer_accepts_raw ? 1 : 0;
  return 0;
}
