#ifndef HUNTDOWN_AUDIO_H
#define HUNTDOWN_AUDIO_H

#include <stdint.h>

/* Installs the version-gated Unity/FMOD stream observer/fallback. */
int hd_audio_stream_install(uintptr_t unity_base);

#endif
