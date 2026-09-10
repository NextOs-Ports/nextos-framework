/*
 * asset_shim.h -- AAssetManager sobre o assets/ extraido do APK + os caminhos
 * do port.
 *
 * A engine le config.txt, os 4 shaders de loading, loading.png, circle.png,
 * ui_image.png e font.png PELA API DE ASSET. Sem isto ela nem chega a desenhar
 * a tela de loading e o sintoma parece "tela preta".
 */

#ifndef __ASSET_SHIM_H__
#define __ASSET_SHIM_H__

#include <stddef.h>
#include <stdint.h>

/* raiz = <gamedir>; espera <gamedir>/assets e <gamedir>/sdcard */
void asset_shim_init(const char *gamedir);

void *asset_shim_manager(void);

const char *ts_paths_gamedir(void);
const char *ts_paths_internal(void);  /* .../files      */
const char *ts_paths_external(void);  /* .../sdcard     */
const char *ts_paths_obb(void);       /* .../sdcard/Android/obb/<pkg> */

/* NDK asset API (nomes exatos que a engine importa) */
void *AAssetManager_open(void *mgr, const char *filename, int mode);
int AAsset_read(void *asset, void *buf, size_t count);
long AAsset_seek(void *asset, long offset, int whence);
long AAsset_getLength(void *asset);
long AAsset_getRemainingLength(void *asset);
void AAsset_close(void *asset);
const void *AAsset_getBuffer(void *asset);
int AAsset_openFileDescriptor(void *asset, long *outStart, long *outLength);
void *AAssetManager_fromJava(void *env, void *obj);

#endif
