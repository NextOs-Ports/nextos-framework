/* ab_apk.c — AAssetManager sobre o APK do dono (BYO), lido como ZIP.
 *
 * A Fusion só importa AAssetManager_fromJava / AAssetManager_open /
 * AAsset_getBuffer / AAsset_getLength64 / AAsset_close: acesso por BUFFER
 * inteiro, sem read/seek incremental. Então:
 *   - o APK é mapeado inteiro com mmap (PROT_READ, MAP_PRIVATE): o kernel
 *     pagina sob demanda e nada disso conta como heap;
 *   - 3.299 dos 3.521 assets estão STORED → o buffer devolvido aponta direto
 *     pro mmap, zero cópia e zero RAM extra;
 *   - os 222 DEFLATE são inflados com zlib só quando abertos.
 * Isso é o que mantém o residente confortável nos 916 MB do device.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "ab_port.h"
#include "ab_slingshot_adapter.inc"

typedef struct ZEntry {
  const char *name; /* aponta pro bloco de nomes */
  uint32_t name_len;
  uint32_t comp_size;
  uint32_t uncomp_size;
  uint32_t local_off;
  uint16_t method;
} ZEntry;

static int g_fd = -1;
static const unsigned char *g_map;
static size_t g_map_size;
static ZEntry *g_entries;
static size_t g_entry_count;
static char *g_names;
static ZEntry **g_hash;
static size_t g_hash_size;
static const unsigned char *g_slingshot_adapter;
static size_t g_slingshot_adapter_size;
static unsigned char *g_slingshot_adapter_override;

static int has_suffix(const char *text, const char *suffix) {
  size_t text_len, suffix_len;
  if (!text || !suffix)
    return 0;
  text_len = strlen(text);
  suffix_len = strlen(suffix);
  return text_len >= suffix_len &&
         memcmp(text + text_len - suffix_len, suffix, suffix_len) == 0;
}

/* The adapter is embedded so a normal build/deploy cannot silently lose the
 * approved slingshot behavior. An external copy is accepted only when the
 * developer explicitly sets AB_SLINGSHOT_ADAPTER; old test files on a device
 * therefore cannot shadow the release behavior. */
static void load_slingshot_adapter(void) {
  char path[1024];
  const char *override_path;
  FILE *stream;
  long size;

  g_slingshot_adapter = ab_slingshot_adapter_embedded;
  g_slingshot_adapter_size = ab_slingshot_adapter_embedded_len;
  override_path = getenv("AB_SLINGSHOT_ADAPTER");
  if (!override_path || !override_path[0]) {
    ab_log("[apk] adapter nativo embutido: %zu bytes",
           g_slingshot_adapter_size);
    return;
  }
  if (override_path[0] == '/')
    snprintf(path, sizeof(path), "%s", override_path);
  else
    snprintf(path, sizeof(path), "%s/%s", ab_gamedir(), override_path);
  stream = fopen(path, "rb");
  if (!stream) {
    ab_log("[apk] override Lua nao abriu; usando adapter embutido: %s", path);
    return;
  }
  if (fseek(stream, 0, SEEK_END) != 0 || (size = ftell(stream)) <= 0 ||
      size > 64 * 1024 || fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    ab_log("[apk] adapter Lua invalido: %s", path);
    return;
  }
  g_slingshot_adapter_override = malloc((size_t)size);
  if (!g_slingshot_adapter_override ||
      fread(g_slingshot_adapter_override, 1, (size_t)size, stream) !=
          (size_t)size) {
    free(g_slingshot_adapter_override);
    g_slingshot_adapter_override = NULL;
    fclose(stream);
    ab_log("[apk] nao foi possivel ler adapter Lua: %s", path);
    return;
  }
  fclose(stream);
  g_slingshot_adapter = g_slingshot_adapter_override;
  g_slingshot_adapter_size = (size_t)size;
  ab_log("[apk] adapter nativo externo: %zu bytes",
         g_slingshot_adapter_size);
}

static uint16_t rd16(const unsigned char *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint32_t name_hash(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 16777619u;
  }
  return h;
}

static void hash_insert(ZEntry *entry) {
  uint32_t h = name_hash(entry->name, entry->name_len);
  for (size_t i = 0; i < g_hash_size; i++) {
    size_t slot = (h + i) & (g_hash_size - 1);
    if (!g_hash[slot]) {
      g_hash[slot] = entry;
      return;
    }
  }
}

static ZEntry *hash_find(const char *name, size_t len) {
  uint32_t h;
  if (!g_hash)
    return NULL;
  h = name_hash(name, len);
  for (size_t i = 0; i < g_hash_size; i++) {
    size_t slot = (h + i) & (g_hash_size - 1);
    ZEntry *e = g_hash[slot];
    if (!e)
      return NULL;
    if (e->name_len == len && memcmp(e->name, name, len) == 0)
      return e;
  }
  return NULL;
}

