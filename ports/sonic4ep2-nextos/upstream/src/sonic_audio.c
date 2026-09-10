#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mpg123.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <vorbis/vorbisfile.h>

#include "so_util.h"
#include "sonic_audio.h"

#define OUT_RATE 44100
#define OUT_CH 2
#define MUSIC_SLOTS 8
#define MAX_VOICES 32
#define MAX_CACHE 256
#define MAX_EXT_SFX_MAP 1024

typedef struct {
  int16_t *pcm;
  uint32_t frames;
} AudioBuffer;

typedef struct {
  char key[96];
  AudioBuffer *buf;
} CacheEntry;

typedef struct {
  char bank[32];
  char key[80];
  char file[144];
} ExtSfxMap;

typedef struct {
  char key[80];
  int handle;
  AudioBuffer *buf;
  uint32_t pos;
  float volume;
  int loop;
  int paused;
  int active;
} Voice;

typedef struct {
  char key[96];
  AudioBuffer *buf;
  uint32_t pos;
  uint32_t loop_start;
  uint32_t loop_end;
  float volume;
  int loop;
  int loop_explicit;
  int paused;
  int active;
  int jingle;
  int paused_by_jingle;
} MusicSlot;

/* ⚠️ ABI: o out-param de size do tsReadFile é `unsigned int*` (Pj) no v2 armv7 e
   `unsigned long*` (Pm) no v3 arm64 (8 bytes!). Usar `unsigned long` cobre os dois
   (no armv7 long = 32-bit) — passar &unsigned no arm64 corrompia a pilha. */
typedef int (*ts_read_alloc_t)(const char *, void **, unsigned long *);
typedef int (*ts_read_buf_t)(const char *, unsigned char *, unsigned long *);
typedef int (*ts_get_size_t)(const char *);
typedef void (*ts_unload_t)(void *);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_dev;
/* Plano C (volume): quando abrimos o card CRU (default/softvol falhou), os botoes
   de volume do device NAO chegam no nosso stream. No batocera/Knulli os botoes
   atualizam audio.volume (0-100) em /userdata/system/batocera.conf em tempo real
   -> uma thread le esse valor e aplica como ganho de software no callback. So
   engata com card cru (se o softvol abrir, fica OFF = sem dupla atenuacao). */
static int g_opened_raw = 0;
static volatile float g_sys_vol = 1.0f;
static int g_audio_init;
static int g_mpg_init;
static int g_next_handle = 1;
static Voice g_voices[MAX_VOICES];
static MusicSlot g_music[MUSIC_SLOTS];
static CacheEntry g_cache[MAX_CACHE];
static ExtSfxMap g_ext_sfx[MAX_EXT_SFX_MAP];
static int g_ext_sfx_count;
static int g_ext_sfx_loaded;
static char g_sfx_bank[32] = "ep2";

static ts_read_alloc_t g_ts_read_alloc;
static ts_read_buf_t g_ts_read_buf;
static ts_get_size_t g_ts_get_size;
static ts_unload_t g_ts_unload;
static int g_ts_resolved;

static int alog_enabled(void) {
  return getenv("SONIC_AUDIOLOG") != NULL;
}

