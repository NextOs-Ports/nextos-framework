// game — as implementacoes em C dos 33 metodos estaticos do MainActivity.
// Espelham 1:1 o Java decompilado (ver STUDY.md secoes 2.6 e 9).
#ifndef GAME_H
#define GAME_H

#include <stddef.h>
#include <stdint.h>

// --- idioma -----------------------------------------------------------------
// 🚨 Indice 1 = "en". JAMAIS japones (0).
#define GAME_LANG_EN 1
void game_init_language(void);
extern int g_language;

// --- estado do pad (espelha MainActivity.l) ---------------------------------
extern int g_pad_mask;      // mascara de 16 bits entregue no 7o arg de touch()
extern int g_ab_swapped;    // MainActivity.n  (assignABButton)
extern int g_back_enabled;  // MainActivity.m  (assignBackButton)

// --- VFS ARC1 ---------------------------------------------------------------
int  game_vfs_open(const char *obb_path);
// Devolve buffer malloc'ado (ou NULL). *len recebe o tamanho.
uint8_t *game_vfs_read(const char *name, size_t *len);

// --- callbacks (nomes iguais aos do Java) -----------------------------------
int64_t  game_getCurrentFrame(int64_t last);
uint8_t *game_loadFile(const char *name, size_t *len);
uint8_t *game_loadRawFile(const char *name, size_t *len);
uint8_t *game_loadSound(const char *name, size_t *len);  // FF4: .akb de SOUND/{BGM,SE,VOICE}
int32_t *game_loadTexture(const uint8_t *png, size_t png_len, size_t *out_ints);
int32_t *game_drawFont(const char *text, int a, int b, int c, size_t *out_ints);
uint8_t *game_decodeString(const uint8_t *in, size_t in_len, size_t *out_len);
void     game_trace(const char *msg);

// FMV: playMovie liga o estado, e o engine so volta a renderizar quando
// getMovieState() devolve 0 (ver STUDY.md §10).
void game_playMovie(void);
void game_stopMovie(void);
int  game_getMovieState(void);

void game_playSound(int ch, const char *name);
void game_pauseSound(int ch, int flag);
void game_stopSound(int ch);
void game_setSoundVolume(int ch, float vol);
int  game_getSoundState(int ch);

void game_createSaveFile(int idx);
uint8_t *game_getSaveFileName(size_t *len);
uint8_t *game_getSaveDataPath(size_t *len);  // FF4: caminho completo do save.bin

#endif
