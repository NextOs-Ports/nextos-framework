#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "asset_shim.h"
#include "util.h"

static char g_gamedir[PATH_MAX];
static char g_assets[PATH_MAX];
static char g_save[PATH_MAX];
static int g_manager_tag = 0xA55E7;

typedef struct {
  FILE *f;
  long length;
} BBAsset;

static void mkpath(const char *p) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", p);
  for (char *s = tmp + 1; *s; s++) {
    if (*s != '/') continue;
    *s = '\0';
    mkdir(tmp, 0755);
    *s = '/';
  }
  mkdir(tmp, 0755);
}

void asset_shim_init(const char *gamedir) {
  snprintf(g_gamedir, sizeof(g_gamedir), "%s", gamedir);
  snprintf(g_assets, sizeof(g_assets), "%s/assets", gamedir);
  snprintf(g_save, sizeof(g_save), "%s/save", gamedir);
  mkpath(g_assets);
  mkpath(g_save);
  logPrintf("[asset] asset_shim_init: gamedir=%s assets=%s save=%s\n", g_gamedir, g_assets, g_save);
}

void *asset_shim_manager(void) { return &g_manager_tag; }

void *AAssetManager_fromJava(void *env, void *obj) {
  (void)env; (void)obj;
  return &g_manager_tag;
}

void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;
  char path[PATH_MAX];

  // Try 1: gamedata/assets/<filename>
  snprintf(path, sizeof(path), "%s/%s", g_assets, filename);
  FILE *f = fopen(path, "rb");

  // Try 2: gamedata/<filename>
  if (!f) {
    snprintf(path, sizeof(path), "%s/%s", g_gamedir, filename);
    f = fopen(path, "rb");
  }

  // Try 3: strip path prefix if passed
  if (!f) {
    const char *slash = strrchr(filename, '/');
    if (slash) {
      snprintf(path, sizeof(path), "%s/%s", g_assets, slash + 1);
      f = fopen(path, "rb");
    }
  }

  if (!f) {
    logPrintf("[asset] AAssetManager_open NOT FOUND: '%s'\n", filename);
    return NULL;
  }

  BBAsset *a = (BBAsset *)calloc(1, sizeof(BBAsset));
  if (!a) { fclose(f); return NULL; }
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->length = ftell(f);
  fseek(f, 0, SEEK_SET);
  logPrintf("[asset] AAssetManager_open('%s') -> %ld bytes\n", filename, a->length);
  return a;
}

int AAsset_read(void *asset, void *buf, size_t count) {
  BBAsset *a = (BBAsset *)asset;
  if (!a || !a->f) return -1;
  size_t n = fread(buf, 1, count, a->f);
  return (int)n;
}

long AAsset_seek(void *asset, long offset, int whence) {
  BBAsset *a = (BBAsset *)asset;
  if (!a || !a->f) return -1;
  if (fseek(a->f, offset, whence) != 0) return -1;
  return ftell(a->f);
}

long AAsset_getLength(void *asset) {
  BBAsset *a = (BBAsset *)asset;
  return a ? a->length : 0;
}

long AAsset_getRemainingLength(void *asset) {
  BBAsset *a = (BBAsset *)asset;
  if (!a || !a->f) return 0;
  return a->length - ftell(a->f);
}

void AAsset_close(void *asset) {
  BBAsset *a = (BBAsset *)asset;
  if (!a) return;
  if (a->f) fclose(a->f);
  free(a);
}

const void *AAsset_getBuffer(void *asset) {
  BBAsset *a = (BBAsset *)asset;
  if (!a || !a->f) return NULL;
  long pos = ftell(a->f);
  void *mem = malloc((size_t)a->length + 1);
  if (!mem) return NULL;
  fseek(a->f, 0, SEEK_SET);
  size_t rd = fread(mem, 1, (size_t)a->length, a->f);
  ((char *)mem)[rd] = 0;
  fseek(a->f, pos, SEEK_SET);
  return mem;
}

int AAsset_openFileDescriptor(void *asset, long *outStart, long *outLength) {
  BBAsset *a = (BBAsset *)asset;
  if (!a || !a->f) return -1;
  if (outStart) *outStart = 0;
  if (outLength) *outLength = a->length;
  return dup(fileno(a->f));
}

int ANativeWindow_setBuffersGeometry(void *window, int width, int height, int format) {
  (void)window; (void)width; (void)height; (void)format;
  return 0;
}