static void alog(const char *fmt, ...) {
  if (!alog_enabled()) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static float clamp_volume(float v) {
  if (!(v >= 0.0f)) return 0.0f;
  if (v > 2.0f) return 2.0f;
  return v;
}

static int music_key_is_jingle(const char *key) {
  if (!key) return 0;
  return strstr(key, "_jin_") != NULL;
}

static uint32_t frames_from_ms(int ms) {
  if (ms <= 0) return 0;
  return (uint32_t)(((uint64_t)ms * OUT_RATE + 500) / 1000);
}

static int sfx_key_is_mechanical(const char *key) {
  if (!key) return 0;
  return strcasestr(key, "Rotary") || strcasestr(key, "Piller") ||
         strcasestr(key, "D_wall") || strcasestr(key, "Bl_land") ||
         strcasestr(key, "Pstand") || strcasestr(key, "Jetwall") ||
         strcasestr(key, "Shutter") || strcasestr(key, "MetalUnit") ||
         strcasestr(key, "Tomado") || strcasestr(key, "Propeller") ||
         strcasestr(key, "SandBranch") || strcasestr(key, "SandTrank") ||
         strcasestr(key, "Sandstorm") || strcasestr(key, "Oil") ||
         strcasestr(key, "Pipe") || strcasestr(key, "Piston") ||
         strcasestr(key, "Steam") || strcasestr(key, "Waterdash");
}

static int soft_limit_s16(int v) {
  const int limit = 28672; /* ~-1.2 dBFS: deixa folga antes do clamp real. */
  const int room = 32767 - limit;
  if (v > limit) {
    int over = v - limit;
    return limit + (int)(((int64_t)over * room) / (over + room));
  }
  if (v < -limit) {
    int over = -limit - v;
    return -limit - (int)(((int64_t)over * room) / (over + room));
  }
  return v;
}

static int any_active_jingle_locked(void) {
  for (int i = 0; i < MUSIC_SLOTS; i++)
    if (g_music[i].active && !g_music[i].paused && g_music[i].jingle)
      return 1;
  return 0;
}

static void pause_music_for_jingle_locked(int jingle_id) {
  for (int i = 0; i < MUSIC_SLOTS; i++) {
    if (i == jingle_id) continue;
    MusicSlot *m = &g_music[i];
    if (m->active && !m->paused && !m->jingle) {
      m->paused = 1;
      m->paused_by_jingle = 1;
    }
  }
}

static void resume_music_after_jingle_locked(void) {
  if (any_active_jingle_locked()) return;
  for (int i = 0; i < MUSIC_SLOTS; i++) {
    MusicSlot *m = &g_music[i];
    if (m->paused_by_jingle) {
      m->paused = 0;
      m->paused_by_jingle = 0;
    }
  }
}

static int16_t clamp_s16(int v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static void resolve_ts_symbols(void) {
  if (g_ts_resolved) return;
  /* v2 armv7 = ...Pj (uint*); v3 arm64 = ...Pm (ulong*). Tentar os dois. */
  g_ts_read_alloc =
      (ts_read_alloc_t)so_find_addr_safe("_Z10tsReadFilePKcPPvPj");
  if (!g_ts_read_alloc)
    g_ts_read_alloc =
        (ts_read_alloc_t)so_find_addr_safe("_Z10tsReadFilePKcPPvPm");
  g_ts_read_buf =
      (ts_read_buf_t)so_find_addr_safe("_Z10tsReadFilePKcPhPj");
  if (!g_ts_read_buf)
    g_ts_read_buf =
        (ts_read_buf_t)so_find_addr_safe("_Z10tsReadFilePKcPhPm");
  g_ts_get_size =
      (ts_get_size_t)so_find_addr_safe("_Z13tsGetFileSizePKc");
  g_ts_unload =
      (ts_unload_t)so_find_addr_safe("_Z15tsUnloadFileBufPv");
  g_ts_resolved = 1;
  alog("sonic_audio: tsRead alloc=%p buf=%p getsize=%p unload=%p\n",
       (void *)g_ts_read_alloc, (void *)g_ts_read_buf,
       (void *)g_ts_get_size, (void *)g_ts_unload);
}

static void free_lpk_buf(void *p, int engine_owned) {
  if (!p) return;
  if (engine_owned && g_ts_unload) g_ts_unload(p);
  else free(p);
}

static int read_lpk_file(const char *path, unsigned char **out, size_t *out_size,
                         int *engine_owned) {
  *out = NULL;
  *out_size = 0;
  *engine_owned = 0;
  resolve_ts_symbols();

  if (g_ts_read_alloc) {
    void *ptr = NULL;
    unsigned long size = 0;
    int ret = g_ts_read_alloc(path, &ptr, &size);
    if (ptr && size > 0) {
      *out = (unsigned char *)ptr;
      *out_size = (size_t)size;
      *engine_owned = 1;
      alog("sonic_audio: LPK read alloc ok path=\"%s\" size=%lu ret=%d\n",
           path, size, ret);
      return 1;
    }
  }

  if (g_ts_get_size && g_ts_read_buf) {
    int size = g_ts_get_size(path);
    if (size > 0 && size < 256 * 1024 * 1024) {
      unsigned char *buf = malloc((size_t)size);
      if (!buf) return 0;
      unsigned long got = (unsigned long)size;
      int ret = g_ts_read_buf(path, buf, &got);
      if (got > 0) {
        *out = buf;
        *out_size = (size_t)got;
        alog("sonic_audio: LPK read buf ok path=\"%s\" size=%lu ret=%d\n",
             path, got, ret);
        return 1;
      }
      free(buf);
    }
  }

  alog("sonic_audio: LPK miss path=\"%s\"\n", path);
  return 0;
}

static int append_bytes(unsigned char **data, size_t *len, size_t *cap,
                        const void *src, size_t n) {
  if (n == 0) return 1;
  if (*len > SIZE_MAX - n) return 0;
  size_t need = *len + n;
  if (need > *cap) {
    size_t nc = *cap ? *cap * 2 : 65536;
    while (nc < need) {
      if (nc > SIZE_MAX / 2) return 0;
      nc *= 2;
    }
    unsigned char *p = realloc(*data, nc);
    if (!p) return 0;
    *data = p;
    *cap = nc;
  }
  memcpy(*data + *len, src, n);
  *len += n;
  return 1;
}

static AudioBuffer *buffer_from_s16(const int16_t *src, uint32_t frames,
                                    int rate, int channels) {
  if (!src || frames == 0 || channels <= 0) return NULL;
  if (rate <= 0) return NULL;
  uint64_t out_frames64 = ((uint64_t)frames * OUT_RATE + (rate / 2)) / rate;
  if (out_frames64 == 0 || out_frames64 > UINT32_MAX) return NULL;

  AudioBuffer *b = calloc(1, sizeof(*b));
  if (!b) return NULL;
  b->frames = (uint32_t)out_frames64;
  b->pcm = malloc((size_t)b->frames * OUT_CH * sizeof(int16_t));
  if (!b->pcm) {
    free(b);
    return NULL;
  }

  if (rate == OUT_RATE && channels == OUT_CH) {
    memcpy(b->pcm, src, (size_t)b->frames * OUT_CH * sizeof(int16_t));
    return b;
  }

  for (uint32_t f = 0; f < b->frames; f++) {
    uint64_t src_fp = ((uint64_t)f * (uint64_t)rate << 16) / OUT_RATE;
    uint32_t idx = (uint32_t)(src_fp >> 16);
    uint32_t frac = (uint32_t)(src_fp & 0xffff);
    if (idx >= frames) idx = frames - 1;
    uint32_t next = idx + 1 < frames ? idx + 1 : idx;

    int l0, r0, l1, r1;
    if (channels == 1) {
      l0 = r0 = src[idx];
      l1 = r1 = src[next];
    } else {
      l0 = src[idx * channels + 0];
      r0 = src[idx * channels + 1];
      l1 = src[next * channels + 0];
      r1 = src[next * channels + 1];
    }

    int l = l0 + (int)(((int64_t)(l1 - l0) * frac) >> 16);
    int r = r0 + (int)(((int64_t)(r1 - r0) * frac) >> 16);
    b->pcm[f * 2 + 0] = clamp_s16(l);
    b->pcm[f * 2 + 1] = clamp_s16(r);
  }
  return b;
}

static void free_buffer(AudioBuffer *b) {
  if (!b) return;
  free(b->pcm);
  free(b);
}

static AudioBuffer *decode_mp3(const unsigned char *data, size_t size,
                               const char *label) {
  if (!g_mpg_init) {
    if (mpg123_init() != MPG123_OK) {
      alog("sonic_audio: mpg123_init failed\n");
      return NULL;
    }
    g_mpg_init = 1;
  }

  int err = MPG123_OK;
  mpg123_handle *mh = mpg123_new(NULL, &err);
  if (!mh) {
    alog("sonic_audio: mpg123_new failed err=%d\n", err);
    return NULL;
  }
  mpg123_format_none(mh);
  mpg123_format(mh, OUT_RATE, MPG123_STEREO, MPG123_ENC_SIGNED_16);
  if (mpg123_open_feed(mh) != MPG123_OK ||
      mpg123_feed(mh, data, size) != MPG123_OK) {
    alog("sonic_audio: mpg123 feed failed for %s: %s\n",
         label, mpg123_strerror(mh));
    mpg123_delete(mh);
    return NULL;
  }

  unsigned char *pcm = NULL;
  size_t len = 0, cap = 0;
  unsigned char tmp[32768];
  for (;;) {
    size_t done = 0;
    int r = mpg123_read(mh, tmp, sizeof(tmp), &done);
    if (done > 0 && !append_bytes(&pcm, &len, &cap, tmp, done)) {
      free(pcm);
      mpg123_delete(mh);
      return NULL;
    }
    if (r == MPG123_NEW_FORMAT) continue;
    if (r == MPG123_OK) continue;
    if (r == MPG123_DONE || r == MPG123_NEED_MORE) break;
    alog("sonic_audio: mpg123 read %s stopped r=%d err=%s\n",
         label, r, mpg123_strerror(mh));
    break;
  }
  mpg123_delete(mh);

  uint32_t frames = (uint32_t)(len / (OUT_CH * sizeof(int16_t)));
  AudioBuffer *b = buffer_from_s16((const int16_t *)pcm, frames, OUT_RATE, OUT_CH);
  free(pcm);
  if (b) {
    alog("sonic_audio: decoded MP3 %s frames=%u seconds=%.2f\n",
         label, b->frames, (double)b->frames / OUT_RATE);
  } else {
    alog("sonic_audio: decode MP3 failed %s len=%zu\n", label, len);
  }
  return b;
}

typedef struct {
  const unsigned char *data;
  size_t size;
  size_t pos;
} MemFile;

static size_t mem_read_cb(void *ptr, size_t size, size_t nmemb, void *ds) {
  MemFile *m = (MemFile *)ds;
  size_t want = size * nmemb;
  size_t left = m->size - m->pos;
  if (want > left) want = left;
  if (want > 0) {
    memcpy(ptr, m->data + m->pos, want);
    m->pos += want;
  }
  return size ? want / size : 0;
}

static int mem_seek_cb(void *ds, ogg_int64_t offset, int whence) {
  MemFile *m = (MemFile *)ds;
  ogg_int64_t base = 0;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = (ogg_int64_t)m->pos;
  else if (whence == SEEK_END) base = (ogg_int64_t)m->size;
  else return -1;
  ogg_int64_t np = base + offset;
  if (np < 0 || (uint64_t)np > m->size) return -1;
  m->pos = (size_t)np;
  return 0;
}

static long mem_tell_cb(void *ds) {
  MemFile *m = (MemFile *)ds;
  return (long)m->pos;
}

static AudioBuffer *decode_ogg(const unsigned char *data, size_t size,
                               const char *label) {
  MemFile mf = {data, size, 0};
  ov_callbacks cb;
  cb.read_func = mem_read_cb;
  cb.seek_func = mem_seek_cb;
  cb.close_func = NULL;
  cb.tell_func = mem_tell_cb;

  OggVorbis_File vf;
  memset(&vf, 0, sizeof(vf));
  if (ov_open_callbacks(&mf, &vf, NULL, 0, cb) < 0) {
    alog("sonic_audio: ov_open failed %s\n", label);
    return NULL;
  }

  vorbis_info *vi = ov_info(&vf, -1);
  if (!vi || vi->channels <= 0 || vi->rate <= 0) {
    ov_clear(&vf);
    return NULL;
  }

  unsigned char *pcm = NULL;
  size_t len = 0, cap = 0;
  char tmp[32768];
  int bitstream = 0;
  for (;;) {
    long r = ov_read(&vf, tmp, sizeof(tmp), 0, 2, 1, &bitstream);
    if (r > 0) {
      if (!append_bytes(&pcm, &len, &cap, tmp, (size_t)r)) {
        free(pcm);
        ov_clear(&vf);
        return NULL;
      }
      continue;
    }
    if (r == 0) break;
    alog("sonic_audio: ov_read error %s r=%ld\n", label, r);
    break;
  }

  int channels = vi->channels;
  int rate = vi->rate;
  ov_clear(&vf);

  uint32_t frames = (uint32_t)(len / ((size_t)channels * sizeof(int16_t)));
  AudioBuffer *b = buffer_from_s16((const int16_t *)pcm, frames, rate, channels);
  free(pcm);
  if (b) {
    alog("sonic_audio: decoded OGG %s src=%dHz/%dch frames=%u seconds=%.2f\n",
         label, rate, channels, b->frames, (double)b->frames / OUT_RATE);
  } else {
    alog("sonic_audio: decode OGG failed %s len=%zu\n", label, len);
  }
  return b;
}

static const struct {
  const char *key;
  const char *file;
  int loop;
  int loop_start_ms;
  int loop_end_ms;
} g_music_map[] = {
  {"ep2_sng_title", "MIXED00_EP2_TITLE.MP3", 0, 0, 0},
  {"ep2_sng_menu", "SNG01_EP2_SNG_MENU_V00_EP1.MP3", 1, 3310, 29793},
  {"ep2_sng_worldmap", "MIXED02_EP2_WORLDMaP.MP3", 1, 5818, 23272},
  {"ep2_sng_z1a1", "MIXED03_EP2_Z1A1.MP3", 1, 6400, 59200},
  {"ep2_sng_z1a2", "MIXED04_EP2_Z1A2.MP3", 1, 6857, 62571},
  {"ep2_sng_z1a3", "MIXED05_EP2_Z1A3.MP3", 1, 3636, 32727},
  {"ep2_sng_z2a1", "MIXED06_EP2_Z2A1.MP3", 1, 3777, 48222},
  {"ep2_sng_z2a2", "MIXED07_EP2_Z2A2.MP3", 1, 6000, 54000},
  {"ep2_sng_z2a3", "MIXED08_EP2_Z2A3.MP3", 1, 3840, 28800},
  {"ep2_sng_z3a1", "MIXED09_EP2_Z3A1.MP3", 1, 6956, 45217},
  {"ep2_sng_z3a2", "MIXED10_EP2_Z3A2.MP3", 1, 8307, 53538},
  {"ep2_sng_z3a3", "MIXED11_EP2_Z3A3.MP3", 1, 3478, 46086},
  {"ep2_sng_z4a1", "MIXED12_EP2_Z4A1.MP3", 1, 6901, 34507},
  {"ep2_sng_z4a2", "MIXED13_EP2_Z4A2.MP3", 1, 6000, 67500},
  {"ep2_sng_z4a3", "MIXED14_EP2_Z4A3.MP3", 1, 6800, 72400},
  {"ep2_sng_z1a1_speedup", "MIXED15_EP2_Z1A1_SPDUP.MP3", 1, 16118, 68918},
  {"ep2_sng_z1a2_speedup", "MIXED16_EP2_Z1A2_SPDUP.MP3", 1, 17243, 72957},
  {"ep2_sng_z1a3_speedup", "MIXED17_EP2_Z1A3_SPDUP.MP3", 1, 18287, 47378},
  {"ep2_sng_z2a1_speedup", "MIXED06_EP2_Z2A1.MP3", 1, 3777, 48222},
  {"ep2_sng_z2a2_speedup", "MIXED07_EP2_Z2A2.MP3", 1, 6000, 54000},
  {"ep2_sng_z2a3_speedup", "MIXED08_EP2_Z2A3.MP3", 1, 3840, 28800},
  {"ep2_sng_z3a1_speedup", "MIXED21_EP2_Z3A1_SPDUP.MP3", 1, 17957, 56218},
  {"ep2_sng_z3a2_speedup", "MIXED22_EP2_Z3A2_SPDUP.MP3", 1, 17118, 62349},
  {"ep2_sng_z3a3_speedup", "MIXED11_EP2_Z3A3.MP3", 1, 3478, 46086},
  {"ep2_sng_z4a1_speedup", "MIXED12_EP2_Z4A1.MP3", 1, 6901, 34507},
  {"ep2_sng_z4a2_speedup", "MIXED25_EP2_Z4A2_SPDUP.MP3", 1, 17067, 78567},
  {"ep2_sng_z4a3_speedup", "MIXED14_EP2_Z4A3.MP3", 1, 6800, 72400},
  {"ep2_sng_zfa1_speedup", "MIXED66_EP2_FINaL_SPDUP.MP3", 1, 16083, 52083},
  {"ep2_sng_boss1", "MIXED27_EP2_EGGMaN.MP3", 1, 8269, 26097},
  {"ep2_sng_boss2", "MIXED28_EP2_FINaLA1.MP3", 1, 18000, 54000},
  {"ep2_sng_boss3", "MIXED29_EP2_METaLSONIC.MP3", 1, 6171, 39085},
  {"ep2_sng_boss5", "MIXED31_EP2_STaRDUSTSPDWY.MP3", 1, 7196, 71962},
  {"ep2_sng_boss6", "MIXED32_EP2_DEMK2.MP3", 1, 6315, 58421},
  {"ep2_sng_special", "MIXED33_EP2_SPECIaLSTaGE.MP3", 1, 4114, 56228},
  {"ep2_sng_special2", "MIXED33_EP2_SPECIaLSTaGE.MP3", 1, 4114, 56228},
  {"ep2_sng_special3", "MIXED33_EP2_SPECIaLSTaGE.MP3", 1, 4114, 56228},
  {"ep2_sng_endroll", "MIXED34_EP2_ENDROLL.MP3", 0, 0, 0},
  {"ep2_sng_actclear", "MIXED35_EP2_ACTCLEaR.MP3", 0, 0, 0},
  {"snd_sng_boss2", "MIXED29_EP2_METaLSONIC.MP3", 1, 6171, 39085},
  {"snd_sng_final", "MIXED28_EP2_FINaLA1.MP3", 1, 18000, 54000},
  {"ep2_jin_clear", "MIXED35_EP2_ACTCLEaR.MP3", 0, 0, 0},
  {"ep2_jin_clear_final", "MIXED35_EP2_ACTCLEaR.MP3", 0, 0, 0},
  {"ep2_jin_emerald", "SNG37_EP2_JIN_EMERaLD_V00_EP1.MP3", 0, 0, 0},
  {"ep2_jin_timer", "SNG38_EP2_JIN_TIMER_V00_EP1.MP3", 0, 0, 0},
  {"ep2_jin_invincible", "SNG39_EP2_JIN_INVINCIBLE_V00_EP1.MP3", 1, 6552, 22935},
  {"ep2_jin_supersonic", "SNG40_EP2_JIN_SUPERSONIC_V00_EP1.MP3", 1, 2571, 12857},
  {"ep2_jin_1up", "SNG41_EP2_JIN_1UP_V00_EP1.MP3", 0, 0, 0},
  {"ep2_jin_new_record", "SNG42_EP2_JIN_NEW_RECORD_V00_EP1.MP3", 0, 0, 0},
  {"ep2_jin_gameover", "SNG43_EP2_JIN_GaMEOVER_V00_EP1.MP3", 0, 0, 0},
  {"ep1_sng_z1a1", "SNG47_EP1_SNG_z1a1_V00_EP1.MP3", 1, 6075, 41007},
  {"ep1_sng_z1a1_speedup", "SNG48_EP1_SNG_z1a1_SPEEDUP_V00_EP1.MP3", 1, 16875, 51815},
  {"ep1_sng_z2a1", "SNG49_EP1_SNG_z2a1_V00_EP1.MP3", 1, 4664, 55754},
  {"ep1_sng_z2a1_speedup", "SNG50_EP1_SNG_z2a1_SPEEDUP_V00_EP1.MP3", 1, 16976, 68073},
  {"ep1_sng_z3a1", "SNG51_EP1_SNG_z3a1_V00_EP1.MP3", 1, 2799, 50799},
  {"ep1_sng_z3a1_speedup", "SNG52_EP1_SNG_z3a1_SPEEDUP_V00_EP1.MP3", 1, 16227, 64231},
  {"ep1_sng_z4a1", "SNG53_EP1_SNG_z4a1_V00_EP1.MP3", 1, 3465, 41927},
  {"ep1_sng_z4a1_speedup", "SNG54_EP1_SNG_z4a1_SPEEDUP_V00_EP1.MP3", 1, 21685, 60151},
};

static const struct {
  const char *key;
  const char *file;
} g_sfx_map[] = {
  {"Ok", "S4EP2FX_001_SHSY08_22.OGG"},
  {"Cancel", "S4EP2FX_002_SHSY09_22.OGG"},
  {"Pause", "S4EP2FX_004_SHSY10_22.OGG"},
  {"Special_1up", "S4EP2FX_008_S1BF_44.OGG"},
  {"Jump", "S4EP2FX_009_SK62_44.OGG"},
  {"Ring1", "S4EP2FX_011_SK33_44.OGG"},
  {"Ring2", "S4EP2FX_012_SKB9_44.OGG"},
  {"Damage1", "S4EP2FX_013_S1A3_44.OGG"},
  {"Damage3", "S4EP2FX_014_S1B2_44.OGG"},
  {"Homing", "S4EP2FX_015_SHPLSP01_22.OGG"},
  {"Transform", "S4EP2FX_016a_S2_645F_44.OGG"},
  {"Enemy", "S4EP2FX_017a_S2_3441_44.OGG"},
  {"Barrier", "S4EP2FX_018_SK3A_44.OGG"},
  {"Spin", "S4EP2FX_019_SKAB_44.OGG"},
  {"Dash1", "S4EP2FX_020_SKB6_44.OGG"},
  {"Dash2", "S4EP2FX_021_SK3C_44.OGG"},
  {"Damage2", "S4EP2FX_022_S1A6_44.OGG"},
  {"Attention", "S4EP2FX_024_S1C2_44.OGG"},
  {"Breathe", "S4EP2FX_025_S1AD_44.OGG"},
  {"Spring", "S4EP2FX_067_SKB1_44.OGG"},
  {"DashPanel", "S4EP2FX_069_SHCN000B_22.OGG"},
  {"BreakGround", "S4EP2FX_070_S1B9_44.OGG"},
  {"Catapult", "S4EP2FX_084a_S2_6762_44_CaSINO_CaTaPULT.OGG"},
  {"Ring1L", "S4EP2FX_112a_S2_2235_44L.OGG"},
  {"Ring1R", "S4EP2FX_113a_S2_2235_44R.OGG"},
  {"Catapult1", "S4EP2FX_120a_SKA4_EDIT_44LP.OGG"},
  {"LockedOn", "S4EP2FX_144a_COLORS_OBJ_LOCKON_22.OGG"},
  {"TlsScrew", "S4EP2FX_151_V2_1208.OGG"},
  {"TlsProp", "S4EP2FX_151X_S3_D1.OGG"},
  {"Waterdash01", "S4EP2FX_158X_S2_81_70_1210V3.OGG"},
  {"Coop01", "S4EP2FX_202_332_V1_1028.OGG"},
  {"Coop02", "S4EP2FX_203_V2_1207.OGG"},
  {"Coop04", "S4EP2FX_205_V1_1201_COOP_SPIN_ST.OGG"},
  {"Coop05", "S4EP2FX_206_V6_1225.OGG"},
  {"Double01", "S4EP2FX_207_V2_1217.OGG"},
  {"Double02", "S4EP2FX_208_V2_1207.OGG"},
  {"Double03", "S4EP2FX_209_V2_1217.OGG"},
  {"MS_Jump", "S4EP2FX_210_METaLSONIC_JUMP_44.OGG"},
  {"MS_Ring2", "S4EP2FX_212_V1_1028.OGG"},
  {"MS_Burner1", "S4EP2FX_219_V2_HEaD.OGG"},
  {"MS_Burner2", "S4EP2FX_220_V1_1028.OGG"},
  {"LightRing01", "S4EP2FX_246_V1_1117.OGG"},
  {"LightRing02", "S4EP2FX_247_V4_1221.OGG"},
  {"LightRing03", "S4EP2FX_248_V1_1028.OGG"},
  {"ItemBox_Dbl", "S4EP2FX_266_V2 1210.OGG"},
  {"RedStar01", "S4EP2FX_378_V1_1028.OGG"},
  {"Double04", "S4EP2FX_381_V1_1210.OGG"},
  {"Double05", "S4EP2FX_382_1_V1_1210.OGG"},
  {"Double06", "S4EP2FX_383_V1_1210.OGG"},
  {"RingGate", "S4EP2FX_403_V1_1229.OGG"},
};

static char *trim_field(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  char *e = s + strlen(s);
  while (e > s &&
         (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    *--e = 0;
  return s;
}

static void normalize_sfx_bank(const char *src, char out[32]) {
  const char *bank = "ep2";
  if (src && *src) {
    if (strcasestr(src, "ep2zone1")) bank = "ep2zone1";
    else if (strcasestr(src, "ep2zone2")) bank = "ep2zone2";
    else if (strcasestr(src, "ep2zone3")) bank = "ep2zone3";
    else if (strcasestr(src, "ep2zone4")) bank = "ep2zone4";
    else if (strcasestr(src, "ep2zonef")) bank = "ep2zonef";
    else if (strcasestr(src, "ep1")) bank = "ep1";
    else if (strcasestr(src, "ep2")) bank = "ep2";
    else bank = src;
  }
  strncpy(out, bank, 31);
  out[31] = 0;
}

static void load_external_sfx_file(const char *path) {
  if (!path || !*path || g_ext_sfx_count >= MAX_EXT_SFX_MAP) return;
  FILE *fp = fopen(path, "r");
  if (!fp) return;

  char line[512];
  int before = g_ext_sfx_count;
  while (fgets(line, sizeof(line), fp) &&
         g_ext_sfx_count < MAX_EXT_SFX_MAP) {
    char *p = trim_field(line);
    if (!*p || *p == '#') continue;

    char *save = NULL;
    char *bank = strtok_r(p, "\t", &save);
    char *key = strtok_r(NULL, "\t", &save);
    char *file = strtok_r(NULL, "\r\n", &save);
    if (!bank || !key || !file) continue;
    bank = trim_field(bank);
    key = trim_field(key);
    file = trim_field(file);
    if (!*bank || !*key || !*file) continue;

    ExtSfxMap *m = &g_ext_sfx[g_ext_sfx_count++];
    strncpy(m->bank, bank, sizeof(m->bank) - 1);
    strncpy(m->key, key, sizeof(m->key) - 1);
    strncpy(m->file, file, sizeof(m->file) - 1);
  }
  fclose(fp);

  if (g_ext_sfx_count > before)
    alog("sonic_audio: loaded %d external sfx map rows from %s\n",
         g_ext_sfx_count - before, path);
}

static void load_external_sfx_map(void) {
  if (g_ext_sfx_loaded) return;
  g_ext_sfx_loaded = 1;

  const char *env = getenv("SONIC_SFX_MAP");
  if (env) load_external_sfx_file(env);

  const char *dir = getenv("SONIC_DATADIR");
  if (dir && *dir && g_ext_sfx_count < MAX_EXT_SFX_MAP) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/sfx_map.tsv", dir);
    load_external_sfx_file(path);
  }

  if (g_ext_sfx_count == 0)
    load_external_sfx_file("sfx_map.tsv");
}

static const char *find_external_sfx_in_bank(const char *bank,
                                             const char *key) {
  if (!bank || !*bank || !key || !*key) return NULL;
  for (int i = 0; i < g_ext_sfx_count; i++) {
    if (strcmp(g_ext_sfx[i].bank, bank) == 0 &&
        strcmp(g_ext_sfx[i].key, key) == 0)
      return g_ext_sfx[i].file;
  }
  return NULL;
}

static const char *map_external_sfx_file(const char *key) {
  load_external_sfx_map();
  if (!g_ext_sfx_count) return NULL;

  char bank[32];
  pthread_mutex_lock(&g_lock);
  strncpy(bank, g_sfx_bank, sizeof(bank) - 1);
  bank[sizeof(bank) - 1] = 0;
  pthread_mutex_unlock(&g_lock);

  const char *file = find_external_sfx_in_bank(bank, key);
  if (file) return file;
  if (strcmp(bank, "ep2") != 0) {
    file = find_external_sfx_in_bank("ep2", key);
    if (file) return file;
  }
  file = find_external_sfx_in_bank("default", key);
  if (file) return file;
  return find_external_sfx_in_bank("ep1", key);
}

void sonic_audio_set_sfx_bank(const char *bank) {
  char normalized[32];
  normalize_sfx_bank(bank, normalized);
  pthread_mutex_lock(&g_lock);
  if (strcmp(g_sfx_bank, normalized) != 0) {
    strncpy(g_sfx_bank, normalized, sizeof(g_sfx_bank) - 1);
    g_sfx_bank[sizeof(g_sfx_bank) - 1] = 0;
    pthread_mutex_unlock(&g_lock);
    alog("sonic_audio: sfx bank=\"%s\" source=\"%s\"\n",
         normalized, bank ? bank : "");
    return;
  }
  pthread_mutex_unlock(&g_lock);
}

static const char *map_override_file(const char *env_name, const char *key) {
  static char value[160];
  const char *env = getenv(env_name);
  if (!env || !key || !*key) return NULL;
  size_t key_len = strlen(key);
  const char *p = env;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
    if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
      p += key_len + 1;
      size_t n = 0;
      while (p[n] && p[n] != ',' && p[n] != ';') n++;
      while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
      if (n >= sizeof(value)) n = sizeof(value) - 1;
      memcpy(value, p, n);
      value[n] = 0;
      if (value[0]) {
        alog("sonic_audio: override %s key=\"%s\" file=\"%s\"\n",
             env_name, key, value);
        return value;
      }
    }
    while (*p && *p != ',' && *p != ';') p++;
  }
  return NULL;
}

static const typeof(g_music_map[0]) *find_music_info(const char *key) {
  for (size_t i = 0; i < sizeof(g_music_map) / sizeof(g_music_map[0]); i++)
    if (strcmp(key, g_music_map[i].key) == 0) return &g_music_map[i];
  return NULL;
}

static const char *map_music_file(const char *key) {
  const typeof(g_music_map[0]) *info = find_music_info(key);
  return info ? info->file : NULL;
}

static int music_key_default_loop(const char *key) {
  const typeof(g_music_map[0]) *info = find_music_info(key);
  if (info) return info->loop != 0;
  return !music_key_is_jingle(key);
}

static int music_key_allows_loop(const char *key) {
  const typeof(g_music_map[0]) *info = find_music_info(key);
  if (info) return info->loop != 0;
  return !music_key_is_jingle(key);
}

static void music_key_loop_points(const char *key, uint32_t *start,
                                  uint32_t *end) {
  const typeof(g_music_map[0]) *info = find_music_info(key);
  *start = info ? frames_from_ms(info->loop_start_ms) : 0;
  *end = info ? frames_from_ms(info->loop_end_ms) : 0;
}

static const char *map_sfx_file(const char *key) {
  const char *override = map_override_file("SONIC_SFX_OVERRIDE", key);
  if (override) return override;
  const char *external = map_external_sfx_file(key);
  if (external) return external;
  for (size_t i = 0; i < sizeof(g_sfx_map) / sizeof(g_sfx_map[0]); i++)
    if (strcmp(key, g_sfx_map[i].key) == 0) return g_sfx_map[i].file;
  return NULL;
}

static AudioBuffer *cache_find(const char *cache_key) {
  for (int i = 0; i < MAX_CACHE; i++)
    if (g_cache[i].buf && strcmp(g_cache[i].key, cache_key) == 0)
      return g_cache[i].buf;
  return NULL;
}

static void cache_put(const char *cache_key, AudioBuffer *buf) {
  if (!buf) return;
  for (int i = 0; i < MAX_CACHE; i++) {
    if (!g_cache[i].buf) {
      strncpy(g_cache[i].key, cache_key, sizeof(g_cache[i].key) - 1);
      g_cache[i].buf = buf;
      return;
    }
  }
  alog("sonic_audio: cache full, dropping %s\n", cache_key);
  free_buffer(buf);
}

static AudioBuffer *load_audio_candidates(const char *cache_key,
                                          const char **paths,
                                          int is_mp3) {
  pthread_mutex_lock(&g_lock);
  AudioBuffer *cached = cache_find(cache_key);
  pthread_mutex_unlock(&g_lock);
  if (cached) return cached;

  for (int i = 0; paths[i]; i++) {
    unsigned char *data = NULL;
    size_t size = 0;
    int engine_owned = 0;
    if (!read_lpk_file(paths[i], &data, &size, &engine_owned)) continue;
    AudioBuffer *buf = is_mp3 ? decode_mp3(data, size, paths[i])
                              : decode_ogg(data, size, paths[i]);
    free_lpk_buf(data, engine_owned);
    if (buf) {
      pthread_mutex_lock(&g_lock);
      AudioBuffer *again = cache_find(cache_key);
      if (again) {
        pthread_mutex_unlock(&g_lock);
        free_buffer(buf);
        return again;
      }
      cache_put(cache_key, buf);
      pthread_mutex_unlock(&g_lock);
      return buf;
    }
  }
  return NULL;
}

static AudioBuffer *load_music(const char *key) {
  const char *file = map_music_file(key);
  if (!file) {
    alog("sonic_audio: unmapped music key=\"%s\"\n", key ? key : "");
    return NULL;
  }
  char cache_key[128], p0[160], p1[160], p2[160];
  snprintf(cache_key, sizeof(cache_key), "M:%s", key);
  snprintf(p0, sizeof(p0), "SOUND/MUSIC/%s", file);
  snprintf(p1, sizeof(p1), "SOUND/%s", file);
  snprintf(p2, sizeof(p2), "%s", file);
  const char *paths[] = {p0, p1, p2, NULL};
  return load_audio_candidates(cache_key, paths, 1);
}

static AudioBuffer *load_sfx(const char *key) {
  const char *file = map_sfx_file(key);
  if (!file) {
    alog("sonic_audio: unmapped sfx key=\"%s\"\n", key ? key : "");
    return NULL;
  }
  char cache_key[256], p0[160], p1[160], p2[160];
  snprintf(cache_key, sizeof(cache_key), "S:%s:%s", key, file);
  snprintf(p0, sizeof(p0), "SOUND/SFX/%s", file);
  snprintf(p1, sizeof(p1), "SOUND/%s", file);
  snprintf(p2, sizeof(p2), "%s", file);
  const char *paths[] = {p0, p1, p2, NULL};
  return load_audio_candidates(cache_key, paths, 0);
}

static void mix_buffer(int32_t *mix, int frames, AudioBuffer *buf,
                       uint32_t *pos, float volume, int loop,
                       uint32_t loop_start, uint32_t loop_end, int *active) {
  if (!buf || !buf->pcm || !active || !*active || volume <= 0.0f) return;
  uint32_t p = *pos;
  uint32_t ls = loop_start;
  uint32_t le = loop_end;
  if (le == 0 || le > buf->frames) le = buf->frames;
  if (ls >= le) ls = 0;
  for (int f = 0; f < frames; f++) {
    if (loop && p >= le) {
      uint32_t span = le - ls;
      p = span ? ls + ((p - ls) % span) : 0;
    }
    if (p >= buf->frames) {
      if (loop) p = ls;
      else {
        *active = 0;
        break;
      }
    }
    int l = buf->pcm[p * 2 + 0];
    int r = buf->pcm[p * 2 + 1];
    mix[f * 2 + 0] += (int32_t)(l * volume);
    mix[f * 2 + 1] += (int32_t)(r * volume);
    p++;
  }
  *pos = p;
}

static void sdl_audio_cb(void *ud, Uint8 *stream, int len) {
  (void)ud;
  memset(stream, 0, len);
  int frames = len / (OUT_CH * (int)sizeof(int16_t));
  if (frames <= 0) return;
  if (frames > 4096) frames = 4096;

  int32_t mix[4096 * OUT_CH];
  memset(mix, 0, (size_t)frames * OUT_CH * sizeof(mix[0]));

  pthread_mutex_lock(&g_lock);
  int ended_jingle = 0;
  for (int i = 0; i < MUSIC_SLOTS; i++) {
    MusicSlot *m = &g_music[i];
    if (m->active && !m->paused) {
      int was_jingle = m->jingle;
      mix_buffer(mix, frames, m->buf, &m->pos, m->volume, m->loop,
                 m->loop_start, m->loop_end, &m->active);
      if (was_jingle && !m->active) {
        m->pos = 0;
        m->jingle = 0;
        m->paused_by_jingle = 0;
        ended_jingle = 1;
      }
    }
  }
  if (ended_jingle) resume_music_after_jingle_locked();
  for (int i = 0; i < MAX_VOICES; i++) {
    Voice *v = &g_voices[i];
    if (v->active && !v->paused) {
      mix_buffer(mix, frames, v->buf, &v->pos, v->volume, v->loop,
                 0, 0, &v->active);
    }
  }
  pthread_mutex_unlock(&g_lock);

  int16_t *out = (int16_t *)stream;
  float gain = 0.80f;
  const char *eg = getenv("SONIC_AUDIO_GAIN");
  if (eg) {
    gain = (float)atof(eg);
    if (gain <= 0.0f || gain > 4.0f) gain = 1.0f;
  }
  /* Plano C: card cru -> aplica o volume do sistema (botoes) por software. */
  if (g_opened_raw) gain *= g_sys_vol;
  for (int s = 0; s < frames * OUT_CH; s++) {
    int v = soft_limit_s16((int)(mix[s] * gain));
    out[s] = clamp_s16(v);
  }
}

static int sa_try_open(SDL_AudioSpec *want, SDL_AudioSpec *have) {
  /* 1) tenta o device "default" (NULL) — funciona na maioria (igual aos ports padrao do PortMaster
     e ao Crazy Taxi: abre o default e pronto). */
  g_dev = SDL_OpenAudioDevice(NULL, 0, want, have, 0);
  if (g_dev) return 1;
  /* 🔑 SIMPLIFICADO: por PADRAO paramos aqui (default falhou = mudo, sem forcar nada). A varredura
     de card NOMEADO abaixo (batocera/knulli sem PCM default) era o que, no muOS, mandava o som pro
     HDMI (speaker busy -> proximo card = HDMI). So roda se SONIC_AUDIO_ENUM=1. O jeito certo p/
     "sem som" e o AUDIO_DRIVER (alsa/pulse/pipewire) no launcher, nao brigar com o card. */
  if (!getenv("SONIC_AUDIO_ENUM")) {
    fprintf(stderr, "sonic_audio: default nao abriu (%s); enum de card DESLIGADA (SONIC_AUDIO_ENUM=1 liga). "
            "Se faltar som, use AUDIO_DRIVER=alsa|pulse|pipewire.\n", SDL_GetError());
    return 0;
  }
  /* 2) "default" falhou. Em varios batocera/knulli o ALSA NAO tem PCM "default"
     definido -> "Couldn't open audio device: No such file or directory", e o som
     fica num card especifico (hw:0...). Enumera os devices de SAIDA que o driver
     atual expoe e tenta cada um pelo NOME -> acha o card real sem hardcode. */
  /* SDL_GetNumAudioDevices/SDL_GetAudioDeviceName NAO estao na libSDL2 stub do
     sysroot (link falha) -> resolve em runtime na libSDL2 REAL do device. */
  static int (*p_num)(int) = NULL;
  static const char *(*p_name)(int, int) = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_num  = (int (*)(int))dlsym(RTLD_DEFAULT, "SDL_GetNumAudioDevices");
    p_name = (const char *(*)(int, int))dlsym(RTLD_DEFAULT, "SDL_GetAudioDeviceName");
    resolved = 1;
  }
  if (!p_num || !p_name) return 0;
  int nd = p_num(0);
  fprintf(stderr, "sonic_audio: default falhou (%s); %d device(s) de saida nomeados\n",
          SDL_GetError(), nd);
  /* 🔊 FIX muOS "som no HDMI" (default ON; SONIC_NO_PREFER_SPEAKER desliga): num
     handheld o alvo e o ALTO-FALANTE, nao o HDMI. O bug: o alto-falante (audiocodec)
     dava "Device or resource busy" (o frontend ainda nao soltou o card) -> o loop
     pegava o PROXIMO que abrisse = ahubhdmi (HDMI) -> som no HDMI.
     PASS 1: ignora devices com "hdmi" no nome E faz RETRY no que der "busy" (o card
     do falante costuma liberar 1-2s apos o launch). PASS 2: fallback p/ qualquer um
     (inclusive HDMI) se nada nao-HDMI abriu. */
  int prefer_speaker = (getenv("SONIC_NO_PREFER_SPEAKER") == NULL);
  for (int pass = 0; pass < (prefer_speaker ? 2 : 1); pass++) {
    for (int i = 0; i < nd; i++) {
      const char *dn = p_name(i, 0);
      if (!dn || !*dn) continue;
      int is_hdmi = (strcasestr(dn, "hdmi") != NULL);
      if (prefer_speaker && pass == 0 && is_hdmi) {
        fprintf(stderr, "sonic_audio:   [%d] \"%s\" -> PULADO (HDMI, pass 1)\n", i, dn);
        continue;
      }
      /* retry no "busy" so no pass 1 (esperando o falante liberar) */
      int tries = (prefer_speaker && pass == 0) ? 5 : 1;
      for (int t = 0; t < tries; t++) {
        g_dev = SDL_OpenAudioDevice(dn, 0, want, have, 0);
        if (g_dev) break;
        const char *err = SDL_GetError();
        int busy = err && (strcasestr(err, "busy") != NULL);
        if (!busy || t + 1 >= tries) break;
        fprintf(stderr, "sonic_audio:   [%d] \"%s\" busy, retry %d/%d...\n",
                i, dn, t + 1, tries);
        usleep(400 * 1000); /* 400ms; ate ~2s no total */
      }
      fprintf(stderr, "sonic_audio:   [%d] \"%s\" (pass %d) -> %s\n", i, dn, pass + 1,
              g_dev ? "ABRIU" : SDL_GetError());
      if (g_dev) { g_opened_raw = 1; /* card cru: liga o volume por software (Plano C) */
                   return 1; }
    }
  }
  return 0;
}

