/*
 * obb_data.c -- replica o pipeline de dados do FF4 (decompilado do MainActivity):
 *   f():   hdr=g(0,16); encode(hdr,0x5E51D48); magic C4F1; idxOff=a(hdr,8);
 *          idxLen=a(hdr,12); L=g(idxOff,idxLen); encode(L,idxOff+0x5E51D48); L=l(L).
 *   h(nm): busca binária em L (count=a(L,0); stride 12; nome em +4; dados +8/+12);
 *          raw=g(dataOff,dataLen); encode(raw,dataOff+0x5E51D48); return l(raw).
 *   a():   int32 little-endian.  g(): read do main.obb.  l(): [BE len][gzip].
 *   encode(): LCG-XOR  st=key; por byte: st=st*0x41C64E6D+0x3039; b^=(st>>24).
 */
#define _GNU_SOURCE
#include "obb_data.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define ENC_KEY_CONST 98910408u /* 0x5E51D48 */
#define OBB_MAGIC 0x31435241     /* "ARC1" (826495553) little-endian */

static FILE *g_obb;
static unsigned char *g_L;
static int g_L_len;

/* LCG-XOR in place (== encode nativo) */
static void enc(unsigned char *b, int len, unsigned int key) {
  unsigned int st = key;
  for (int i = 0; i < len; i++) {
    st = st * 0x41C64E6Du + 0x3039u;
    b[i] ^= (unsigned char)(st >> 24);
  }
}

/* a(): int32 little-endian */
static int rd32(const unsigned char *b, int off) {
  return (b[off] & 0xff) | ((b[off + 1] & 0xff) << 8) |
         ((b[off + 2] & 0xff) << 16) | ((b[off + 3] & 0xff) << 24);
}

/* g(): lê len bytes do OBB a partir de off */
static unsigned char *obb_read(int off, int len) {
  if (len < 0) return NULL;
  unsigned char *buf = (unsigned char *)malloc(len ? len : 1);
  if (!buf) return NULL;
  if (fseek(g_obb, off, SEEK_SET) != 0 ||
      fread(buf, 1, len, g_obb) != (size_t)len) {
    free(buf);
    return NULL;
  }
  return buf;
}

/* l(): [4-byte BE ulen][gzip stream] -> descomprimido */
static unsigned char *l_decompress(unsigned char *b, int len, int *out_len) {
  if (len < 4) return NULL;
  int ulen = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; /* BE */
  if (ulen < 0) return NULL;
  unsigned char *out = (unsigned char *)malloc(ulen ? ulen : 1);
  if (!out) return NULL;
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  zs.next_in = b + 4;
  zs.avail_in = len - 4;
  zs.next_out = out;
  zs.avail_out = ulen;
  if (inflateInit2(&zs, 16 + 15) != Z_OK) { /* gzip */
    free(out);
    return NULL;
  }
  int r = inflate(&zs, Z_FINISH);
  inflateEnd(&zs);
  if (r != Z_STREAM_END && r != Z_OK && r != Z_BUF_ERROR) {
    free(out);
    return NULL;
  }
  *out_len = ulen;
  return out;
}

int obb_init(const char *obb_path) {
  g_obb = fopen(obb_path, "rb");
  if (!g_obb) {
    debugPrintf("[OBB] fopen(%s) falhou\n", obb_path);
    return -1;
  }
  unsigned char *hdr = obb_read(0, 16);
  if (!hdr) return -2;
  enc(hdr, 16, ENC_KEY_CONST);
  int magic = rd32(hdr, 0);
  if (magic != OBB_MAGIC) {
    debugPrintf("[OBB] magic errado: 0x%08x (esperado 0x%08x)\n", magic, OBB_MAGIC);
    free(hdr);
    return -3;
  }
  int idxOff = rd32(hdr, 8);
  int idxLen = rd32(hdr, 12);
  free(hdr);
  debugPrintf("[OBB] magic OK; índice off=%d len=%d\n", idxOff, idxLen);
  unsigned char *idx = obb_read(idxOff, idxLen);
  if (!idx) return -4;
  enc(idx, idxLen, (unsigned int)(idxOff + (int)ENC_KEY_CONST));
  g_L = l_decompress(idx, idxLen, &g_L_len);
  free(idx);
  if (!g_L) {
    debugPrintf("[OBB] gunzip do índice falhou\n");
    return -5;
  }
  int count = rd32(g_L, 0);
  debugPrintf("[OBB] índice OK: L=%d bytes, %d entradas\n", g_L_len, count);
  return 0;
}

unsigned char *obb_load_exact(const char *name, int *out_len) {
  if (!g_L) return NULL;
  int count = rd32(g_L, 0);
  int namelen = (int)strlen(name);
  int descOff = 0;
  int lo = 0, hi = count;
  while (hi > lo) {
    int mid = (lo + hi) / 2;
    int eoff = mid * 12;
    int nameOff = rd32(g_L, eoff + 4);
    int cmp = 0, k = 0;
    while (k < namelen && cmp == 0) {
      cmp = (g_L[nameOff + k] & 0xff) - ((unsigned char)name[k] & 0xff);
      k++;
    }
    if (cmp == 0) cmp = (g_L[nameOff + namelen] & 0xff); /* terminador */
    if (cmp != 0) {
      if (cmp <= 0) lo = mid + 1;
      else hi = mid;
    } else {
      descOff = eoff + 8;
      break;
    }
  }
  if (descOff == 0) return NULL;
  int dataOff = rd32(g_L, descOff + 0);
  int dataLen = rd32(g_L, descOff + 4);
  unsigned char *raw = obb_read(dataOff, dataLen);
  if (!raw) return NULL;
  enc(raw, dataLen, (unsigned int)(dataOff + (int)ENC_KEY_CONST));
  unsigned char *dec = l_decompress(raw, dataLen, out_len);
  free(raw);
  return dec;
}

extern const char *ff4a_language_lproj(void); /* jni_shim.c */

unsigned char *obb_load_file(const char *path, int *out_len) {
  char buf[512];
  unsigned char *r;
  /* Idioma escolhido primeiro; en como rede de seguranca; files/ neutro. */
  snprintf(buf, sizeof(buf), "%s.lproj/%s", ff4a_language_lproj(), path);
  r = obb_load_exact(buf, out_len);
  if (r) return r;
  if (strcmp(ff4a_language_lproj(), "en") != 0) {
    snprintf(buf, sizeof(buf), "en.lproj/%s", path);
    r = obb_load_exact(buf, out_len);
    if (r) return r;
  }
  snprintf(buf, sizeof(buf), "files/%s", path); /* fallback neutro */
  return obb_load_exact(buf, out_len);
}
