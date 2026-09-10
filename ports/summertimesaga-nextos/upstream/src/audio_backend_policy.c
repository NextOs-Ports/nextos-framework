#include "audio_backend_policy.h"

#include <string.h>

static int ss_audio_is_pulse_name(const char *name) {
  return name &&
         (strcmp(name, "pulse") == 0 || strcmp(name, "pulseaudio") == 0);
}

const char *ss_audio_backend_name(enum ss_audio_backend backend) {
  switch (backend) {
  case SS_AUDIO_BACKEND_PULSE:
    return "pulse";
  case SS_AUDIO_BACKEND_ALSA:
    return "alsa";
  default:
    return "none";
  }
}

int ss_audio_parse_backend(const char *value,
                           enum ss_audio_backend *backend) {
  if (!value || !*value || strcmp(value, "auto") == 0)
    return 0;

  enum ss_audio_backend parsed;
  if (ss_audio_is_pulse_name(value))
    parsed = SS_AUDIO_BACKEND_PULSE;
  else if (strcmp(value, "alsa") == 0)
    parsed = SS_AUDIO_BACKEND_ALSA;
  else if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 ||
           strcmp(value, "disabled") == 0)
    parsed = SS_AUDIO_BACKEND_NONE;
  else
    return -1;

  if (backend)
    *backend = parsed;
  return 1;
}

enum ss_audio_backend ss_audio_choose_backend(
    const char *explicit_driver, const char *inherited_sdl_driver,
    int pulse_available, int alsa_available, int keep_inherited_pulse,
    int *escaped_inherited_pulse, int *used_explicit_choice) {
  if (escaped_inherited_pulse)
    *escaped_inherited_pulse = 0;
  if (used_explicit_choice)
    *used_explicit_choice = 0;

  enum ss_audio_backend explicit_backend = SS_AUDIO_BACKEND_NONE;
  int explicit_status =
      ss_audio_parse_backend(explicit_driver, &explicit_backend);
  if (explicit_status == 1) {
    if (used_explicit_choice)
      *used_explicit_choice = 1;
    if (explicit_backend == SS_AUDIO_BACKEND_PULSE && !pulse_available)
      return SS_AUDIO_BACKEND_NONE;
    if (explicit_backend == SS_AUDIO_BACKEND_ALSA && !alsa_available)
      return SS_AUDIO_BACKEND_NONE;
    return explicit_backend;
  }

  if (inherited_sdl_driver &&
      strcmp(inherited_sdl_driver, "alsa") == 0 && alsa_available)
    return SS_AUDIO_BACKEND_ALSA;

  if (ss_audio_is_pulse_name(inherited_sdl_driver)) {
    if (!keep_inherited_pulse && alsa_available) {
      if (escaped_inherited_pulse)
        *escaped_inherited_pulse = 1;
      return SS_AUDIO_BACKEND_ALSA;
    }
    if (pulse_available)
      return SS_AUDIO_BACKEND_PULSE;
    if (alsa_available)
      return SS_AUDIO_BACKEND_ALSA;
  }

  if (pulse_available)
    return SS_AUDIO_BACKEND_PULSE;
  if (alsa_available)
    return SS_AUDIO_BACKEND_ALSA;
  return SS_AUDIO_BACKEND_NONE;
}