/* Le um arquivo com um NUMERO cru 0-100 (ex.: /var/run/batocera-pending-volume,
   atualizado IMEDIATAMENTE pelos botoes). */
static int sa_read_bare_int(const char *f, float *out) {
  FILE *fp = fopen(f, "r");
  if (!fp) return 0;
  char buf[64] = {0};
  size_t n = fread(buf, 1, sizeof buf - 1, fp);
  fclose(fp);
  if (!n) return 0;
  char *p = buf; while (*p == ' ' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return 0;
  int v = atoi(p);
  if (v < 0 || v > 100) return 0;
  *out = (float)v / 100.0f;
  return 1;
}
/* Le a chave audio.volume=N de UM arquivo de config (batocera.conf / system.cfg). */
static int sa_read_conf_volume(const char *f, const char *key, float *out) {
  FILE *fp = fopen(f, "r");
  if (!fp) return 0;
  size_t kl = strlen(key); int got = 0; char line[256];
  while (fgets(line, sizeof line, fp)) {
    char *p = line; while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == ';') continue;
    if (strncmp(p, key, kl) == 0) {
      char *eq = strchr(p, '=');
      if (eq) { int v = atoi(eq + 1);
                if (v >= 0 && v <= 100) { *out = (float)v / 100.0f; got = 1; } }
    }
  }
  fclose(fp);
  return got;
}
/* Fallback DEVICE-AGNOSTICO: le o volume do mixer ALSA Master via amixer.
   Funciona em QUALQUER CFW com alsa (ROCKNIX/Knulli/muOS/ArchR) quando os paths
   de config nao existem. Parseia o "[NN%]" da saida de `amixer sget Master`. */
