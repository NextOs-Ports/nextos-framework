#define _GNU_SOURCE
#include "audio_backend.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

#define MAX_SFX 512
#define MAX_MUSIC 128

typedef struct {
  char key[256];
  char path[512];
  Mix_Chunk *chunk;
  int channel;
  float volume;
  float pan;
} SfxEntry;

typedef struct {
  char key[256];
  char path[512];
  Mix_Music *music;
  void *data;
  size_t size;
} MusicEntry;

static zip_t *g_apk;
static int g_ready;
static float g_global_volume = 1.0f;
static float g_sfx_volume = 1.0f;
static SfxEntry g_sfx[MAX_SFX];
static MusicEntry g_music[MAX_MUSIC];
static unsigned g_sfx_count;
static unsigned g_music_count;

static float clamp01(float v) {
  if (!(v >= 0.0f))
    return 0.0f;
  if (v > 1.0f)
    return 1.0f;
  return v;
}

static int mix_volume(float v) {
  v = clamp01(v);
  return (int)(v * MIX_MAX_VOLUME + 0.5f);
}

static const char *base_name(const char *s) {
  const char *last = s;
  if (!s)
    return "";
  for (const char *p = s; *p; p++) {
    if (*p == '/' || *p == '\\')
      last = p + 1;
  }
  return last;
}

static void normalize_key(char *out, size_t out_sz, const char *name) {
  size_t j = 0;
  if (!out_sz)
    return;
  out[0] = 0;
  if (!name)
    return;
  while (*name == '/' || *name == '\\')
    name++;
  for (const char *p = name; *p && j + 1 < out_sz; p++) {
    char c = *p == '\\' ? '/' : *p;
    out[j++] = (char)tolower((unsigned char)c);
  }
  out[j] = 0;
}

static int has_audio_ext(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return 0;
  return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".ogg") ||
         !strcasecmp(dot, ".wav") || !strcasecmp(dot, ".flac") ||
         !strcasecmp(dot, ".m4a");
}

static int zip_resolve_case(char *out, size_t out_sz, const char *path) {
  zip_stat_t st;
  zip_int64_t count;
  if (!g_apk || !path || !*path)
    return 0;
  zip_stat_init(&st);
  if (zip_stat(g_apk, path, 0, &st) == 0 && st.size > 0) {
    snprintf(out, out_sz, "%s", path);
    return 1;
  }

  count = zip_get_num_entries(g_apk, 0);
  for (zip_uint64_t i = 0; i < (zip_uint64_t)count; i++) {
    const char *name = zip_get_name(g_apk, i, 0);
    if (name && !strcasecmp(name, path)) {
      zip_stat_init(&st);
      if (zip_stat(g_apk, name, 0, &st) == 0 && st.size > 0) {
        snprintf(out, out_sz, "%s", name);
        return 1;
      }
    }
  }
  return 0;
}

static int resolve_path(char *out, size_t out_sz, const char *name,
                        int prefer_music) {
  char key[256];
  normalize_key(key, sizeof(key), name);
  if (!key[0])
    return 0;

  const char *prefixes_music[] = {"assets/music/", "music/", "", NULL};
  const char *prefixes_sfx[] = {"assets/soundfx/", "soundfx/", "", NULL};
  const char *exts[] = {"", ".mp3", ".ogg", ".wav", ".flac", NULL};
  const char **prefixes = prefer_music ? prefixes_music : prefixes_sfx;

  if (!strncmp(key, "assets/", 7)) {
    if (zip_resolve_case(out, out_sz, key))
      return 1;
  } else if (strchr(key, '/')) {
    char prefixed[512];
    snprintf(prefixed, sizeof(prefixed), "assets/%s", key);
    if (zip_resolve_case(out, out_sz, prefixed))
      return 1;
    if (zip_resolve_case(out, out_sz, key))
      return 1;
  }

  const char *leaf = base_name(key);
  for (int p = 0; prefixes[p]; p++) {
    for (int e = 0; exts[e]; e++) {
      char candidate[512];
      if (exts[e][0] && has_audio_ext(leaf))
        continue;
      snprintf(candidate, sizeof(candidate), "%s%s%s", prefixes[p], leaf,
               exts[e]);
      if (strncmp(candidate, "assets/", 7)) {
        char prefixed[512];
        snprintf(prefixed, sizeof(prefixed), "assets/%s", candidate);
        if (zip_resolve_case(out, out_sz, prefixed))
          return 1;
      }
      if (zip_resolve_case(out, out_sz, candidate))
        return 1;
    }
  }
  return 0;
}

