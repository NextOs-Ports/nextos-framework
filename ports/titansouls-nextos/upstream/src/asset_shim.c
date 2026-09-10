/*
 * asset_shim.c -- AAssetManager sobre o assets/ do APK, ja extraido em disco.
 *
 * Nada de mapear o APK: o NXExtract entrega <gamedir>/assets/ com os arquivos
 * soltos. A API de asset entao e' um fopen com nome relativo — que e'
 * exatamente o contrato que a engine espera.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "asset_shim.h"
#include "util.h"

#define TS_PACKAGE "com.devolver.titansouls"

static char g_gamedir[PATH_MAX];
static char g_assets[PATH_MAX];
static char g_internal[PATH_MAX];
static char g_external[PATH_MAX];
static char g_obb[PATH_MAX];
static int g_manager = 0xA55E7;

typedef struct {
  FILE *f;
  long length;
} TSAsset;

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
  snprintf(g_external, sizeof(g_external), "%s/sdcard", gamedir);
  snprintf(g_internal, sizeof(g_internal), "%s/files", gamedir);
  snprintf(g_obb, sizeof(g_obb), "%s/Android/obb/" TS_PACKAGE, g_external);

  /* Os tres caminhos que a engine tem CRAVADOS no binario vivem debaixo de
   * <gamedir>/sdcard; a interposicao de open/stat traduz /sdcard/... para ca.
   *
   * A ARVORE DO SAVE NAO E' PALPITE: no Android quem a prepara e' a
   * TVActivity$FileTask, ANTES de o motor rodar (lido no dex deste APK):
   *
   *     new File("/sdcard/TitanSoulsSave/data/SAVE").mkdirs();
   *     if (!new File("/sdcard/TitanSoulsSave/data/config.txt").exists())
   *         copyFile("config.txt", "/sdcard/TitanSoulsSave/data/config.txt");
   *
   * A engine so' abre os arquivos — ela NAO cria o `data/SAVE`. Sem esse
   * diretorio o fopen de escrita falha calado: o jogo loga "Saving …", nada
   * chega ao disco, e na proxima abertura so' existe New Game. Fazemos aqui o
   * MESMO que a Activity faria, no MESMO momento (antes do android_main). */
  mkpath(g_internal);
  mkpath(g_obb);
  {
    char dir[PATH_MAX], dst[PATH_MAX], src[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/TitanSoulsSave/data/SAVE", g_external);
    mkpath(dir);
    snprintf(dst, sizeof(dst), "%s/TitanSoulsSave/data/config.txt", g_external);
    if (access(dst, F_OK) != 0) {
      snprintf(src, sizeof(src), "%s/config.txt", g_assets);
      FILE *in = fopen(src, "rb");
      FILE *out = in ? fopen(dst, "wb") : NULL;
      if (in && out) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        logPrintf("asset_shim: config.txt copiado para o save (%s)\n", dst);
      } else {
        logPrintf("asset_shim: NAO copiei config.txt (%s -> %s)\n", src, dst);
      }
      if (out) fclose(out);
      if (in) fclose(in);
    }
    logPrintf("asset_shim: arvore de save pronta em %s\n", dir);
  }
  logPrintf("asset_shim: gamedir=%s assets=%s obb=%s\n", g_gamedir, g_assets, g_obb);
}

void *asset_shim_manager(void) { return &g_manager; }
void *AAssetManager_fromJava(void *env, void *obj) {
  (void)env; (void)obj;
  return &g_manager;
}

const char *ts_paths_gamedir(void) { return g_gamedir; }
const char *ts_paths_internal(void) { return g_internal; }
const char *ts_paths_external(void) { return g_external; }
const char *ts_paths_obb(void) { return g_obb; }

void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", g_assets, filename);
  FILE *f = fopen(path, "rb");
  if (!f) {
    /* alguns builds pedem "assets/x" — tenta tambem sem duplicar o prefixo */
    const char *slash = strrchr(filename, '/');
    if (slash) {
      snprintf(path, sizeof(path), "%s/%s", g_assets, slash + 1);
      f = fopen(path, "rb");
    }
  }
  if (!f) {
    logPrintf("AAssetManager_open: NAO ACHOU '%s' (em %s)\n", filename, g_assets);
    return NULL;
  }
  TSAsset *a = (TSAsset *)calloc(1, sizeof(TSAsset));
  if (!a) { fclose(f); return NULL; }
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->length = ftell(f);
  fseek(f, 0, SEEK_SET);
  debugPrintf("AAssetManager_open('%s') -> %ld bytes\n", filename, a->length);
  return a;
}

int AAsset_read(void *asset, void *buf, size_t count) {
  TSAsset *a = (TSAsset *)asset;
  if (!a || !a->f) return -1;
  size_t n = fread(buf, 1, count, a->f);
  return (int)n;
}

long AAsset_seek(void *asset, long offset, int whence) {
  TSAsset *a = (TSAsset *)asset;
  if (!a || !a->f) return -1;
  if (fseek(a->f, offset, whence) != 0) return -1;
  return ftell(a->f);
}

long AAsset_getLength(void *asset) {
  TSAsset *a = (TSAsset *)asset;
  return a ? a->length : 0;
}

long AAsset_getRemainingLength(void *asset) {
  TSAsset *a = (TSAsset *)asset;
  if (!a || !a->f) return 0;
  return a->length - ftell(a->f);
}

void AAsset_close(void *asset) {
  TSAsset *a = (TSAsset *)asset;
  if (!a) return;
  if (a->f) fclose(a->f);
  free(a);
}

const void *AAsset_getBuffer(void *asset) {
  TSAsset *a = (TSAsset *)asset;
  if (!a || !a->f) return NULL;
  long pos = ftell(a->f);
  void *mem = malloc((size_t)a->length + 1);
  if (!mem) return NULL;
  fseek(a->f, 0, SEEK_SET);
  size_t rd = fread(mem, 1, (size_t)a->length, a->f);
  ((char *)mem)[rd] = 0;
  fseek(a->f, pos, SEEK_SET);
  return mem; /* vazamento consciente: a engine nunca libera este buffer */
}

int AAsset_openFileDescriptor(void *asset, long *outStart, long *outLength) {
  TSAsset *a = (TSAsset *)asset;
  if (!a || !a->f) return -1;
  if (outStart) *outStart = 0;
  if (outLength) *outLength = a->length;
  return dup(fileno(a->f));
}