static int sa_read_amixer_volume(float *out) {
  FILE *fp = popen("amixer sget Master 2>/dev/null", "r");
  if (!fp) return 0;
  char line[256]; int got = 0;
  while (fgets(line, sizeof line, fp)) {
    char *b = strchr(line, '[');
    while (b) {
      char *pct = strchr(b, '%');
      if (pct && pct > b) {
        int v = atoi(b + 1);
        if (v >= 0 && v <= 100) { *out = (float)v / 100.0f; got = 1; }
        break;
      }
      b = strchr(b + 1, '[');
    }
    if (got) break;
  }
  pclose(fp);
  return got;
}
/* Le o volume do sistema (0..1). Ordem: SONIC_VOLUME_FILE (override/teste) ->
   /var/run/batocera-pending-volume (batocera/Knulli, botoes em tempo real) ->
   configs (batocera.conf E system.cfg do ROCKNIX/ArchR, audio.volume=N) ->
   amixer Master (fallback device-agnostico p/ qualquer CFW alsa). */
static int sa_read_sys_volume(float *out) {
  const char *ovr = getenv("SONIC_VOLUME_FILE");
  const char *key = getenv("SONIC_VOLUME_KEY");
  if (!key || !*key) key = "audio.volume";
  if (ovr && *ovr) {
    if (sa_read_bare_int(ovr, out)) return 1;
    if (sa_read_conf_volume(ovr, key, out)) return 1; /* aceita key=value no override */
  }
  if (sa_read_bare_int("/var/run/batocera-pending-volume", out)) return 1;
  /* paths de config por CFW (1o que existir e tiver a chave vence) */
  static const char *confs[] = {
    "/userdata/system/batocera.conf",            /* batocera/Knulli */
    "/storage/.config/system/configs/system.cfg",/* ROCKNIX/ArchR/JELOS */
    "/storage/.config/distribution/configs/distribution.conf",
  };
  for (unsigned i = 0; i < sizeof(confs)/sizeof(confs[0]); i++)
    if (sa_read_conf_volume(confs[i], key, out)) return 1;
  /* fallback universal: mixer ALSA (so quando os arquivos nao existem) */
  if (sa_read_amixer_volume(out)) return 1;
  return 0;
}
static void *sa_sysvol_thread(void *a) {
  (void)a;
  float last = -1.0f;
  for (;;) {
    float v;
    if (sa_read_sys_volume(&v)) {
      g_sys_vol = v;
      if (v != last) { /* diagnostico: prova que segue os botoes */
        fprintf(stderr, "sonic_audio: sys volume -> %.2f\n", v);
        last = v;
      }
    }
    usleep(250000); /* ~0.25s: responde rapido aos botoes */
  }
  return NULL;
}

