#ifndef __ASSET_SHIM_H__
#define __ASSET_SHIM_H__

#include <stddef.h>

void asset_shim_init(const char *gamedir);
void *asset_shim_manager(void);

void *AAssetManager_fromJava(void *env, void *obj);
void *AAssetManager_open(void *mgr, const char *filename, int mode);
int AAsset_read(void *asset, void *buf, size_t count);
long AAsset_seek(void *asset, long offset, int whence);
long AAsset_getLength(void *asset);
long AAsset_getRemainingLength(void *asset);
void AAsset_close(void *asset);
const void *AAsset_getBuffer(void *asset);
int AAsset_openFileDescriptor(void *asset, long *outStart, long *outLength);

int ANativeWindow_setBuffersGeometry(void *window, int width, int height, int format);

#endif
