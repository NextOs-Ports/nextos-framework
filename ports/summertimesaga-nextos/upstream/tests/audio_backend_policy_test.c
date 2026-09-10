#include "audio_backend_policy.h"

#include <stdio.h>
#include <stdlib.h>

struct policy_case {
  const char *name;
  const char *explicit_driver;
  const char *inherited_driver;
  int pulse_available;
  int alsa_available;
  int keep_inherited_pulse;
  enum ss_audio_backend expected;
  int escaped;
  int explicit_choice;
};

int main(void) {
  static const struct policy_case cases[] = {
      {"automatic pulse", NULL, NULL, 1, 1, 0,
       SS_AUDIO_BACKEND_PULSE, 0, 0},
      {"automatic alsa", NULL, NULL, 0, 1, 0,
       SS_AUDIO_BACKEND_ALSA, 0, 0},
      {"inherited alsa", NULL, "alsa", 1, 1, 0,
       SS_AUDIO_BACKEND_ALSA, 0, 0},
      {"rocknix pulse escape", NULL, "pulseaudio", 1, 1, 0,
       SS_AUDIO_BACKEND_ALSA, 1, 0},
      {"legacy pulse escape", NULL, "pulse", 1, 1, 0,
       SS_AUDIO_BACKEND_ALSA, 1, 0},
      {"keep inherited pulse", NULL, "pulseaudio", 1, 1, 1,
       SS_AUDIO_BACKEND_PULSE, 0, 0},
      {"pulse unavailable", NULL, "pulseaudio", 0, 1, 1,
       SS_AUDIO_BACKEND_ALSA, 0, 0},
      {"explicit pulse", "pulse", "alsa", 1, 1, 0,
       SS_AUDIO_BACKEND_PULSE, 0, 1},
      {"explicit alsa", "alsa", "pulseaudio", 1, 1, 1,
       SS_AUDIO_BACKEND_ALSA, 0, 1},
      {"explicit mute", "none", NULL, 1, 1, 0,
       SS_AUDIO_BACKEND_NONE, 0, 1},
      {"explicit missing backend", "pulse", NULL, 0, 1, 0,
       SS_AUDIO_BACKEND_NONE, 0, 1},
      {"invalid becomes automatic", "invalid", NULL, 0, 1, 0,
       SS_AUDIO_BACKEND_ALSA, 0, 0},
      {"no backend", NULL, NULL, 0, 0, 0,
       SS_AUDIO_BACKEND_NONE, 0, 0},
  };

  for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int escaped = -1;
    int explicit_choice = -1;
    enum ss_audio_backend actual = ss_audio_choose_backend(
        cases[index].explicit_driver, cases[index].inherited_driver,
        cases[index].pulse_available, cases[index].alsa_available,
        cases[index].keep_inherited_pulse, &escaped, &explicit_choice);
    if (actual != cases[index].expected || escaped != cases[index].escaped ||
        explicit_choice != cases[index].explicit_choice) {
      fprintf(stderr,
              "%s: backend=%s escaped=%d explicit=%d (expected %s/%d/%d)\n",
              cases[index].name, ss_audio_backend_name(actual), escaped,
              explicit_choice, ss_audio_backend_name(cases[index].expected),
              cases[index].escaped, cases[index].explicit_choice);
      return EXIT_FAILURE;
    }
  }

  enum ss_audio_backend parsed = SS_AUDIO_BACKEND_NONE;
  if (ss_audio_parse_backend("invalid", &parsed) != -1 ||
      ss_audio_parse_backend("auto", &parsed) != 0) {
    fputs("explicit backend parser contract failed\n", stderr);
    return EXIT_FAILURE;
  }

  puts("audio backend policy: all cases passed");
  return EXIT_SUCCESS;
}