/* Drivers que ABREM com sucesso mas NAO produzem som audivel -> nunca aceitar
   como "funcional": "dummy" (silencio) e "disk" (escreve o audio num ARQUIVO
   sdlaudio.raw). Foi a causa REAL do "sem som" num device de teste: o SDL caiu
   de pipewire (sem daemon) direto p/ "disk". Pulando-os, a varredura encontra o
   driver real (alsa) que a maioria dos devices usa. ("dsp"/OSS NAO e bloqueado:
   pode ser audivel; fica como candidato normal.) */
static int sa_driver_silent(const char *n) {
  return !n || strcmp(n, "dummy") == 0 || strcmp(n, "disk") == 0;
}

/* Áudio ADAPTATIVO, sem caminho fixo (regra #6: nunca hardcodar SDL_AUDIODRIVER).
   1) tenta o driver que o SDL do device escolhe sozinho — onde já funciona
      (alsa/pulse/pipewire vivo) abre de primeira, idêntico ao comportamento antigo;
   2) se falhar (ex.: SDL escolhe pipewire mas o daemon não existe -> "pw_loop_new
      can't make support.system handle" no muOS, ou pulse sem runtime-dir), VARRE
      todos os drivers que aquele SDL expõe e usa o 1º que ABRE de verdade (pula
      "dummy" e o que já falhou). Cobre ALSA/Pulse/PipeWire automaticamente. */
