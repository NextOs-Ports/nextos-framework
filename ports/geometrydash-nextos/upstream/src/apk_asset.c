/*
 * apk_asset.c -- serve "file:///android_asset/NAME" out of game.apk.
 *
 * FMOD asks for its sounds by the Android asset URL and, on a phone, the
 * AssetManager answers.  There is no AssetManager here, so every createSound
 * came back FMOD_ERR_FILE_NOTFOUND (18) and the mixer had nothing but silence
 * to hand us -- the audio route was fine all along.
 *
 * The engine itself reads assets straight out of the apk with its own minizip,
 * but FMOD wants a path it can open.  So the 34 audio entries (12.9 MB, all
 * STORED) are copied out on first use into the port's own writable directory
 * and reused from there afterwards.  Nothing else is unpacked, and a second run
 * finds the files already in place.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define ASSET_URL "file:///android_asset/"

typedef struct {
  char *name; /* entry name inside the apk, e.g. "assets/menuLoop.mp3" */
  long long local_hdr;
  unsigned size;
  unsigned method;
} zip_entry;

static char g_apk[PATH_MAX];
static char g_cache[PATH_MAX];
static zip_entry *g_ent;
static int g_ent_n;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

static int is_audio(const char *n) {
  const char *d = strrchr(n, '.');
  return d && (!strcasecmp(d, ".ogg") || !strcasecmp(d, ".mp3") ||
               !strcasecmp(d, ".wav"));
}

/* Indexes only the audio entries under assets/ -- a few dozen of the 5853. */
int gd_apk_index(const char *apk_path, const char *cache_dir) {
  snprintf(g_apk, sizeof(g_apk), "%s", apk_path);
  snprintf(g_cache, sizeof(g_cache), "%s", cache_dir);
  mkdir(g_cache, 0755);

  FILE *f = fopen(apk_path, "rb");
  if (!f) {
    debugPrintf("[apk] nao abriu %s\n", apk_path);
    return 0;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long long fsz = ftello(f);

  /* End of central directory: scan the tail for its signature. */
  long long tail = fsz > 66000 ? 66000 : fsz;
  unsigned char *buf = malloc((size_t)tail);
  if (!buf) {
    fclose(f);
    return 0;
  }
  fseeko(f, fsz - tail, SEEK_SET);
  if (fread(buf, 1, (size_t)tail, f) != (size_t)tail) {
    free(buf);
    fclose(f);
    return 0;
  }
  long long eocd = -1;
  for (long long i = tail - 22; i >= 0; i--)
    if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 &&
        buf[i + 3] == 6) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    debugPrintf("[apk] central directory nao encontrada\n");
    free(buf);
    fclose(f);
    return 0;
  }
  unsigned count = rd16(buf + eocd + 10);
  unsigned cd_size = rd32(buf + eocd + 12);
  unsigned cd_off = rd32(buf + eocd + 16);
  free(buf);

  unsigned char *cd = malloc(cd_size);
  if (!cd) {
    fclose(f);
    return 0;
  }
  fseeko(f, cd_off, SEEK_SET);
  if (fread(cd, 1, cd_size, f) != cd_size) {
    free(cd);
    fclose(f);
    return 0;
  }
  fclose(f);

  g_ent = calloc(count ? count : 1, sizeof(zip_entry));
  unsigned p = 0;
  for (unsigned i = 0; i < count && p + 46 <= cd_size; i++) {
    if (rd32(cd + p) != 0x02014b50)
      break;
    unsigned method = rd16(cd + p + 10);
    unsigned usize = rd32(cd + p + 24);
    unsigned nlen = rd16(cd + p + 28);
    unsigned elen = rd16(cd + p + 30);
    unsigned clen = rd16(cd + p + 32);
    unsigned lho = rd32(cd + p + 42);
    const char *name = (const char *)(cd + p + 46);
    if (nlen > 8 && !strncmp(name, "assets/", 7)) {
      char tmp[512];
      unsigned n = nlen < sizeof(tmp) - 1 ? nlen : (unsigned)sizeof(tmp) - 1;
      memcpy(tmp, name, n);
      tmp[n] = 0;
      if (is_audio(tmp)) {
        g_ent[g_ent_n].name = strdup(tmp);
        g_ent[g_ent_n].local_hdr = lho;
        g_ent[g_ent_n].size = usize;
        g_ent[g_ent_n].method = method;
        g_ent_n++;
      }
    }
    p += 46 + nlen + elen + clen;
  }
  free(cd);
  debugPrintf("[apk] %d audios indexados em %s\n", g_ent_n, apk_path);
  return g_ent_n;
}

static const zip_entry *find_entry(const char *asset) {
  char want[512];
  snprintf(want, sizeof(want), "assets/%s", asset);
  for (int i = 0; i < g_ent_n; i++)
    if (!strcmp(g_ent[i].name, want))
      return &g_ent[i];
  return NULL;
}

/* Copies the entry out once; every later call just finds it already there. */
static int materialize(const zip_entry *e, const char *out) {
  struct stat st;
  if (stat(out, &st) == 0 && st.st_size == (off_t)e->size)
    return 1;
  if (e->method != 0) {
    debugPrintf("[apk] %s esta comprimido (metodo %u) -- nao suportado\n",
                e->name, e->method);
    return 0;
  }
  FILE *f = fopen(g_apk, "rb");
  if (!f)
    return 0;
  /* The data offset is only knowable from the LOCAL header: its name and
   * extra fields may differ in length from the central directory's. */
  unsigned char lh[30];
  fseeko(f, e->local_hdr, SEEK_SET);
  if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) {
    fclose(f);
    return 0;
  }
  long long data = e->local_hdr + 30 + rd16(lh + 26) + rd16(lh + 28);
  fseeko(f, data, SEEK_SET);

  char tmp[PATH_MAX + 8];
  snprintf(tmp, sizeof(tmp), "%s.part", out);
  FILE *o = fopen(tmp, "wb");
  if (!o) {
    fclose(f);
    return 0;
  }
  char blk[64 * 1024];
  unsigned left = e->size;
  while (left) {
    size_t want = left < sizeof(blk) ? left : sizeof(blk);
    size_t got = fread(blk, 1, want, f);
    if (!got || fwrite(blk, 1, got, o) != got)
      break;
    left -= (unsigned)got;
  }
  fclose(o);
  fclose(f);
  if (left) {
    unlink(tmp);
    debugPrintf("[apk] copia de %s incompleta\n", e->name);
    return 0;
  }
  if (rename(tmp, out) != 0) {
    unlink(tmp);
    return 0;
  }
  return 1;
}

/* Returns a real path for an android_asset URL, or NULL to leave it alone.
 * The buffer is the caller's. */
const char *gd_asset_path(const char *url, char *out, size_t outsz) {
  if (!url || strncmp(url, ASSET_URL, strlen(ASSET_URL)) != 0)
    return NULL;
  const char *asset = url + strlen(ASSET_URL);
  const zip_entry *e = find_entry(asset);
  if (!e) {
    debugPrintf("[apk] asset nao encontrado no apk: %s\n", asset);
    return NULL;
  }
  const char *base = strrchr(asset, '/');
  snprintf(out, outsz, "%s/%s", g_cache, base ? base + 1 : asset);
  if (!materialize(e, out))
    return NULL;
  return out;
}
