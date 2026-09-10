#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "opensles_shim.h"
#include "util.h"

#define MAX_PLAYERS 16
#define RING_BUFFER_SIZE (512 * 1024)
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define SDL_AUDIO_CHUNK 4096

static const int id_engine_tag = 1;
static const int id_play_tag = 2;
static const int id_volume_tag = 3;
static const int id_bufferqueue_tag = 4;
static const int id_effectsend_tag = 5;
static const int id_enginecap_tag = 6;
static const int id_envreverb_tag = 7;

const SLInterfaceID sl_IID_ENGINE = &id_engine_tag;
const SLInterfaceID sl_IID_PLAY = &id_play_tag;
const SLInterfaceID sl_IID_VOLUME = &id_volume_tag;
const SLInterfaceID sl_IID_BUFFERQUEUE = &id_bufferqueue_tag;
const SLInterfaceID sl_IID_EFFECTSEND = &id_effectsend_tag;
const SLInterfaceID sl_IID_ENGINECAPABILITIES = &id_enginecap_tag;
const SLInterfaceID sl_IID_ENVIRONMENTALREVERB = &id_envreverb_tag;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void (*slBufferQueueCallback)(void *caller, void *pContext);

typedef struct {
  void *vtable;
  void (*RegisterCallback)(void *self, slBufferQueueCallback callback, void *pContext);
  void (*Clear)(void *self);
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*GetState)(void *self, void *pState);
} SLBufferQueueItf_Struct;

typedef struct {
  void *vtable;
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLmillisecond *pMsec);
  SLresult (*GetPosition)(void *self, SLmillisecond *pMsec);
  SLresult (*RegisterCallback)(void *self, void *callback, void *pContext);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 eventFlags);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pEventFlags);
  SLresult (*SetMarkerPosition)(void *self, SLmillisecond mSec);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLmillisecond *pMsec);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLmillisecond mSec);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLmillisecond *pMsec);
} SLPlayItf_Struct;

typedef struct {
  void *vtable;
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *pLevel);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *pMaxLevel);
  SLresult (*SetMute)(void *self, SLBoolean mute);
  SLresult (*GetMute)(void *self, SLBoolean *pMute);
  SLresult (*EnableStereoPosition)(void *self, SLBoolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLBoolean *pEnable);
  SLresult (*SetStereoPosition)(void *self, SLmillibel position);
  SLresult (*GetStereoPosition)(void *self, SLmillibel *pPosition);
} SLVolumeItf_Struct;

typedef struct {
  void *object_vtable;
  SLPlayItf_Struct play_itf;
  SLBufferQueueItf_Struct bq_itf;
  SLVolumeItf_Struct vol_itf;

  SLPlayItf_Struct *play_ptr;
  SLBufferQueueItf_Struct *bq_ptr;
  SLVolumeItf_Struct *vol_ptr;

  SLuint32 state;
  slBufferQueueCallback callback;
  void *callback_context;

  uint8_t ring[RING_BUFFER_SIZE];
  uint32_t ring_write;
  uint32_t ring_read;

  SLmillibel volume_level;
  float volume_gain;
  int in_use;
  int sample_rate;
  int channels;
  int bits_per_sample;
  int needs_callback;
} AudioPlayer;

typedef struct {
  void *object_vtable;
  void *engine_vtable;
  void *engine_ptr;
} AudioEngine;

static AudioPlayer g_players[MAX_PLAYERS];
static int g_num_players = 0;
static SDL_AudioStream *g_audio_stream = NULL;
static int g_audio_initialized = 0;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ring_write_data(AudioPlayer *p, const uint8_t *src, uint32_t len) {
  uint32_t w = p->ring_write;
  uint32_t r = p->ring_read;
  uint32_t avail = (r - w - 1) & RING_BUFFER_MASK;
  if (len > avail) len = avail;

  uint32_t part1 = RING_BUFFER_SIZE - (w & RING_BUFFER_MASK);
  if (part1 > len) part1 = len;
  uint32_t part2 = len - part1;

  memcpy(&p->ring[w & RING_BUFFER_MASK], src, part1);
  if (part2 > 0) memcpy(&p->ring[0], src + part1, part2);

  __sync_synchronize();
  p->ring_write = (w + len) & RING_BUFFER_MASK;
}

static uint32_t ring_read_data(AudioPlayer *p, uint8_t *dst, uint32_t len) {
  uint32_t w = p->ring_write;
  uint32_t r = p->ring_read;
  uint32_t avail = (w - r) & RING_BUFFER_MASK;
  if (len > avail) len = avail;

  uint32_t part1 = RING_BUFFER_SIZE - (r & RING_BUFFER_MASK);
  if (part1 > len) part1 = len;
  uint32_t part2 = len - part1;

  memcpy(dst, &p->ring[r & RING_BUFFER_MASK], part1);
  if (part2 > 0) memcpy(dst + part1, &p->ring[0], part2);

  __sync_synchronize();
  p->ring_read = (r + len) & RING_BUFFER_MASK;
  return len;
}

