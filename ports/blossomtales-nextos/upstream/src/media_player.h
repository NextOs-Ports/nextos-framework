/*
 * media_player.h -- android.media.MediaPlayer de verdade para o so-loader.
 *
 * O MonoGame toca SFX pelo OpenAL (SoundEffect) mas a MUSICA (Song) sai
 * inteira pelo MediaPlayer do Java: Reset -> SetDataSource(fd,off,len) ->
 * Prepare -> Looping -> SetVolume -> Start. Sem esse caminho o jogo roda com
 * som de cenario e nenhuma trilha, sem erro nenhum no log.
 *
 * Aqui a decodificacao e feita por um processo `ffmpeg` lendo o arquivo do
 * asset (m4a/AAC no Blossom Tales) e devolvendo PCM s16 estereo 48 kHz por um
 * pipe; uma thread alimenta uma fonte OpenAL propria, que se mistura no mesmo
 * contexto que o MonoGame ja criou para os efeitos. Trocar o decodificador
 * depois e mudanca contida: so `decoder_spawn` conhece o formato de entrada.
 */
#ifndef __MEDIA_PLAYER_H__
#define __MEDIA_PLAYER_H__

typedef struct nx_media_player nx_media_player;

nx_media_player *nx_mp_create(void);
void nx_mp_destroy(nx_media_player *mp);

/* Volta ao estado "sem fonte", parando o que estiver tocando. */
void nx_mp_reset(nx_media_player *mp);
/* Caminho real do arquivo (ja resolvido a partir do AssetFileDescriptor). */
int  nx_mp_set_source(nx_media_player *mp, const char *path);
/* Le a duracao e deixa pronto para tocar. Devolve 0 se nao ha fonte. */
int  nx_mp_prepare(nx_media_player *mp);

void nx_mp_set_looping(nx_media_player *mp, int looping);
int  nx_mp_get_looping(nx_media_player *mp);
void nx_mp_set_volume(nx_media_player *mp, float left, float right);

void nx_mp_start(nx_media_player *mp);
void nx_mp_pause(nx_media_player *mp);
void nx_mp_stop(nx_media_player *mp);
void nx_mp_seek_ms(nx_media_player *mp, int position_ms);

int  nx_mp_duration_ms(nx_media_player *mp);
int  nx_mp_position_ms(nx_media_player *mp);
int  nx_mp_is_playing(nx_media_player *mp);

#endif
