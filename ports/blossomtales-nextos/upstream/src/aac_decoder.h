/*
 * aac_decoder.h -- decodificador AAC-LC/MP4 INTERNO do port.
 *
 * Substitui a dependencia de `ffmpeg`/`ffprobe` do sistema, que era especifica
 * do NextOS e nao vale para um pacote universal ("jogo abre sem musica" e'
 * falha). Toda a trilha do Blossom Tales sao 43 arquivos `.m4a`: AAC-LC,
 * 48 kHz, estereo, em container MP4.
 *
 * Duas pecas, ambas de terceiros e pinadas (ver NOTICE.md/SBOM):
 *   - minimp4 (CC0-1.0)        -- demux do container MP4;
 *   - FDK-AAC 2.0.3 (Fraunhofer FDK AAC Codec Library) -- decodificacao AAC.
 *
 * A saida e' sempre PCM s16 interleaved, estereo, na taxa pedida em
 * nx_aac_open(), para entrar direto na fila OpenAL do media_player.
 */
#ifndef NX_AAC_DECODER_H
#define NX_AAC_DECODER_H

typedef struct nx_aac nx_aac;

/* Abre `path` e posiciona em `start_ms`. `rate_hz`/`channels` e' o formato de
 * SAIDA desejado (o decodificador converte quando o arquivo diferir).
 * Devolve NULL quando o arquivo nao existe, nao e' MP4/AAC ou o decodificador
 * nao pode ser criado. */
nx_aac *nx_aac_open(const char *path, int start_ms, int rate_hz, int channels);

/* Le ate `bytes` de PCM. Devolve >0 = bytes escritos, 0 = fim da faixa,
 * -1 = erro. Nunca bloqueia em I/O de rede nem em processo externo. */
int nx_aac_read(nx_aac *d, void *buf, int bytes);

void nx_aac_close(nx_aac *d);

/* Duracao em ms lida do proprio container (substitui o `ffprobe`).
 * 0 quando desconhecida. */
int nx_aac_duration_ms(const char *path);

#endif
