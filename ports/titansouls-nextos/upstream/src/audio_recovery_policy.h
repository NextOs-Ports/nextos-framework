#ifndef TS_AUDIO_RECOVERY_POLICY_H
#define TS_AUDIO_RECOVERY_POLICY_H

#define TS_FMOD_VERSION_44417 0x00044417u
#define TS_FMOD_OUTPUT_NOSOUND 2
#define TS_FMOD_OUTPUT_OPENSL 22

int ts_audio_select_opensl_preinit(int disabled, int version_result,
                                   unsigned int version, int output_result,
                                   int output);
int ts_audio_retry_opensl(int disabled, int version_result,
                          unsigned int version, int first_init_result,
                          int opensl_already_selected,
                          unsigned int retry_count);

#endif
