#include "audio_recovery_policy.h"

static int output_failure(int result) {
  return result == 31 || (result >= 55 && result <= 62) || result == 66 ||
         result == 79;
}

int ts_audio_select_opensl_preinit(int disabled, int version_result,
                                   unsigned int version, int output_result,
                                   int output) {
  return !disabled && version_result == 0 &&
         version == TS_FMOD_VERSION_44417 && output_result == 0 &&
         output == TS_FMOD_OUTPUT_NOSOUND;
}

int ts_audio_retry_opensl(int disabled, int version_result,
                          unsigned int version, int first_init_result,
                          int opensl_already_selected,
                          unsigned int retry_count) {
  return !disabled && !opensl_already_selected && retry_count == 0u &&
         version_result == 0 && version == TS_FMOD_VERSION_44417 &&
         output_failure(first_init_result);
}
