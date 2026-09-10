#ifndef SWORDIGO_MUSIC_PLAYER_H
#define SWORDIGO_MUSIC_PLAYER_H

void music_init(const char *base_dir);
void music_deinit(void);
void music_load(const char *logical_name);
void music_play(void);
void music_pause(void);
void music_stop(void);
void music_set_loop(int looping);
void music_set_volume(float vol);

#endif