static int read_apk_file(const char *path, void **out_data, size_t *out_size) {
  zip_stat_t st;
  zip_file_t *zf;
  uint8_t *buf;
  zip_uint64_t done = 0;

  *out_data = NULL;
  *out_size = 0;
  zip_stat_init(&st);
  if (!g_apk || zip_stat(g_apk, path, 0, &st) != 0 || st.size == 0)
    return 0;

  zf = zip_fopen(g_apk, path, 0);
  if (!zf)
    return 0;
  buf = (uint8_t *)malloc((size_t)st.size);
  if (!buf) {
    zip_fclose(zf);
    return 0;
  }
  while (done < st.size) {
    zip_int64_t r = zip_fread(zf, buf + done, st.size - done);
    if (r <= 0) {
      free(buf);
      zip_fclose(zf);
      return 0;
    }
    done += (zip_uint64_t)r;
  }
  zip_fclose(zf);
  *out_data = buf;
  *out_size = (size_t)st.size;
  return 1;
}

static SfxEntry *find_sfx(const char *name) {
  char key[256];
  normalize_key(key, sizeof(key), name);
  for (unsigned i = 0; i < g_sfx_count; i++) {
    if (!strcmp(g_sfx[i].key, key))
      return &g_sfx[i];
  }
  return NULL;
}

static MusicEntry *find_music(const char *name) {
  char key[256];
  normalize_key(key, sizeof(key), name);
  for (unsigned i = 0; i < g_music_count; i++) {
    if (!strcmp(g_music[i].key, key))
      return &g_music[i];
  }
  return NULL;
}

static int name_prefers_music(const char *name) {
  char key[256];
  normalize_key(key, sizeof(key), name);
  return !strncmp(key, "music/", 6) || !strncmp(key, "assets/music/", 13);
}

int audio_backend_init(const char *apk_path) {
  int zip_err = 0;
  int mix_flags;
  if (g_ready)
    return 1;

  mix_flags = Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC);
  fprintf(stderr, "[audio] Mix_Init flags=0x%x\n", mix_flags);
  if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
    fprintf(stderr, "[audio] Mix_OpenAudio falhou: %s\n", Mix_GetError());
    return 0;
  }
  Mix_AllocateChannels(32);
  Mix_VolumeMusic(mix_volume(g_global_volume));
  {
    int frequency = 0;
    int channels = 0;
    Uint16 format = 0;
    Mix_QuerySpec(&frequency, &format, &channels);
    fprintf(stderr,
            "[audio] output=OK driver=%s frequency=%d format=0x%x channels=%d\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
            frequency, (unsigned)format, channels);
  }

  g_apk = zip_open(apk_path, ZIP_RDONLY, &zip_err);
  if (!g_apk) {
    fprintf(stderr, "[audio] zip_open(%s) falhou err=%d\n",
            apk_path ? apk_path : "?", zip_err);
    Mix_CloseAudio();
    return 0;
  }
  g_ready = 1;
  fprintf(stderr, "[audio] backend SDL_mixer pronto apk=%s\n", apk_path);
  return 1;
}

void audio_backend_shutdown(void) {
  for (unsigned i = 0; i < g_sfx_count; i++) {
    if (g_sfx[i].chunk)
      Mix_FreeChunk(g_sfx[i].chunk);
  }
  for (unsigned i = 0; i < g_music_count; i++) {
    if (g_music[i].music)
      Mix_FreeMusic(g_music[i].music);
    free(g_music[i].data);
  }
  memset(g_sfx, 0, sizeof(g_sfx));
  memset(g_music, 0, sizeof(g_music));
  g_sfx_count = 0;
  g_music_count = 0;
  if (g_apk) {
    zip_close(g_apk);
    g_apk = NULL;
  }
  if (g_ready) {
    Mix_CloseAudio();
    Mix_Quit();
  }
  g_ready = 0;
}