static uint32_t ring_available(AudioPlayer *p) {
  return (p->ring_write - p->ring_read) & RING_BUFFER_MASK;
}

static void SDLCALL sdl_audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
  (void)userdata; (void)total_amount;
  if (additional_amount <= 0) return;

  int needed = additional_amount;
  int16_t mix_buf[SDL_AUDIO_CHUNK / 2];
  int16_t play_buf[SDL_AUDIO_CHUNK / 2];

  while (needed > 0) {
    int chunk_bytes = needed > (int)sizeof(mix_buf) ? (int)sizeof(mix_buf) : needed;
    int chunk_samples = chunk_bytes / 2;
    memset(mix_buf, 0, chunk_bytes);

    int active = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      AudioPlayer *p = &g_players[i];
      if (!p->in_use || p->state != SL_PLAYSTATE_PLAYING) continue;

      uint32_t avail = ring_available(p);
      if (avail < (uint32_t)chunk_bytes) {
        p->needs_callback = 1;
        if (avail == 0) continue;
      }

      uint32_t got = ring_read_data(p, (uint8_t *)play_buf, chunk_bytes);
      int got_samples = got / 2;
      float gain = p->volume_gain;

      for (int s = 0; s < got_samples; s++) {
        int32_t val = mix_buf[s] + (int32_t)(play_buf[s] * gain);
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        mix_buf[s] = (int16_t)val;
      }
      active = 1;
      if (ring_available(p) < RING_BUFFER_SIZE / 2) p->needs_callback = 1;
    }

    if (active) {
      SDL_PutAudioStreamData(stream, mix_buf, chunk_bytes);
    } else {
      memset(mix_buf, 0, chunk_bytes);
      SDL_PutAudioStreamData(stream, mix_buf, chunk_bytes);
    }
    needed -= chunk_bytes;
  }
}

static void ensure_audio_initialized(void) {
  if (g_audio_initialized) return;
  pthread_mutex_lock(&g_audio_mutex);
  if (g_audio_initialized) {
    pthread_mutex_unlock(&g_audio_mutex);
    return;
  }

  SDL_AudioSpec spec;
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  spec.freq = 44100;

  g_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_audio_callback, NULL);
  if (g_audio_stream) {
    SDL_ResumeAudioStreamDevice(g_audio_stream);
    logPrintf("[SL] SDL3 Audio device stream initialized (44100Hz, 2ch, S16)\n");
  } else {
    logPrintf("[SL] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
  }

  g_audio_initialized = 1;
  pthread_mutex_unlock(&g_audio_mutex);
}

void opensles_shim_pump_callbacks(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->in_use && p->needs_callback) {
      p->needs_callback = 0;
      if (p->callback) p->callback(&p->bq_ptr, p->callback_context);
    }
  }
}

int opensles_shim_engine_active(void) { return g_audio_initialized; }

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, bq_ptr));
  ring_write_data(p, (const uint8_t *)pBuffer, size);
  return SL_RESULT_SUCCESS;
}

static void bq_RegisterCallback(void *self, slBufferQueueCallback callback, void *pContext) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, bq_ptr));
  p->callback = callback;
  p->callback_context = pContext;
}

static void bq_Clear(void *self) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, bq_ptr));
  p->ring_read = p->ring_write;
}

static SLresult bq_GetState(void *self, void *pState) {
  (void)self; (void)pState;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, play_ptr));
  p->state = state;
  if (state == SL_PLAYSTATE_PLAYING && p->callback && ring_available(p) == 0) {
    p->callback(&p->bq_ptr, p->callback_context);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, play_ptr));
  if (pState) *pState = p->state;
  return SL_RESULT_SUCCESS;
}

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, vol_ptr));
  p->volume_level = level;
  p->volume_gain = powf(10.0f, (float)level / 2000.0f);
  return SL_RESULT_SUCCESS;
}

static SLresult vol_GetVolumeLevel(void *self, SLmillibel *pLevel) {
  AudioPlayer *p = (AudioPlayer *)((uint8_t *)self - offsetof(AudioPlayer, vol_ptr));
  if (pLevel) *pLevel = p->volume_level;
  return SL_RESULT_SUCCESS;
}

static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *pMaxLevel) {
  (void)self;
  if (pMaxLevel) *pMaxLevel = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult vol_SetMute(void *self, SLBoolean mute) {
  (void)self; (void)mute;
  return SL_RESULT_SUCCESS;
}

static SLresult player_Realize(void *self, SLBoolean async) {
  (void)self; (void)async;
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  AudioPlayer *p = (AudioPlayer *)self;
  p->in_use = 0;
  p->state = SL_PLAYSTATE_STOPPED;
}

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  AudioPlayer *p = (AudioPlayer *)self;
  if (iid == sl_IID_PLAY) {
    *(void **)pInterface = &p->play_ptr;
    return SL_RESULT_SUCCESS;
  }
  if (iid == sl_IID_BUFFERQUEUE) {
    *(void **)pInterface = &p->bq_ptr;
    return SL_RESULT_SUCCESS;
  }
  if (iid == sl_IID_VOLUME) {
    *(void **)pInterface = &p->vol_ptr;
    return SL_RESULT_SUCCESS;
  }
  return SL_RESULT_RESOURCE_ERROR;
}

