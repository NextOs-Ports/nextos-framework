/*
 * obb_data.h -- leitura dos dados do FF4 do main.obb (replica loadFile/h/g/encode/l
 * do MainActivity do APK, decompilado). Índice L + decripta LCG-XOR + gunzip.
 */
#ifndef FF4A_OBB_DATA_H
#define FF4A_OBB_DATA_H

/* bootstrap: abre o OBB, lê+decripta o header (magic C4F1), carrega o índice L. */
int obb_init(const char *obb_path);

/* carrega um asset por nome-completo (ex "files/music_player.bbd"); devolve buffer
 * malloc'd descomprimido + tamanho, ou NULL se não achar. */
unsigned char *obb_load_exact(const char *name, int *out_len);

/* equivalente ao loadFile(path): tenta "en.lproj/<path>" (inglês) e cai p/
 * "files/<path>". */
unsigned char *obb_load_file(const char *path, int *out_len);

#endif