int audio_backend_load_sound(const char *name) {
  char key[256];
  char path[512];
  void *data = NULL;
  size_t size = 0;
  SDL_RWops *rw;
  Mix_Chunk *chunk;
  SfxEntry *e;

  if (!g_ready || !name || !*name)
    return 1;
  e = find_sfx(name);
  if (e && e->chunk)
    return 1;
  if (g_sfx_count >= MAX_SFX)
    return 0;
  if (!resolve_path(path, sizeof(path), name, 0)) {
    fprintf(stderr, "[audio] sfx ausente: %s\n", name ? name : "?");
    return 0;
  }
  if (!read_apk_file(path, &data, &size))
    return 0;
  rw = SDL_RWFromConstMem(data, (int)size);
  if (!rw) {
    free(data);
    return 0;
  }
  chunk = Mix_LoadWAV_RW(rw, 1);
  free(data);
  if (!chunk) {
    fprintf(stderr, "[audio] Mix_LoadWAV_RW falhou %s: %s\n", path,
            Mix_GetError());
    return 0;
  }
  normalize_key(key, sizeof(key), name);
  e = &g_sfx[g_sfx_count++];
  snprintf(e->key, sizeof(e->key), "%s", key);
  snprintf(e->path, sizeof(e->path), "%s", path);
  e->chunk = chunk;
  e->channel = -1;
  e->volume = 1.0f;
  e->pan = 0.0f;
  Mix_VolumeChunk(chunk, mix_volume(g_global_volume * g_sfx_volume));
  fprintf(stderr, "[audio] sfx carregado: %s -> %s\n", key, path);
  return 1;
}

int audio_backend_load_music(const char *name) {
  char key[256];
  char path[512];
  void *data = NULL;
  size_t size = 0;
  SDL_RWops *rw;
  Mix_Music *music;
  MusicEntry *e;

  if (!g_ready || !name || !*name)
    return 1;
  e = find_music(name);
  if (e && e->music)
    return 1;
  if (g_music_count >= MAX_MUSIC)
    return 0;
  if (!resolve_path(path, sizeof(path), name, 1)) {
    fprintf(stderr, "[audio] musica ausente: %s\n", name ? name : "?");
    return 0;
  }
  if (!read_apk_file(path, &data, &size))
    return 0;
  rw = SDL_RWFromConstMem(data, (int)size);
  if (!rw) {
    free(data);
    return 0;
  }
  music = Mix_LoadMUS_RW(rw, 1);
  if (!music) {
    fprintf(stderr, "[audio] Mix_LoadMUS_RW falhou %s: %s\n", path,
            Mix_GetError());
    free(data);
    return 0;
  }
  normalize_key(key, sizeof(key), name);
  e = &g_music[g_music_count++];
  snprintf(e->key, sizeof(e->key), "%s", key);
  snprintf(e->path, sizeof(e->path), "%s", path);
  e->music = music;
  e->data = data;
  e->size = size;
  fprintf(stderr, "[audio] musica carregada: %s -> %s\n", key, path);
  return 1;
}

void audio_backend_play_sample(const char *name, int loops) {
  SfxEntry *sfx;
  MusicEntry *music;
  if (!g_ready || !name || !*name)
    return;

  if (name_prefers_music(name)) {
    music = find_music(name);
    if (!music && audio_backend_load_music(name))
      music = find_music(name);
    if (music && music->music) {
      Mix_VolumeMusic(mix_volume(g_global_volume));
      Mix_PlayMusic(music->music, loops);
    }
    return;
  }

  sfx = find_sfx(name);
  if (!sfx && audio_backend_load_sound(name))
    sfx = find_sfx(name);
  if (sfx && sfx->chunk) {
    int ch;
    Mix_VolumeChunk(sfx->chunk,
                    mix_volume(g_global_volume * g_sfx_volume * sfx->volume));
    ch = Mix_PlayChannel(-1, sfx->chunk, loops);
    if (ch >= 0) {
      Uint8 left = 255, right = 255;
      if (sfx->pan < -0.01f)
        right = (Uint8)(255.0f * (1.0f + sfx->pan));
      else if (sfx->pan > 0.01f)
        left = (Uint8)(255.0f * (1.0f - sfx->pan));
      Mix_SetPanning(ch, left, right);
      sfx->channel = ch;
    }
    return;
  }

  music = find_music(name);
  if (!music && audio_backend_load_music(name))
    music = find_music(name);
  if (music && music->music) {
    Mix_VolumeMusic(mix_volume(g_global_volume));
    Mix_PlayMusic(music->music, loops);
  }
}