static SLresult engine_CreateAudioPlayer(void *self, void **pPlayer,
                                         SLDataSource *pAudioSrc,
                                         SLDataSink *pAudioSnk,
                                         SLuint32 numInterfaces,
                                         const SLInterfaceID *pInterfaceIds,
                                         const SLBoolean *pInterfaceRequired) {
  (void)self; (void)pAudioSnk; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  ensure_audio_initialized();

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g_players[i].in_use) {
      AudioPlayer *p = &g_players[i];
      memset(p, 0, sizeof(AudioPlayer));
      p->in_use = 1;
      p->state = SL_PLAYSTATE_STOPPED;
      p->volume_gain = 1.0f;

      if (pAudioSrc && pAudioSrc->pFormat) {
        SLDataFormat_PCM *pcm = (SLDataFormat_PCM *)pAudioSrc->pFormat;
        if (pcm->formatType == SL_DATAFORMAT_PCM) {
          p->sample_rate = pcm->samplesPerSec / 1000;
          p->channels = pcm->numChannels;
          p->bits_per_sample = pcm->bitsPerSample;
        }
      }

      static uintptr_t obj_vtable[8];
      obj_vtable[0] = (uintptr_t)player_Realize;
      obj_vtable[1] = (uintptr_t)ret0;
      obj_vtable[2] = (uintptr_t)player_GetInterface;
      obj_vtable[3] = (uintptr_t)player_Destroy;
      p->object_vtable = (void *)obj_vtable;

      p->play_itf.SetPlayState = play_SetPlayState;
      p->play_itf.GetPlayState = play_GetPlayState;
      p->play_ptr = &p->play_itf;

      p->bq_itf.Enqueue = bq_Enqueue;
      p->bq_itf.RegisterCallback = bq_RegisterCallback;
      p->bq_itf.Clear = bq_Clear;
      p->bq_itf.GetState = bq_GetState;
      p->bq_ptr = &p->bq_itf;

      p->vol_itf.SetVolumeLevel = vol_SetVolumeLevel;
      p->vol_itf.GetVolumeLevel = vol_GetVolumeLevel;
      p->vol_itf.GetMaxVolumeLevel = vol_GetMaxVolumeLevel;
      p->vol_itf.SetMute = vol_SetMute;
      p->vol_ptr = &p->vol_itf;

      *pPlayer = p;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_RESOURCE_ERROR;
}

static SLresult engine_CreateOutputMix(void *self, void **pMix, SLuint32 numInterfaces,
                                       const SLInterfaceID *pInterfaceIds,
                                       const SLBoolean *pInterfaceRequired) {
  (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  static uintptr_t mix_vtable[8];
  mix_vtable[0] = (uintptr_t)ret0; // Realize
  mix_vtable[1] = (uintptr_t)ret0;
  mix_vtable[2] = (uintptr_t)ret0; // GetInterface
  mix_vtable[3] = (uintptr_t)ret0; // Destroy
  static void *mix_ptr;
  mix_ptr = (void *)mix_vtable;
  *pMix = &mix_ptr;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_Realize(void *self, SLBoolean async) {
  (void)self; (void)async;
  return SL_RESULT_SUCCESS;
}

static void engine_Destroy(void *self) {
  (void)self;
}

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  AudioEngine *e = (AudioEngine *)self;
  if (iid == sl_IID_ENGINE) {
    *(void **)pInterface = &e->engine_ptr;
    return SL_RESULT_SUCCESS;
  }
  return SL_RESULT_RESOURCE_ERROR;
}

SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  static AudioEngine engine;
  static uintptr_t engine_obj_vtable[8];
  static uintptr_t engine_itf_vtable[8];

  engine_obj_vtable[0] = (uintptr_t)engine_Realize;
  engine_obj_vtable[2] = (uintptr_t)engine_GetInterface;
  engine_obj_vtable[3] = (uintptr_t)engine_Destroy;
  engine.object_vtable = (void *)engine_obj_vtable;

  engine_itf_vtable[0] = (uintptr_t)engine_CreateAudioPlayer;
  engine_itf_vtable[1] = (uintptr_t)ret0; // CreateAudioRecorder
  engine_itf_vtable[2] = (uintptr_t)ret0; // CreateMidiPlayer
  engine_itf_vtable[3] = (uintptr_t)ret0; // CreateListener
  engine_itf_vtable[4] = (uintptr_t)ret0; // Create3DEnvironment
  engine_itf_vtable[5] = (uintptr_t)engine_CreateOutputMix;
  engine.engine_vtable = (void *)engine_itf_vtable;
  engine.engine_ptr = engine.engine_vtable;

  *pEngine = &engine;
  return SL_RESULT_SUCCESS;
}