int ab_apk_open(const char *path) {
  struct stat st;
  const unsigned char *eocd = NULL;
  uint32_t cd_off, cd_size;
  uint64_t entries;
  const unsigned char *p;
  size_t names_bytes = 0;
  char *names_cursor;

  g_fd = open(path, O_RDONLY);
  if (g_fd < 0) {
    ab_log("[apk] open falhou: %s", path);
    return -1;
  }
  if (fstat(g_fd, &st) != 0 || st.st_size < 22) {
    ab_log("[apk] fstat falhou");
    return -1;
  }
  g_map_size = (size_t)st.st_size;
  g_map = mmap(NULL, g_map_size, PROT_READ, MAP_PRIVATE, g_fd, 0);
  if (g_map == MAP_FAILED) {
    ab_log("[apk] mmap falhou");
    g_map = NULL;
    return -1;
  }

  /* EOCD: assinatura 0x06054b50 nos últimos 64 KiB + 22 B */
  {
    size_t max_back = g_map_size < 65557 ? g_map_size : 65557;
    for (size_t back = 22; back <= max_back; back++) {
      const unsigned char *cand = g_map + g_map_size - back;
      if (rd32(cand) == 0x06054b50u) {
        eocd = cand;
        break;
      }
    }
  }
  if (!eocd) {
    ab_log("[apk] EOCD nao encontrado");
    return -1;
  }
  entries = rd16(eocd + 10);
  cd_size = rd32(eocd + 12);
  cd_off = rd32(eocd + 16);
  if ((uint64_t)cd_off + cd_size > g_map_size) {
    ab_log("[apk] central directory fora do arquivo");
    return -1;
  }

  /* passo 1: medir os nomes */
  p = g_map + cd_off;
  for (uint64_t i = 0; i < entries; i++) {
    if (p + 46 > g_map + cd_off + cd_size || rd32(p) != 0x02014b50u)
      break;
    names_bytes += (size_t)rd16(p + 28) + 1;
    p += 46 + rd16(p + 28) + rd16(p + 30) + rd16(p + 32);
  }
  g_entries = calloc(entries ? entries : 1, sizeof(ZEntry));
  g_names = malloc(names_bytes ? names_bytes : 1);
  if (!g_entries || !g_names) {
    ab_log("[apk] sem memoria para o indice");
    return -1;
  }
  names_cursor = g_names;

  /* passo 2: preencher */
  p = g_map + cd_off;
  for (uint64_t i = 0; i < entries; i++) {
    uint16_t nlen, xlen, clen;
    if (p + 46 > g_map + cd_off + cd_size || rd32(p) != 0x02014b50u)
      break;
    nlen = rd16(p + 28);
    xlen = rd16(p + 30);
    clen = rd16(p + 32);
    ZEntry *e = &g_entries[g_entry_count];
    e->method = rd16(p + 10);
    e->comp_size = rd32(p + 20);
    e->uncomp_size = rd32(p + 24);
    e->local_off = rd32(p + 42);
    memcpy(names_cursor, p + 46, nlen);
    names_cursor[nlen] = 0;
    e->name = names_cursor;
    e->name_len = nlen;
    names_cursor += nlen + 1;
    g_entry_count++;
    p += 46 + nlen + xlen + clen;
  }

  g_hash_size = 1;
  while (g_hash_size < g_entry_count * 2)
    g_hash_size <<= 1;
  g_hash = calloc(g_hash_size, sizeof(ZEntry *));
  if (!g_hash) {
    ab_log("[apk] sem memoria para o hash");
    return -1;
  }
  for (size_t i = 0; i < g_entry_count; i++)
    hash_insert(&g_entries[i]);

  load_slingshot_adapter();

  ab_log("[apk] %s: %zu entradas, %zu bytes mapeados", path, g_entry_count,
         g_map_size);
  return 0;
}

void ab_apk_close(void) {
  if (g_map)
    munmap((void *)g_map, g_map_size);
  if (g_fd >= 0)
    close(g_fd);
  free(g_entries);
  free(g_names);
  free(g_hash);
  free(g_slingshot_adapter_override);
  g_map = NULL;
  g_fd = -1;
  g_entries = NULL;
  g_names = NULL;
  g_hash = NULL;
  g_slingshot_adapter = NULL;
  g_slingshot_adapter_override = NULL;
  g_slingshot_adapter_size = 0;
  g_entry_count = 0;
}

/* Início dos dados da entrada, pulando o local file header. */
static const unsigned char *entry_data(const ZEntry *e) {
  const unsigned char *lh = g_map + e->local_off;
  if (e->local_off + 30 > g_map_size || rd32(lh) != 0x04034b50u)
    return NULL;
  const unsigned char *data = lh + 30 + rd16(lh + 26) + rd16(lh + 28);
  if ((size_t)(data - g_map) + e->comp_size > g_map_size)
    return NULL;
  return data;
}

