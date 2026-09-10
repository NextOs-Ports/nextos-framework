#ifndef MAGIC_AUDIO_BACKEND_H
#define MAGIC_AUDIO_BACKEND_H

int audio_backend_init(const char *apk_path);
void audio_backend_shutdown(void);

int audio_backend_load_music(const char *name);
int audio_backend_load_sound(const char *name);
void audio_backend_play_sample(const char *name, int loops);
void audio_backend_stop_sample(const char *name);
void audio_backend_pause_sample(const char *name);
void audio_backend_set_sample_volume(const char *name, float volume);
void audio_backend_set_sample_pan(const char *name, float pan);
void audio_backend_set_sample_speed(const char *name, float speed);
int audio_backend_sample_exists(const char *name);
int audio_backend_is_sample_playing(const char *name);
void audio_backend_set_global_volume(float volume);
float audio_backend_get_global_volume(void);
void audio_backend_set_sound_effect_volume(float volume);
float audio_backend_get_sound_effect_volume(void);

#endif