void audio_backend_stop_sample(const char *name) {
  if (!name || !*name)
    return;
  SfxEntry *sfx = find_sfx(name);
  MusicEntry *music = find_music(name);
  if (sfx && sfx->channel >= 0)
    Mix_HaltChannel(sfx->channel);
  if (music)
    Mix_HaltMusic();
}

void audio_backend_pause_sample(const char *name) {
  if (!name || !*name)
    return;
  SfxEntry *sfx = find_sfx(name);
  MusicEntry *music = find_music(name);
  if (sfx && sfx->channel >= 0)
    Mix_Pause(sfx->channel);
  if (music)
    Mix_PauseMusic();
}

void audio_backend_set_sample_volume(const char *name, float volume) {
  if (!name || !*name)
    return;
  SfxEntry *sfx = find_sfx(name);
  volume = clamp01(volume);
  if (sfx) {
    sfx->volume = volume;
    if (sfx->chunk)
      Mix_VolumeChunk(sfx->chunk,
                      mix_volume(g_global_volume * g_sfx_volume * volume));
  }
}

void audio_backend_set_sample_pan(const char *name, float pan) {
  if (!name || !*name)
    return;
  SfxEntry *sfx = find_sfx(name);
  if (pan < -1.0f)
    pan = -1.0f;
  if (pan > 1.0f)
    pan = 1.0f;
  if (sfx) {
    sfx->pan = pan;
    if (sfx->channel >= 0) {
      Uint8 left = 255, right = 255;
      if (pan < -0.01f)
        right = (Uint8)(255.0f * (1.0f + pan));
      else if (pan > 0.01f)
        left = (Uint8)(255.0f * (1.0f - pan));
      Mix_SetPanning(sfx->channel, left, right);
    }
  }
}

void audio_backend_set_sample_speed(const char *name, float speed) {
  (void)name;
  (void)speed;
}

int audio_backend_sample_exists(const char *name) {
  char path[512];
  if (!name || !*name)
    return 0;
  if (find_sfx(name) || find_music(name))
    return 1;
  return resolve_path(path, sizeof(path), name, 0) ||
         resolve_path(path, sizeof(path), name, 1);
}

int audio_backend_is_sample_playing(const char *name) {
  if (!name || !*name)
    return 0;
  SfxEntry *sfx = find_sfx(name);
  MusicEntry *music = find_music(name);
  if (sfx && sfx->channel >= 0)
    return Mix_Playing(sfx->channel) != 0;
  if (music)
    return Mix_PlayingMusic() != 0;
  return 0;
}

void audio_backend_set_global_volume(float volume) {
  g_global_volume = clamp01(volume);
  Mix_VolumeMusic(mix_volume(g_global_volume));
  for (unsigned i = 0; i < g_sfx_count; i++) {
    if (g_sfx[i].chunk)
      Mix_VolumeChunk(g_sfx[i].chunk,
                      mix_volume(g_global_volume * g_sfx_volume *
                                 g_sfx[i].volume));
  }
}

float audio_backend_get_global_volume(void) { return g_global_volume; }

void audio_backend_set_sound_effect_volume(float volume) {
  g_sfx_volume = clamp01(volume);
  audio_backend_set_global_volume(g_global_volume);
}

float audio_backend_get_sound_effect_volume(void) { return g_sfx_volume; }