/* Normaliza "a//b" e "./b" — a engine concatena raiz + caminho e às vezes o
 * caminho pedido já começa com '/', produzindo barra dupla. */
static size_t normalize_into(char *out, size_t out_size, const char *prefix,
                             const char *name) {
  size_t len = 0;
  for (const char *s = prefix; *s && len + 1 < out_size; s++)
    out[len++] = *s;
  for (const char *s = name; *s && len + 1 < out_size; s++) {
    if (*s == '/' && len > 0 && out[len - 1] == '/')
      continue;
    out[len++] = *s;
  }
  out[len] = 0;
  return len;
}

static ZEntry *lookup_asset(const char *name) {
  char full[1024];
  size_t len;
  ZEntry *entry;
  if (!name)
    return NULL;
  /* The adapter first loads the untouched script under a private alias, then
   * wraps only its public SlingshotSystem methods. */
  if (has_suffix(name, "scripts/SlingshotNextOSOriginal.lua"))
    name = "data/scripts/Slingshot.lua";
  while (*name == '.' && name[1] == '/')
    name += 2;
  len = normalize_into(full, sizeof(full), "assets/", name);
  if (len == 0 || len + 1 >= sizeof(full))
    return NULL;
  entry = hash_find(full, len);
  if (entry)
    return entry;
  /* Alguns `require` do Lua chegam relativos à raiz de dados e não à raiz de
   * assets (ex.: "/IngameCurrency/X.lua" com script path "scripts"). O layout
   * real do APK põe tudo sob assets/data/, então tentamos esse prefixo. */
  len = normalize_into(full, sizeof(full), "assets/data/", name);
  if (len == 0 || len + 1 >= sizeof(full))
    return NULL;
  return hash_find(full, len);
}

int ab_apk_has(const char *name) { return lookup_asset(name) != NULL; }

/* Entrada CRUA do zip (caminho completo, ex. "lib/armeabi-v7a/libX.so").
 * Usado para carregar a .so do convidado direto do APK do dono, sem nunca
 * gravar dado de jogo em disco. */
void *ab_apk_entry(const char *zip_path, size_t *out_size) {
  ZEntry *e = zip_path ? hash_find(zip_path, strlen(zip_path)) : NULL;
  const unsigned char *data;
  if (out_size)
    *out_size = 0;
  if (!e)
    return NULL;
  data = entry_data(e);
  if (!data)
    return NULL;
  {
    unsigned char *out = malloc(e->uncomp_size ? e->uncomp_size : 1);
    if (!out)
      return NULL;
    if (e->method == 0) {
      memcpy(out, data, e->uncomp_size);
    } else if (e->method == 8) {
      z_stream zs;
      int rc;
      memset(&zs, 0, sizeof(zs));
      zs.next_in = (Bytef *)data;
      zs.avail_in = e->comp_size;
      zs.next_out = out;
      zs.avail_out = e->uncomp_size;
      if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        free(out);
        return NULL;
      }
      rc = inflate(&zs, Z_FINISH);
      inflateEnd(&zs);
      if (rc != Z_STREAM_END && rc != Z_OK) {
        free(out);
        return NULL;
      }
    } else {
      free(out);
      return NULL;
    }
    if (out_size)
      *out_size = e->uncomp_size;
    return out;
  }
}

const void *ab_apk_asset(const char *name, size_t *out_size, int *out_owned) {
  if (g_slingshot_adapter && name &&
      has_suffix(name, "scripts/Slingshot.lua")) {
    if (out_size)
      *out_size = g_slingshot_adapter_size;
    if (out_owned)
      *out_owned = 0;
    return g_slingshot_adapter;
  }
  ZEntry *e = lookup_asset(name);
  const unsigned char *data;
  if (out_size)
    *out_size = 0;
  if (out_owned)
    *out_owned = 0;
  if (!e)
    return NULL;
  data = entry_data(e);
  if (!data)
    return NULL;
  if (e->method == 0) {
    if (out_size)
      *out_size = e->uncomp_size;
    return data;
  }
  if (e->method != 8)
    return NULL;
  {
    unsigned char *out = malloc(e->uncomp_size ? e->uncomp_size : 1);
    z_stream zs;
    int rc;
    if (!out)
      return NULL;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = (Bytef *)data;
    zs.avail_in = e->comp_size;
    zs.next_out = out;
    zs.avail_out = e->uncomp_size;
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
      free(out);
      return NULL;
    }
    rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK) {
      free(out);
      return NULL;
    }
    if (out_size)
      *out_size = e->uncomp_size;
    if (out_owned)
      *out_owned = 1;
    return out;
  }
}
