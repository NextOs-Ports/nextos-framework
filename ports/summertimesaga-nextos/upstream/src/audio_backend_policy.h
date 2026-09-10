/* Capability-based host audio selection for the Ren'Py Android bridge. */

#ifndef SUMMERTIME_AUDIO_BACKEND_POLICY_H
#define SUMMERTIME_AUDIO_BACKEND_POLICY_H

enum ss_audio_backend {
  SS_AUDIO_BACKEND_NONE = 0,
  SS_AUDIO_BACKEND_PULSE,
  SS_AUDIO_BACKEND_ALSA,
};

const char *ss_audio_backend_name(enum ss_audio_backend backend);

/*
 * Returns 1 for a recognized explicit backend, 0 for an empty/automatic
 * choice and -1 for an invalid value. "none" is a recognized explicit mute.
 */
int ss_audio_parse_backend(const char *value,
                           enum ss_audio_backend *backend);

/*
 * Select the external PCM sink without entering either server backend.
 * Explicit SUMMERTIME_AUDIO_DRIVER choices win. Otherwise an inherited
 * PulseAudio-only SDL selection can escape directly to ALSA, matching the
 * behavior physically validated by Horizon Chase and TASM2 on ROCKNIX.
 */
enum ss_audio_backend ss_audio_choose_backend(
    const char *explicit_driver, const char *inherited_sdl_driver,
    int pulse_available, int alsa_available, int keep_inherited_pulse,
    int *escaped_inherited_pulse, int *used_explicit_choice);

#endif