/* seleciona ESTE driver pro próximo init e tenta abrir o device. 1 = abriu. */
static int sa_open_named_driver(const char *name, SDL_AudioSpec *want,
                                SDL_AudioSpec *have) {
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  setenv("SDL_AUDIODRIVER", name, 1);
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    alog("sonic_audio: init driver=%s falhou: %s\n", name, SDL_GetError());
    return 0;
  }
  const char *cur = SDL_GetCurrentAudioDriver();
  if (!cur || strcmp(cur, name) != 0) return 0;
  if (sa_try_open(want, have)) {
    fprintf(stderr, "sonic_audio: SDL audio aberto (fallback) %dHz %dch samples=%d driver=%s\n",
            have->freq, have->channels, have->samples, name);
    return 1;
  }
  fprintf(stderr, "sonic_audio: driver=%s nao abriu device: %s\n", name, SDL_GetError());
  return 0;
}

static int ensure_audio(void) {
  if (g_audio_init) return g_dev != 0;

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = OUT_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = OUT_CH;
  want.samples = 1024;
  want.callback = sdl_audio_cb;

  int ndrv = SDL_GetNumAudioDrivers();
  { char list[256]; size_t L = 0; list[0] = 0;
    for (int i = 0; i < ndrv; i++) {
      const char *n = SDL_GetAudioDriver(i);
      if (n && L + strlen(n) + 2 < sizeof(list))
        L += (size_t)snprintf(list + L, sizeof(list) - L, "%s ", n);
    }
    /* SEMPRE visível (não-gated): em devices que não temos (muOS/Knulli) este
     * banner diz exatamente quais drivers existem e qual abriu -> diagnóstico de
     * "sem som". */
    fprintf(stderr, "sonic_audio: drivers disponiveis: %s\n", list);
  }

  /* (0) AUDIO_DRIVER do launcher (via SONIC_AUDIODRIVER): força esse driver do SDL. Aceita
     "alsa", "pulse"/"pulseaudio", "pipewire", etc. TOLERANTE: o SDL chama o pulse de "pulseaudio",
     então normalizamos "pulse"->"pulseaudio", e o sucesso é "abriu o áudio" (não exige nome exato).
     Loga CLARO que foi setado e se deu certo. */
  {
    const char *ovr = getenv("SONIC_AUDIODRIVER");
    if (ovr && *ovr && strcmp(ovr, "dummy") != 0) {
      const char *sdlname = (strcmp(ovr, "pulse") == 0) ? "pulseaudio" : ovr;
      fprintf(stderr, "=== AUDIO_DRIVER=%s -> SDL_AUDIODRIVER=%s (forçado pelo launcher) ===\n",
              ovr, sdlname);
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      setenv("SDL_AUDIODRIVER", sdlname, 1);
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        const char *cur = SDL_GetCurrentAudioDriver();
        if (cur && !sa_driver_silent(cur) && sa_try_open(&want, &have)) {
          fprintf(stderr, "sonic_audio: SDL audio aberto (AUDIO_DRIVER=%s) %dHz %dch samples=%d driver=%s -> OK\n",
                  ovr, have.freq, have.channels, have.samples, cur);
          goto opened;
        }
      }
      fprintf(stderr, "sonic_audio: AUDIO_DRIVER=%s NAO abriu (%s) -> tenta automatico\n",
              ovr, SDL_GetError());
      unsetenv("SDL_AUDIODRIVER");
    }
  }

  /* (1) tentativa automática (escolha do SDL do device) */
  char failed_drv[32]; failed_drv[0] = 0;
  if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
    SDL_InitSubSystem(SDL_INIT_AUDIO);
  if (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) {
    const char *drv = SDL_GetCurrentAudioDriver();
    if (drv && !sa_driver_silent(drv) && sa_try_open(&want, &have)) {
      fprintf(stderr, "sonic_audio: SDL audio aberto (auto) %dHz %dch samples=%d driver=%s\n",
              have.freq, have.channels, have.samples, drv);
      goto opened;
    }
    if (drv) { strncpy(failed_drv, drv, sizeof(failed_drv) - 1); failed_drv[sizeof(failed_drv) - 1] = 0; }
    fprintf(stderr, "sonic_audio: open automatico falhou (driver=%s: %s) -> varrendo drivers\n",
            drv ? drv : "?", SDL_GetError());
  }

  /* (2) fallback SIMPLES: varre os drivers e abre o DEFAULT de cada um, preferindo servidor de som
     (pipewire/pulse) antes do alsa cru. Sem enum de card nomeado (isso virou opt-in em sa_try_open).
     .79/.104 nem chegam aqui (o auto acima ja abre). */
  {
    static const char *pref[] = { "pipewire", "pulseaudio", "pulse", 0 };
    for (int p = 0; pref[p]; p++)
      for (int i = 0; i < ndrv; i++) {
        const char *name = SDL_GetAudioDriver(i);
        if (name && strcmp(name, pref[p]) == 0 && !sa_driver_silent(name) &&
            (!failed_drv[0] || strcmp(name, failed_drv) != 0) &&
            sa_open_named_driver(name, &want, &have)) goto opened;
      }
    for (int i = 0; i < ndrv; i++) {
      const char *name = SDL_GetAudioDriver(i);
      if (name && !sa_driver_silent(name) && (!failed_drv[0] || strcmp(name, failed_drv) != 0) &&
          sa_open_named_driver(name, &want, &have)) goto opened;
    }
  }
  unsetenv("SDL_AUDIODRIVER");
  fprintf(stderr, "sonic_audio: NENHUM driver de audio funcional (jogo segue, mas mudo)\n");
  g_audio_init = 1;
  return 0;

