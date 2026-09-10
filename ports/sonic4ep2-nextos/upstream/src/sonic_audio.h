#ifndef SONIC_AUDIO_H
#define SONIC_AUDIO_H

int sonic_audio_play_sfx(const char *key, float volume, int loop);
void sonic_audio_stop_sfx(int handle);
void sonic_audio_pause_sfx(int handle, int paused);
void sonic_audio_set_sfx_volume(int handle, float volume);
void sonic_audio_reset_sfx(void);
void sonic_audio_set_sfx_bank(const char *bank);

void sonic_audio_music_set_source(int id, const char *key);
void sonic_audio_music_start(int id);
void sonic_audio_music_stop(int id);
void sonic_audio_music_pause(int id, int paused);
void sonic_audio_music_set_volume(int id, float volume);
void sonic_audio_music_set_loop(int id, int loop);
void sonic_audio_reset_music(int id);
int sonic_audio_music_state(int id);

#endif
