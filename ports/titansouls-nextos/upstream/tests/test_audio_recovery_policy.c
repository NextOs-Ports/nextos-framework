#include <assert.h>
#include "audio_recovery_policy.h"

int main(void) {
  assert(ts_audio_select_opensl_preinit(0, 0, TS_FMOD_VERSION_44417, 0,
                                        TS_FMOD_OUTPUT_NOSOUND));
  assert(!ts_audio_select_opensl_preinit(0, 0, 0x00044416u, 0,
                                         TS_FMOD_OUTPUT_NOSOUND));
  assert(!ts_audio_select_opensl_preinit(1, 0, TS_FMOD_VERSION_44417, 0,
                                         TS_FMOD_OUTPUT_NOSOUND));

  assert(ts_audio_retry_opensl(0, 0, TS_FMOD_VERSION_44417, 55, 0, 0));
  assert(ts_audio_retry_opensl(0, 0, TS_FMOD_VERSION_44417, 79, 0, 0));
  assert(!ts_audio_retry_opensl(0, 0, TS_FMOD_VERSION_44417, 33, 0, 0));
  assert(!ts_audio_retry_opensl(0, 0, 0x00044416u, 55, 0, 0));
  assert(!ts_audio_retry_opensl(0, 0, TS_FMOD_VERSION_44417, 55, 0, 1));
  assert(!ts_audio_retry_opensl(1, 0, TS_FMOD_VERSION_44417, 55, 0, 0));
  return 0;
}