opened:
  /* Plano C (volume por software seguindo os botoes via batocera.conf/amixer) — agora OPT-IN:
     so roda com SONIC_SYSVOL=1 (antes ligava sozinho em card cru). Nos nossos devices o volume e do
     sistema (pulse/alsa default), nao precisa. Devices de card cru sem softvol (batocera/Knulli)
     ligam SONIC_SYSVOL=1 (+ SONIC_AUDIO_ENUM=1 p/ chegar no card). SONIC_FORCE_SYSVOL = alias legado. */
  if (g_opened_raw && (getenv("SONIC_SYSVOL") || getenv("SONIC_FORCE_SYSVOL"))) {
    float v0;
    if (sa_read_sys_volume(&v0)) g_sys_vol = v0;
    pthread_t th;
    if (pthread_create(&th, NULL, sa_sysvol_thread, NULL) == 0) {
      pthread_detach(th);
      fprintf(stderr, "sonic_audio: volume por software ON (card cru) vol=%.2f\n", g_sys_vol);
    }
  }
  SDL_PauseAudioDevice(g_dev, 0);
  g_audio_init = 1;
  return 1;
}

int sonic_audio_play_sfx(const char *key, float volume, int loop) {
  if (!key || !*key) return 0;
  if (!ensure_audio()) return 0;
  AudioBuffer *buf = load_sfx(key);
  if (!buf) return 0;

  pthread_mutex_lock(&g_lock);
  if (sfx_key_is_mechanical(key)) {
    for (int i = 0; i < MAX_VOICES; i++) {
      Voice *old = &g_voices[i];
      if (old->active && !old->paused && strcmp(old->key, key) == 0) {
        float vol = clamp_volume(volume);
        if (vol > old->volume) old->volume = vol;
        int handle = old->handle;
        pthread_mutex_unlock(&g_lock);
        alog("sonic_audio: reuse mechanical sfx key=\"%s\" handle=%d\n",
             key, handle);
        return handle;
      }
    }
  }

  int slot = -1;
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!g_voices[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) slot = 0;
  int handle = g_next_handle++;
  if (g_next_handle <= 0) g_next_handle = 1;
  Voice *v = &g_voices[slot];
  strncpy(v->key, key, sizeof(v->key) - 1);
  v->key[sizeof(v->key) - 1] = 0;
  v->handle = handle;
  v->buf = buf;
  v->pos = 0;
  v->volume = clamp_volume(volume);
  /* The game's third PlaySound argument is not a plain Android loop flag here:
   * observed one-shot sounds arrive as 2/3/-1. Treat SFX as one-shot until a
   * concrete looping key is mapped, otherwise rings/springs drone forever. */
  v->loop = 0;
  v->paused = 0;
  v->active = 1;
  pthread_mutex_unlock(&g_lock);
  alog("sonic_audio: play sfx key=\"%s\" handle=%d volume=%.3f loop=%d frames=%u\n",
       key, handle, volume, loop, buf->frames);
  return handle;
}

void sonic_audio_stop_sfx(int handle) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_voices[i].handle == handle) g_voices[i].active = 0;
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_pause_sfx(int handle, int paused) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_voices[i].handle == handle) g_voices[i].paused = paused != 0;
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_set_sfx_volume(int handle, float volume) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (g_voices[i].handle == handle) g_voices[i].volume = clamp_volume(volume);
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_reset_sfx(void) {
  pthread_mutex_lock(&g_lock);
  memset(g_voices, 0, sizeof(g_voices));
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_music_set_source(int id, const char *key) {
  if (id < 0 || id >= MUSIC_SLOTS || !key) return;
  pthread_mutex_lock(&g_lock);
  MusicSlot *m = &g_music[id];
  int same_active = m->active && strcmp(m->key, key) == 0;
  if (same_active) {
    m->jingle = music_key_is_jingle(key);
    uint32_t pos = m->pos;
    pthread_mutex_unlock(&g_lock);
    alog("sonic_audio: music source unchanged id=%d key=\"%s\" pos=%u\n",
         id, key, pos);
    return;
  }
  int was_jingle = m->jingle;
  strncpy(m->key, key, sizeof(m->key) - 1);
  m->key[sizeof(m->key) - 1] = 0;
  m->pos = 0;
  m->loop = music_key_default_loop(key);
  m->loop_explicit = 0;
  music_key_loop_points(key, &m->loop_start, &m->loop_end);
  m->active = 0;
  m->paused = 0;
  m->jingle = music_key_is_jingle(key);
  m->paused_by_jingle = 0;
  if (was_jingle) resume_music_after_jingle_locked();
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_music_start(int id) {
  if (id < 0 || id >= MUSIC_SLOTS) return;
  if (!ensure_audio()) return;

  char key[96];
  pthread_mutex_lock(&g_lock);
  strncpy(key, g_music[id].key, sizeof(key) - 1);
  key[sizeof(key) - 1] = 0;
  int same_active = g_music[id].active && strcmp(g_music[id].key, key) == 0;
  if (same_active) {
    MusicSlot *m = &g_music[id];
    int is_jingle = music_key_is_jingle(key);
    if (is_jingle) {
      pause_music_for_jingle_locked(id);
    } else if (!m->paused_by_jingle) {
      m->paused = 0;
    }
    if (m->volume <= 0.0f) m->volume = 1.0f;
    m->jingle = is_jingle;
    uint32_t pos = m->pos;
    int loop = m->loop;
    pthread_mutex_unlock(&g_lock);
    alog("sonic_audio: music start keep id=%d key=\"%s\" pos=%u jingle=%d loop=%d\n",
         id, key, pos, is_jingle, loop);
    return;
  }
  pthread_mutex_unlock(&g_lock);
  if (!key[0]) return;

  AudioBuffer *buf = load_music(key);
  if (!buf) return;

  pthread_mutex_lock(&g_lock);
  MusicSlot *m = &g_music[id];
  int is_jingle = music_key_is_jingle(key);
  if (is_jingle) pause_music_for_jingle_locked(id);
  int pause_behind_jingle = !is_jingle && any_active_jingle_locked();
  m->buf = buf;
  m->pos = 0;
  if (m->volume <= 0.0f) m->volume = 1.0f;
  if (!m->loop_explicit) m->loop = music_key_default_loop(key);
  music_key_loop_points(key, &m->loop_start, &m->loop_end);
  m->paused = pause_behind_jingle;
  m->active = 1;
  m->jingle = is_jingle;
  m->paused_by_jingle = pause_behind_jingle;
  int loop = m->loop;
  uint32_t loop_start = m->loop_start;
  uint32_t loop_end = m->loop_end;
  pthread_mutex_unlock(&g_lock);
  alog("sonic_audio: music start id=%d key=\"%s\" frames=%u jingle=%d loop=%d loop=%u-%u\n",
       id, key, buf->frames, is_jingle, loop, loop_start, loop_end);
}

void sonic_audio_music_stop(int id) {
  if (id < 0 || id >= MUSIC_SLOTS) return;
  pthread_mutex_lock(&g_lock);
  int was_jingle = g_music[id].jingle;
  g_music[id].active = 0;
  g_music[id].pos = 0;
  g_music[id].jingle = 0;
  g_music[id].paused_by_jingle = 0;
  if (was_jingle) resume_music_after_jingle_locked();
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_music_pause(int id, int paused) {
  if (id < 0 || id >= MUSIC_SLOTS) return;
  pthread_mutex_lock(&g_lock);
  g_music[id].paused = paused != 0;
  if (g_music[id].jingle && paused)
    resume_music_after_jingle_locked();
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_music_set_volume(int id, float volume) {
  if (id < 0 || id >= MUSIC_SLOTS) return;
  pthread_mutex_lock(&g_lock);
  g_music[id].volume = clamp_volume(volume);
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_music_set_loop(int id, int loop) {
  if (id < 0 || id >= MUSIC_SLOTS) return;
  pthread_mutex_lock(&g_lock);
  g_music[id].loop = (loop != 0) && music_key_allows_loop(g_music[id].key);
  g_music[id].loop_explicit = 1;
  pthread_mutex_unlock(&g_lock);
}

void sonic_audio_reset_music(int id) {
  if (id < 0) {
    pthread_mutex_lock(&g_lock);
    memset(g_music, 0, sizeof(g_music));
    pthread_mutex_unlock(&g_lock);
    return;
  }
  if (id >= MUSIC_SLOTS) return;
  pthread_mutex_lock(&g_lock);
  int was_jingle = g_music[id].jingle;
  memset(&g_music[id], 0, sizeof(g_music[id]));
  if (was_jingle) resume_music_after_jingle_locked();
  pthread_mutex_unlock(&g_lock);
}

int sonic_audio_music_state(int id) {
  if (id < 0 || id >= MUSIC_SLOTS) return 1;
  pthread_mutex_lock(&g_lock);
  int playing = g_music[id].active && !g_music[id].paused;
  pthread_mutex_unlock(&g_lock);
  return playing ? 0 : 1;
}

/* 1 se o canal `id` for um JINGLE e estiver tocando; senão 0. Usado pelo
   my_MediaPlayerisPlaying p/ consertar o jingle do 1up SEM mudar os outros canais
   (canais não-jingle mantêm o comportamento antigo, preservando os SFX/BGM). */
int sonic_audio_jingle_playing(int id) {
  if (id < 0 || id >= MUSIC_SLOTS) return 0;
  pthread_mutex_lock(&g_lock);
  int r = g_music[id].active && !g_music[id].paused && g_music[id].jingle;
  pthread_mutex_unlock(&g_lock);
  return r ? 1 : 0;
}
