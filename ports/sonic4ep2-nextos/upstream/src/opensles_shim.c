/*
 * opensles_shim.c -- OpenSL ES to SDL2 audio bridge
 *
 * Translates OpenSL ES BufferQueue audio players into SDL2 audio output.
 * Lock-free SPSC ring buffers between game thread and SDL audio thread.
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#define MAX_PLAYERS 16
/* Ring por player. ANTES 4MB (~23s áudio) × 16 = 64MB ESTÁTICOS (BSS) reservados
 * SEMPRE -> peso morto num device de ~833MB. O Oboe enfileira buffers pequenos
 * (~2×1024 frames); 512KB (~3s) cabe a fila com folga. 16×512KB = 8MB (T5: corte
 * geral de 8MB vs 1MB, sem downside se underruns=0). DEVE ser potência de 2 (& MASK).
 * Validar com [PERFAUD] underruns=0/dead=0. */
#define RING_BUFFER_SIZE (512 * 1024)
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define SDL_AUDIO_SAMPLES 4096

/* Interface ID storage */
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

/* OpenSL ES data structures */
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

/* Per-player state */
typedef void (*slBufferQueueCallback)(void *caller, void *pContext);

typedef struct {
  uint8_t ring[RING_BUFFER_SIZE];
  volatile uint32_t ring_head;
  volatile uint32_t ring_tail;

  uint32_t queued_sizes[64];
  volatile uint32_t queued_head_index;
  volatile uint32_t queued_tail_index;
  volatile uint32_t queued_count;
  volatile uint32_t queued_front_offset;
  uint32_t queue_capacity;

  slBufferQueueCallback callback;
  void *callback_context;

  uint32_t enqueued_since_cb;
  uint32_t last_enqueue_size;

  void (*play_callback)(void *caller, void *pContext, SLuint32 event);
  void *play_callback_context;
  SLuint32 play_event_mask;

  uint32_t enqueue_counter;
  uint32_t debug_enqueue_logs;
  uint32_t debug_callback_logs;
  uint32_t debug_play_callback_logs;
  int ever_enqueued;
  int headatend_fired;
  int decoder_done;
  int cb_noenq;   /* callbacks consecutivos que NÃO enfileiraram (anti-spam Oboe blocking-mode) */
  int cb_dead;    /* 1 = callback improdutivo -> parar de chamar (dados vêm via Enqueue) */
  volatile uint32_t underrun_count;
  volatile uint32_t fadeout_count;
  volatile uint32_t frames_played;  /* total output frames mixed, for fade-in */
  volatile int stopping;            /* 1 = SetPlayState(STOPPED) pediu parada -> fade-out 1 buffer e para (anti-click) */

  volatile SLuint32 play_state;
  float volume;
  int active;
  uint64_t played_bytes;

  SLuint32 num_channels;
  SLuint32 sample_rate;
  SLuint32 bits_per_sample;
  int is_float; /* PCM_EX float32 (Oboe) -> converte p/ S16 no Enqueue */

  void *obj_vtable[8];
  void *obj_ptr;
  void *play_vtable[8];
  void *play_ptr;
  void *volume_vtable[8];
  void *volume_ptr;
  void *bq_vtable[8];
  void *bq_ptr;
  void *effectsend_vtable[8];
  void *effectsend_ptr;
} AudioPlayer;

static AudioPlayer g_players[MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_initialized = 0;

static void queue_reset(AudioPlayer *p) {
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->played_bytes = 0;
}

static void queue_push(AudioPlayer *p, uint32_t size) {
  if (size == 0) return;

  if (p->queued_count >= (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]))) {
    uint32_t tail = (p->queued_tail_index - 1) %
                    (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
    p->queued_sizes[tail] += size;
    return;
  }

  uint32_t tail = p->queued_tail_index %
                  (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
  p->queued_sizes[tail] = size;
  p->queued_tail_index++;
  p->queued_count++;
}

static void queue_consume(AudioPlayer *p, uint32_t bytes) {
  while (bytes > 0 && p->queued_count > 0) {
    uint32_t head = p->queued_head_index %
                    (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
    uint32_t queued = p->queued_sizes[head];
    uint32_t remaining = queued - p->queued_front_offset;

    if (bytes < remaining) {
      p->queued_front_offset += bytes;
      return;
    }

    bytes -= remaining;
    p->queued_sizes[head] = 0;
    p->queued_head_index++;
    p->queued_count--;
    p->queued_front_offset = 0;
  }
}

/* Ring buffer helpers */
static inline uint32_t ring_readable(const AudioPlayer *p) {
  return p->ring_head - p->ring_tail;
}

static inline uint32_t ring_writable(const AudioPlayer *p) {
  return RING_BUFFER_SIZE - (p->ring_head - p->ring_tail);
}

static uint32_t ring_write(AudioPlayer *p, const void *data, uint32_t len) {
  uint32_t space = ring_writable(p);
  if (len > space) len = space;
  if (len == 0) return 0;

  const uint8_t *src = (const uint8_t *)data;
  uint32_t head = p->ring_head & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - head;
  if (first > len) first = len;
  memcpy(p->ring + head, src, first);
  if (len > first) memcpy(p->ring, src + first, len - first);

  __sync_synchronize();
  p->ring_head += len;
  return len;
}

static uint32_t ring_read(AudioPlayer *p, void *data, uint32_t len) {
  uint32_t avail = ring_readable(p);
  if (len > avail) len = avail;
  if (len == 0) return 0;

  uint8_t *dst = (uint8_t *)data;
  uint32_t tail = p->ring_tail & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - tail;
  if (first > len) first = len;
  memcpy(dst, p->ring + tail, first);
  if (len > first) memcpy(dst + first, p->ring, len - first);

  __sync_synchronize();
  p->ring_tail += len;
  return len;
}

/* SDL2 audio callback */
#define SDL_OUTPUT_RATE 44100
#define TMP_BUF_SAMPLES (SDL_AUDIO_SAMPLES * 2)

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  memset(stream, 0, len);

  int16_t *out = (int16_t *)stream;
  int out_samples = len / (int)sizeof(int16_t);

  static float mix_buf[SDL_AUDIO_SAMPLES * 2];
  if (out_samples > SDL_AUDIO_SAMPLES * 2) out_samples = SDL_AUDIO_SAMPLES * 2;
  memset(mix_buf, 0, out_samples * sizeof(float));

  uint32_t out_frames = out_samples / 2;

  /* Per-player temp buffer on stack */
  int16_t tmp[TMP_BUF_SAMPLES];

  /* Per-player diagnostics for click detection */
  float player_peak[MAX_PLAYERS];
  float player_vol[MAX_PLAYERS];
  int player_active_list[MAX_PLAYERS];
  int num_active = 0;
  memset(player_peak, 0, sizeof(player_peak));
  memset(player_vol, 0, sizeof(player_vol));

  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    uint32_t src_rate = p->sample_rate;
    if (src_rate == 0) src_rate = SDL_OUTPUT_RATE;
    uint32_t src_channels = p->num_channels;
    if (src_channels == 0) src_channels = 2;
    uint32_t frame_size = src_channels * sizeof(int16_t);
    float vol = p->volume;
    /* Guard against corrupted volume */
    if (vol < 0.0f || vol > 2.0f || vol != vol /* NaN */) {
      static uint32_t vol_warn = 0;
      if (vol_warn < 20) {
        debugPrintf("opensles_shim: CORRUPT vol player %d: volume=%f (raw bits=0x%08x)\n",
                    i, vol, *(uint32_t*)&vol);
        vol_warn++;
      }
      vol = 0.0f; /* mute corrupted player */
    }
    if (src_channels == 1) {
      vol *= 0.35f;
    } else {
      vol *= 0.8f;
    }
    player_vol[i] = vol;
    player_active_list[num_active++] = i;

    uint32_t src_frames_needed;
    if (src_rate == SDL_OUTPUT_RATE) {
      src_frames_needed = out_frames;
    } else {
      src_frames_needed = (uint32_t)((uint64_t)out_frames * src_rate / SDL_OUTPUT_RATE) + 2;
    }

    uint32_t src_bytes_want = src_frames_needed * frame_size;
    if (src_bytes_want > sizeof(tmp)) src_bytes_want = sizeof(tmp);
    src_bytes_want = (src_bytes_want / frame_size) * frame_size;

    uint32_t got = ring_read(p, tmp, src_bytes_want);
    got = (got / frame_size) * frame_size;
    uint32_t src_frames_got = got / frame_size;
    if (src_frames_got == 0) continue;
    queue_consume(p, got);
    p->played_bytes += got;

    /* Detect underrun: got less than requested = fade out last frames to avoid click */
    int underrun = (got < src_bytes_want);
    uint32_t fade_out_len = 64;
    uint32_t fade_start = 0;
    if (underrun) {
      p->underrun_count++;
      if (src_frames_got > fade_out_len) {
        fade_start = src_frames_got - fade_out_len;
      } else {
        /* Very short buffer: fade the entire thing */
        fade_start = 0;
        fade_out_len = src_frames_got;
      }
      p->fadeout_count++;
    }

    /* anti-click no STOP: rampa 1->0 no buffer inteiro deste callback (reusa o
     * caminho de fade-out) e depois para de fato (ver finalize abaixo). */
    int stopping_now = p->stopping;
    if (stopping_now) {
      underrun = 1;
      fade_start = 0;
      fade_out_len = src_frames_got;
    }

    /* Fade-in: first 32 output frames of a player's lifetime */
    uint32_t fadein_remaining = (p->frames_played < 32) ? (32 - p->frames_played) : 0;

    if (src_rate == SDL_OUTPUT_RATE && src_channels == 2) {
      uint32_t n = src_frames_got;
      if (n > out_frames) n = out_frames;
      for (uint32_t f = 0; f < n; f++) {
        float env = 1.0f;
        /* Fade-in */
        if (f < fadein_remaining) {
          env = (float)(p->frames_played + f) / 32.0f;
        }
        /* Fade-out on underrun */
        if (underrun && f >= fade_start && fade_out_len > 0) {
          float fo = 1.0f - (float)(f - fade_start) / (float)fade_out_len;
          if (fo < env) env = fo;
        }
        if (env < 0.0f) env = 0.0f;
        float v = vol * env;
        float cl = (float)tmp[f * 2]     * v;
        float cr = (float)tmp[f * 2 + 1] * v;
        mix_buf[f * 2]     += cl;
        mix_buf[f * 2 + 1] += cr;
        float ap = fabsf(cl);
        if (ap > player_peak[i]) player_peak[i] = ap;
        ap = fabsf(cr);
        if (ap > player_peak[i]) player_peak[i] = ap;
      }
      p->frames_played += n;
    } else {
      uint32_t step = (uint32_t)((uint64_t)src_rate * 65536 / SDL_OUTPUT_RATE);
      uint32_t pos = 0;
      /* Map fade_start from source frames to output frames */
      uint32_t fade_start_out = underrun ?
        (uint32_t)((uint64_t)fade_start * SDL_OUTPUT_RATE / src_rate) : out_frames + 1;
      uint32_t fade_out_len_out = (uint32_t)((uint64_t)fade_out_len * SDL_OUTPUT_RATE / src_rate);
      if (fade_out_len_out == 0) fade_out_len_out = 1;
      uint32_t mixed = 0;
      for (uint32_t f = 0; f < out_frames; f++) {
        uint32_t idx = pos >> 16;
        uint32_t frac = pos & 0xFFFF;
        if (idx >= src_frames_got) break;

        float l0, r0, l1, r1;
        if (src_channels == 1) {
          l0 = (float)tmp[idx]; r0 = l0;
          l1 = (idx + 1 < src_frames_got) ? (float)tmp[idx + 1] : l0; r1 = l1;
        } else {
          l0 = (float)tmp[idx * 2]; r0 = (float)tmp[idx * 2 + 1];
          if (idx + 1 < src_frames_got) { l1 = (float)tmp[(idx + 1) * 2]; r1 = (float)tmp[(idx + 1) * 2 + 1]; }
          else { l1 = l0; r1 = r0; }
        }

        float frac_f = (float)frac / 65536.0f;
        float left  = l0 + (l1 - l0) * frac_f;
        float right = r0 + (r1 - r0) * frac_f;
        float env = 1.0f;
        /* Fade-in */
        if (f < fadein_remaining) {
          env = (float)(p->frames_played + f) / 32.0f;
        }
        /* Fade-out on underrun */
        if (f >= fade_start_out && fade_out_len_out > 0) {
          float fo = 1.0f - (float)(f - fade_start_out) / (float)fade_out_len_out;
          if (fo < env) env = fo;
        }
        if (env < 0.0f) env = 0.0f;
        float v = vol * env;
        float cl = left * v;
        float cr = right * v;
        mix_buf[f * 2]     += cl;
        mix_buf[f * 2 + 1] += cr;
        float ap = fabsf(cl);
        if (ap > player_peak[i]) player_peak[i] = ap;
        ap = fabsf(cr);
        if (ap > player_peak[i]) player_peak[i] = ap;
        pos += step;
        mixed++;
      }
      p->frames_played += mixed;
    }

    /* finaliza o STOP depois do fade-out: agora sim para e descarta o resto */
    if (stopping_now) {
      p->play_state = SL_PLAYSTATE_STOPPED;
      p->ring_head = 0;
      p->ring_tail = 0;
      queue_reset(p);
      p->stopping = 0;
    }
  }

  /* Soft-clip using tanh-style limiter - smooth, no discontinuities.
   * Sonic 4 EP2 manda UM stream Oboe pré-mixado -> default 1.0; ajustável
   * sem rebuild via env SLSHIM_GAIN (ex: SLSHIM_GAIN=1.5). */
  static float master_gain = -1.0f;
  if (master_gain < 0.0f) {
    const char *g = getenv("SLSHIM_GAIN");
    master_gain = g ? (float)atof(g) : 1.0f;
    if (master_gain <= 0.0f || master_gain > 8.0f) master_gain = 1.0f;
    debugPrintf("opensles_shim: master_gain=%f\n", master_gain);
  }
  const float threshold = 28000.0f;
  const float knee = 4000.0f;  /* transition zone */
  static int16_t prev_left = 0, prev_right = 0;
  static uint32_t click_count = 0;
  static uint32_t callback_count = 0;
  callback_count++;

  for (int s = 0; s < out_samples; s++) {
    float x = mix_buf[s] * master_gain;
    float ax = fabsf(x);
    if (ax > threshold) {
      float over = ax - threshold;
      float compressed = threshold + knee * (over / (over + knee));
      x = (x > 0) ? compressed : -compressed;
    }
    if (x > 32767.0f) x = 32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    out[s] = (int16_t)x;
  }

  /* Detect clicks: large jump between last sample of previous buffer
   * and first sample of this buffer */
  int32_t jump_l = abs((int32_t)out[0] - (int32_t)prev_left);
  int32_t jump_r = abs((int32_t)out[1] - (int32_t)prev_right);
  /* Also detect mix_buf peak */
  float mix_peak = 0.0f;
  for (int s = 0; s < out_samples; s++) {
    float av = fabsf(mix_buf[s]);
    if (av > mix_peak) mix_peak = av;
  }
  if (jump_l > 8000 || jump_r > 8000) {
    click_count++;
    debugPrintf("opensles_shim: CLICK #%u cb#%u jump L=%d R=%d prev=%d/%d new=%d/%d mixpeak=%.0f active=%d\n",
                click_count, callback_count,
                jump_l, jump_r,
                (int)prev_left, (int)prev_right,
                (int)out[0], (int)out[1],
                mix_peak, num_active);
    for (int a = 0; a < num_active; a++) {
      int pi = player_active_list[a];
      AudioPlayer *pp = &g_players[pi];
      debugPrintf("  p%d: vol=%.3f peak=%.0f ch=%u rate=%u readable=%u\n",
                  pi, player_vol[pi], player_peak[pi],
                  pp->num_channels, pp->sample_rate, ring_readable(pp));
    }
  }
  if (out_samples >= 2) {
    prev_left = out[out_samples - 2];
    prev_right = out[out_samples - 1];
  }
  /* [PERFAUD] relatório ~5s: callbacks, underruns acumulados, clicks, players
   * ativos e enchimento do ring de cada um (diagnóstico do lag de áudio). */
  {
    static uint32_t last_report_cb = 0;
    uint32_t cbs_5s = (uint32_t)(5.0 * SDL_OUTPUT_RATE / (out_frames ? out_frames : 1024));
    if (callback_count - last_report_cb >= cbs_5s) {
      last_report_cb = callback_count;
      uint32_t under = 0;
      int dead = 0;  /* players c/ callback marcado morto = sinal do bug v2/v3 (esperado 0 na v4) */
      char rings[128]; int rp = 0; rings[0] = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        AudioPlayer *p = &g_players[i];
        under += p->underrun_count;
        if (p->cb_dead) dead++;
        if (p->active && p->play_state == SL_PLAYSTATE_PLAYING && rp < 100)
          rp += snprintf(rings + rp, sizeof(rings) - rp, " p%d=%u", i, ring_readable(p));
      }
      fprintf(stderr, "[PERFAUD] cb=%u underruns=%u clicks=%u active=%d dead=%d ring:%s\n",
              callback_count, under, click_count, num_active, dead, rings);
    }
  }
}

/* Initialize SDL2 audio */
static void ensure_audio_initialized(void) {
  if (g_audio_initialized) return;

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  /* 1024 frames = ~23ms (4096 dava 93ms de latência + 4 enqueues por callback
   * = áudio atrasado e engasgado). SEM ALLOW_SAMPLES_CHANGE: o SDL emula 1024
   * mesmo se o ALSA pedir mais. SONIC_AUDIO_SAMPLES ajusta sem rebuild. */
  {
    const char *e = getenv("SONIC_AUDIO_SAMPLES");
    int s = e ? atoi(e) : 1024;
    if (s < 256 || s > SDL_AUDIO_SAMPLES) s = 1024;
    want.samples = (Uint16)s;
  }
  want.callback = sdl_audio_callback;
  want.userdata = NULL;

  /* Abertura ADAPTATIVA (espelha sonic_audio.c). O fox usa OpenSLES -> ESTE é o
   * caminho de áudio real do jogo, e ele só fazia um SDL_OpenAudioDevice PLANO
   * com o driver default. Em muitos devices PortMaster (muOS, Knulli) o SDL
   * escolhe um default que NÃO abre de verdade (pipewire sem daemon, pulse sem
   * runtime-dir) -> o open falhava e o jogo ficava MUDO, SEM fallback (vídeo e
   * controle seguiam — exatamente o sintoma relatado). Aqui: tenta o default e,
   * se falhar, VARRE todos os drivers e usa o 1º que ABRE. Sempre loga a lista
   * (diagnóstico p/ devices que não temos). Regra #6: nada hardcoded, escolha
   * automática entre os drivers que o SDL do device expõe. */
  {
    int ndrv = SDL_GetNumAudioDrivers();
    char list[256]; size_t L = 0; list[0] = 0;
    for (int i = 0; i < ndrv; i++) {
      const char *n = SDL_GetAudioDriver(i);
      if (n && L + strlen(n) + 2 < sizeof(list))
        L += (size_t)snprintf(list + L, sizeof(list) - L, "%s ", n);
    }
    const char *cur0 = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "opensles_shim: audio drivers=[ %s] default=%s\n",
            list, cur0 ? cur0 : "?");

    /* (1) default escolhido pelo SDL do device. Rejeita "dummy" e "disk":
       ambos ABREM mas nao tocam ("disk" grava sdlaudio.raw num arquivo -> foi a
       causa do "sem som" num device que caiu de pipewire p/ disk). */
    char failed_drv[32]; failed_drv[0] = 0;
    if (cur0 && strcmp(cur0, "dummy") != 0 && strcmp(cur0, "disk") != 0) {
      g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
      if (g_audio_dev)
        fprintf(stderr, "opensles_shim: audio aberto (auto) %dHz %dch samples=%d driver=%s\n",
                have.freq, have.channels, have.samples, cur0);
      else {
        strncpy(failed_drv, cur0, sizeof(failed_drv) - 1);
        failed_drv[sizeof(failed_drv) - 1] = 0;
        fprintf(stderr, "opensles_shim: open auto falhou (driver=%s: %s) -> varrendo\n",
                cur0, SDL_GetError());
      }
    }

    /* (2) fallback: 1º driver que realmente abre o device */
    for (int i = 0; !g_audio_dev && i < ndrv; i++) {
      const char *name = SDL_GetAudioDriver(i);
      if (!name || strcmp(name, "dummy") == 0 || strcmp(name, "disk") == 0) continue;
      if (failed_drv[0] && strcmp(name, failed_drv) == 0) continue;
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      setenv("SDL_AUDIODRIVER", name, 1); /* seleciona ESTE pro próximo init */
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "opensles_shim: init driver=%s falhou: %s\n", name, SDL_GetError());
        continue;
      }
      const char *cur = SDL_GetCurrentAudioDriver();
      if (!cur || strcmp(cur, name) != 0) continue;
      g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
      if (g_audio_dev) {
        fprintf(stderr, "opensles_shim: audio aberto (fallback) %dHz %dch samples=%d driver=%s\n",
                have.freq, have.channels, have.samples, name);
        break;
      }
      fprintf(stderr, "opensles_shim: driver=%s nao abriu: %s\n", name, SDL_GetError());
    }
    if (!g_audio_dev) unsetenv("SDL_AUDIODRIVER");
  }

  if (g_audio_dev == 0) {
    fprintf(stderr, "opensles_shim: NENHUM driver de audio abriu -> jogo segue MUDO\n");
    g_audio_initialized = 1;
    return;
  }
  SDL_PauseAudioDevice(g_audio_dev, 0);
  g_audio_initialized = 1;
  /* 🔊 PUMP THREAD: no Android real o OpenSLES chama os bq callbacks de uma
   * thread PRÓPRIA. Bombear só do event-loop (1x/frame) deixa o ring secar
   * entre frames -> underrun constante (causa do áudio engasgado). Thread
   * dedicada a ~4ms mantém o ring cheio. SONIC_NO_AUDIOPUMP=1 desliga. */
  if (!getenv("SONIC_NO_AUDIOPUMP")) {
    static int started = 0;
    if (!started) {
      started = 1;
      pthread_t t;
      extern void *sl_pump_thread_fn(void *);
      pthread_create(&t, NULL, sl_pump_thread_fn, NULL);
      debugPrintf("opensles_shim: pump thread iniciada (4ms)\n");
    }
  }
}
void *sl_pump_thread_fn(void *a) {
  (void)a;
  for (;;) {
    opensles_shim_pump_callbacks();
    usleep(4000);
  }
  return NULL;
}

/* Reset player metadata without touching the 4MB ring buffer.
 * The ring head/tail tracking ensures we never read unwritten data. */
static void player_reset_meta(AudioPlayer *p) {
  p->ring_head = 0;
  p->ring_tail = 0;
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->queue_capacity = 0;
  p->callback = NULL;
  p->callback_context = NULL;
  p->enqueued_since_cb = 0;
  p->last_enqueue_size = 0;
  p->play_callback = NULL;
  p->play_callback_context = NULL;
  p->play_event_mask = 0;
  p->enqueue_counter = 0;
  p->debug_enqueue_logs = 0;
  p->debug_callback_logs = 0;
  p->debug_play_callback_logs = 0;
  p->ever_enqueued = 0;
  p->headatend_fired = 0;
  p->decoder_done = 0;
  p->cb_noenq = 0;
  p->cb_dead = 0;
  p->underrun_count = 0;
  p->fadeout_count = 0;
  p->stopping = 0;
  p->frames_played = 0;
  p->play_state = SL_PLAYSTATE_STOPPED;
  p->volume = 1.0f;
  p->active = 1;
  p->played_bytes = 0;
  p->num_channels = 0;
  p->sample_rate = 0;
  p->bits_per_sample = 0;
}

/* Allocate a player */
static AudioPlayer *alloc_player(void) {
  pthread_mutex_lock(&g_players_lock);
  /* 1. Find inactive player */
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g_players[i].active) {
      AudioPlayer *p = &g_players[i];
      player_reset_meta(p);
      pthread_mutex_unlock(&g_players_lock);
      /* debugPrintf("opensles_shim: allocated player %d\n", i); */
      return p;
    }
  }
  /* 2. Recycle stopped + drained player */
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->play_state == SL_PLAYSTATE_STOPPED &&
        ring_readable(p) == 0 &&
        p->queued_count == 0) {
      player_reset_meta(p);
      pthread_mutex_unlock(&g_players_lock);
      /* debugPrintf("opensles_shim: recycled player %d\n", i); */
      return p;
    }
  }
  /* 3. Recycle any stopped player */
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->play_state == SL_PLAYSTATE_STOPPED) {
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      player_reset_meta(p);
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      pthread_mutex_unlock(&g_players_lock);
      /* debugPrintf("opensles_shim: force-recycled stopped player %d\n", i); */
      return p;
    }
  }
  /* 4. Force-kill oldest playing player (last resort) */
  {
    int oldest = -1;
    uint64_t most_played = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      AudioPlayer *p = &g_players[i];
      if (p->played_bytes >= most_played) {
        most_played = p->played_bytes;
        oldest = i;
      }
    }
    if (oldest >= 0) {
      AudioPlayer *p = &g_players[oldest];
      debugPrintf("opensles_shim: WARNING: force-killing player %d (state=%u played=%llu)\n",
                  oldest, p->play_state, (unsigned long long)p->played_bytes);
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->play_state = SL_PLAYSTATE_STOPPED;
      player_reset_meta(p);
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      pthread_mutex_unlock(&g_players_lock);
      return p;
    }
  }
  pthread_mutex_unlock(&g_players_lock);
  debugPrintf("opensles_shim: FATAL: no player slots at all!\n");
  return NULL;
}

/* SLPlayItf methods */
static SLresult play_GetDuration(void *self, SLmillisecond *pMsec) {
  (void)self;
  if (pMsec) *pMsec = SL_TIME_UNKNOWN;
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPosition(void *self, SLmillisecond *pMsec) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      uint64_t position = 0;
      uint32_t channels = p->num_channels ? p->num_channels : 2;
      uint32_t sample_rate = p->sample_rate ? p->sample_rate : 44100;
      uint32_t bytes_per_frame = channels * sizeof(int16_t);
      if (bytes_per_frame != 0 && sample_rate != 0) {
        uint64_t frames = p->played_bytes / bytes_per_frame;
        position = (frames * 1000ULL) / sample_rate;
      }
      if (pMsec) *pMsec = (SLmillisecond)position;
      return SL_RESULT_SUCCESS;
    }
  }
  if (pMsec) *pMsec = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      /* debugPrintf("opensles_shim: player %d SetPlayState(%u -> %u)\n",
                  i, p->play_state, state); */
      /* DEFAULT OFF (v4, 2026-06-15): fade-out no STOP volta ao corte seco da v1.
       * O caminho stopping podia ENGASGAR um player em PLAYING p/ sempre se o ring
       * drenasse p/ um resto < 1 frame (src_frames_got==0 -> 'continue' ANTES do
       * finalize) -> slot preso, nunca recicla. v1 (corte seco) não tem esse risco.
       * Opt-in p/ debug do anti-click. */
      static int nofadestop = -1;
      if (nofadestop < 0) nofadestop = getenv("SONIC_FADEOUT_STOP") ? 0 : 1;
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      if (state == SL_PLAYSTATE_STOPPED && p->play_state == SL_PLAYSTATE_PLAYING
          && !nofadestop && ring_readable(p) > 0) {
        /* anti-click: em vez de cortar seco (zera ring), marca stopping -> o mixer
         * rampa 1->0 no proximo buffer e SO entao para de fato. NAO mexe no ring
         * nem no play_state aqui (segue PLAYING durante o fade). */
        p->stopping = 1;
      } else {
        if (state == SL_PLAYSTATE_STOPPED && p->play_state != SL_PLAYSTATE_STOPPED) {
          p->headatend_fired = 0;
          p->decoder_done = 0;
          p->ring_head = 0;
          p->ring_tail = 0;
          queue_reset(p);
        }
        if (state == SL_PLAYSTATE_PLAYING && p->play_state != SL_PLAYSTATE_PLAYING) {
          p->frames_played = 0;
          p->underrun_count = 0;
          p->fadeout_count = 0;
          p->stopping = 0;
          /* reuse do pool: o slot vai tocar um SOM NOVO -> des-envenena o callback
           * (se SONIC_AUDIO_CBGUARD estiver ligado, sem isso o slot ficava morto
           * p/ sempre). Garante que cb_dead nunca persista entre sons no mesmo player. */
          p->cb_dead = 0;
          p->cb_noenq = 0;
        }
        p->play_state = state;
      }
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      if (pState) *pState = g_players[i].play_state;
      return SL_RESULT_SUCCESS;
    }
  }
  if (pState) *pState = SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}

static SLresult play_RegisterCallback(void *self, void *callback, void *ctx) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      /* uintptr_t ra = (uintptr_t)__builtin_return_address(0); */
      g_players[i].play_callback = (void (*)(void *, void *, SLuint32))callback;
      g_players[i].play_callback_context = ctx;
      /* if (text_base && ra >= (uintptr_t)text_base &&
          ra < (uintptr_t)text_base + text_size) {
        debugPrintf("opensles_shim: player %d play callback registered=%p ctx=%p caller=game.so+0x%lx\n",
                    i, callback, ctx, (unsigned long)(ra - (uintptr_t)text_base));
      } else {
        debugPrintf("opensles_shim: player %d play callback registered=%p ctx=%p caller=%p\n",
                    i, callback, ctx, (void *)ra);
      } */
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetCallbackEventsMask(void *self, SLuint32 eventFlags) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      g_players[i].play_event_mask = eventFlags;
      /* debugPrintf("opensles_shim: player %d play event mask=0x%x\n", i, eventFlags); */
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

/* SLVolumeItf methods */
static SLresult volume_SetVolumeLevel(void *self, SLmillibel level) {
  float linear;
  if (level <= -9600) linear = 0.0f;
  else linear = powf(10.0f, level / 2000.0f);

  /* Clamp insane values */
  if (linear > 2.0f) {
    debugPrintf("opensles_shim: WARNING: SetVolumeLevel level=%d -> linear=%f, clamping to 1.0\n",
                (int)level, linear);
    linear = 1.0f;
  }

  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].volume_ptr == itf_ptr) {
      /* debugPrintf("opensles_shim: player %d SetVolumeLevel(%d) -> %f\n", i, (int)level, linear); */
      g_players[i].volume = linear;
      return SL_RESULT_SUCCESS;
    }
  }
  debugPrintf("opensles_shim: WARNING: SetVolumeLevel - no matching player for itf=%p\n", self);
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetVolumeLevel(void *self, SLmillibel *pLevel) {
  (void)self;
  if (pLevel) *pLevel = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetMaxVolumeLevel(void *self, SLmillibel *pMaxLevel) {
  (void)self;
  if (pMaxLevel) *pMaxLevel = 0;
  return SL_RESULT_SUCCESS;
}

/* SLBufferQueueItf methods */
static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      /* float32 (Oboe PCM_EX) -> S16 antes do ring (resto do shim é S16) */
      static int16_t conv_buf[16384];
      if (p->is_float) {
        uint32_t n = size / 4;
        if (n > sizeof(conv_buf) / 2) n = sizeof(conv_buf) / 2;
        const float *f = (const float *)pBuffer;
        for (uint32_t k = 0; k < n; k++) {
          float v = f[k];
          if (v > 1.0f) v = 1.0f;
          if (v < -1.0f) v = -1.0f;
          conv_buf[k] = (int16_t)(v * 32767.0f);
        }
        pBuffer = conv_buf;
        size = n * 2;
      }
      uint32_t written = ring_write(p, pBuffer, size);
      if (written != size) {
        debugPrintf("opensles_shim: WARNING: truncated enqueue for player %d (%u/%u bytes)\n",
                    i, written, size);
      }
      queue_push(p, written);
      p->enqueued_since_cb += written;
      if (written > 0) {
        p->last_enqueue_size = written;
        p->enqueue_counter++;
        p->ever_enqueued = 1;
        /* if (p->debug_enqueue_logs < 16 || p->enqueue_counter % 64 == 0) {
          debugPrintf("opensles_shim: player %d enqueue size=%u written=%u readable=%u counter=%u\n",
                      i, size, written, ring_readable(p), p->enqueue_counter);
          p->debug_enqueue_logs++;
        } */
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      /* debugPrintf("opensles_shim: player %d BufferQueue Clear\n", i); */
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->ring_head = 0;
      p->ring_tail = 0;
      queue_reset(p);
      p->enqueued_since_cb = 0;
      p->enqueue_counter = 0;
      p->ever_enqueued = 0;
      p->headatend_fired = 0;
      p->decoder_done = 0;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (pState) {
        SLuint32 *state = (SLuint32 *)pState;
        uint32_t play_index = 0;
        uint32_t capacity = p->queue_capacity ? p->queue_capacity : 1;
        if (p->queued_count > 0) {
          play_index = p->queued_head_index % capacity;
        }
        state[0] = p->queued_count;
        state[1] = play_index;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback callback, void *pContext) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      /* uintptr_t ra = (uintptr_t)__builtin_return_address(0); */
      p->callback = callback;
      p->callback_context = pContext;
      /* if (text_base && ra >= (uintptr_t)text_base &&
          ra < (uintptr_t)text_base + text_size) {
        debugPrintf("opensles_shim: player %d buffer callback registered=%p ctx=%p caller=game.so+0x%lx\n",
                    i, callback, pContext, (unsigned long)(ra - (uintptr_t)text_base));
      } else {
        debugPrintf("opensles_shim: player %d buffer callback registered=%p ctx=%p caller=%p\n",
                    i, callback, pContext, (void *)ra);
      } */
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState_or_RegisterCallback(void *self, void *arg1, void *arg2) {
  uintptr_t maybe_callback = (uintptr_t)arg1;
  uintptr_t text = (uintptr_t)text_base;

  if (text_base && maybe_callback >= text && maybe_callback < text + text_size) {
    return bq_RegisterCallback(self, (slBufferQueueCallback)arg1, arg2);
  }

  return bq_GetState(self, arg1);
}

/* Stub for unused interfaces */
static SLresult stub_success(void) { return SL_RESULT_SUCCESS; }

/* Player object methods */
static SLresult player_Realize(void *self, SLBoolean async) {
  (void)self; (void)async;
  return SL_RESULT_SUCCESS;
}

static SLresult player_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (iid == sl_IID_PLAY) {
        /* debugPrintf("opensles_shim: player %d GetInterface(PLAY)\n", i); */
        *pInterface = &p->play_ptr;
      } else if (iid == sl_IID_VOLUME) {
        /* debugPrintf("opensles_shim: player %d GetInterface(VOLUME)\n", i); */
        *pInterface = &p->volume_ptr;
      } else if (iid == sl_IID_BUFFERQUEUE) {
        /* debugPrintf("opensles_shim: player %d GetInterface(BUFFERQUEUE)\n", i); */
        *pInterface = &p->bq_ptr;
      } else if (iid == sl_IID_EFFECTSEND) {
        /* debugPrintf("opensles_shim: player %d GetInterface(EFFECTSEND)\n", i); */
        *pInterface = &p->effectsend_ptr;
      } else {
        /* debugPrintf("opensles_shim: player %d GetInterface(unknown=%p)\n", i, iid); */
        *pInterface = &p->effectsend_ptr;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->play_state = SL_PLAYSTATE_STOPPED;
      p->active = 0;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return;
    }
  }
}

/* Setup player vtables */
static void setup_player_vtables(AudioPlayer *p) {
  for (int i = 0; i < 8; i++) p->obj_vtable[i] = (void *)stub_success;
  p->obj_vtable[0] = (void *)player_Realize;
  p->obj_vtable[3] = (void *)player_GetInterface;
  p->obj_vtable[6] = (void *)player_Destroy;
  p->obj_ptr = p->obj_vtable;

  for (int i = 0; i < 8; i++) p->play_vtable[i] = (void *)stub_success;
  p->play_vtable[0] = (void *)play_SetPlayState;
  p->play_vtable[1] = (void *)play_GetPlayState;
  p->play_vtable[2] = (void *)play_GetDuration;
  p->play_vtable[3] = (void *)play_GetPosition;
  p->play_vtable[4] = (void *)play_RegisterCallback;
  p->play_vtable[5] = (void *)play_SetCallbackEventsMask;
  p->play_ptr = p->play_vtable;

  for (int i = 0; i < 8; i++) p->volume_vtable[i] = (void *)stub_success;
  p->volume_vtable[0] = (void *)volume_SetVolumeLevel;
  p->volume_vtable[1] = (void *)volume_GetVolumeLevel;
  p->volume_vtable[2] = (void *)volume_GetMaxVolumeLevel;
  p->volume_ptr = p->volume_vtable;

  for (int i = 0; i < 8; i++) p->bq_vtable[i] = (void *)stub_success;
  p->bq_vtable[0] = (void *)bq_Enqueue;
  p->bq_vtable[1] = (void *)bq_Clear;
  p->bq_vtable[2] = (void *)bq_GetState_or_RegisterCallback;
  p->bq_vtable[3] = (void *)bq_RegisterCallback;
  p->bq_ptr = p->bq_vtable;

  for (int i = 0; i < 8; i++) p->effectsend_vtable[i] = (void *)stub_success;
  p->effectsend_ptr = p->effectsend_vtable;
}

/* Output mix */
static void *g_outmix_vtable[8];
static void *g_outmix_ptr;

static SLresult outmix_Realize(void *self, SLBoolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLresult outmix_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  (void)self; (void)iid;
  static void *stub_vtable[8];
  static void *stub_ptr;
  static int inited = 0;
  if (!inited) {
    for (int i = 0; i < 8; i++) stub_vtable[i] = (void *)stub_success;
    stub_ptr = stub_vtable;
    inited = 1;
  }
  if (pInterface) *pInterface = &stub_ptr;
  return SL_RESULT_SUCCESS;
}

static void outmix_Destroy(void *self) { (void)self; }

static void init_outmix(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;
  for (int i = 0; i < 8; i++) g_outmix_vtable[i] = (void *)stub_success;
  g_outmix_vtable[0] = (void *)outmix_Realize;
  g_outmix_vtable[3] = (void *)outmix_GetInterface;
  g_outmix_vtable[6] = (void *)outmix_Destroy;
  g_outmix_ptr = g_outmix_vtable;
}

/* Engine interface */
static SLresult engine_CreateOutputMix(void *self, void **pMix,
                                        SLuint32 numInterfaces,
                                        const SLInterfaceID *pInterfaceIds,
                                        const SLBoolean *pInterfaceRequired) {
  (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  /* debugPrintf("opensles_shim: CreateOutputMix\n"); */
  init_outmix();
  if (pMix) *pMix = &g_outmix_ptr;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateAudioPlayer(void *self, void **pPlayer,
                                          void *pAudioSrc, void *pAudioSnk,
                                          SLuint32 numInterfaces,
                                          const SLInterfaceID *pInterfaceIds,
                                          const SLBoolean *pInterfaceRequired) {
  (void)self; (void)pAudioSnk; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;

  /* debugPrintf("opensles_shim: CreateAudioPlayer\n"); */
  ensure_audio_initialized();

  AudioPlayer *p = alloc_player();
  if (!p) {
    debugPrintf("opensles_shim: CreateAudioPlayer FATAL: no player slots\n");
    if (pPlayer) *pPlayer = NULL;
    return SL_RESULT_RESOURCE_ERROR;
  }

  if (pAudioSrc) {
    SLDataSource *src = (SLDataSource *)pAudioSrc;
    if (src->pLocator) {
      SLDataLocator_BufferQueue *loc = (SLDataLocator_BufferQueue *)src->pLocator;
      if (loc->locatorType == SL_DATALOCATOR_BUFFERQUEUE) {
        p->queue_capacity = loc->numBuffers;
      }
    }
    if (src->pFormat) {
      SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)src->pFormat;
      /* 4 = SL_ANDROID_DATAFORMAT_PCM_EX (Oboe float): mesmos 1ºs campos do
       * PCM + representation; bits=32 float -> converter p/ S16 no Enqueue. */
      if (fmt->formatType == SL_DATAFORMAT_PCM || fmt->formatType == 4) {
        p->num_channels = fmt->numChannels;
        p->sample_rate = fmt->samplesPerSec / 1000;
        p->bits_per_sample = fmt->bitsPerSample;
        p->is_float = (fmt->formatType == 4 && fmt->bitsPerSample == 32);
        debugPrintf("opensles_shim: format: %u ch, %u Hz, %u bit float=%d\n",
                    p->num_channels, p->sample_rate, p->bits_per_sample,
                    p->is_float);
      }
    }
  }

  if (p->sample_rate == 0) {
    p->num_channels = 2;
    p->sample_rate = 44100;
    p->bits_per_sample = 16;
  }

  setup_player_vtables(p);
  if (pPlayer) *pPlayer = &p->obj_ptr;
  return SL_RESULT_SUCCESS;
}

/* Engine object */
static void *g_engine_obj_vtable[8];
static void *g_engine_obj_ptr;
static void *g_engine_itf_vtable[16];
static void *g_engine_itf_ptr;

static SLresult engine_obj_Realize(void *self, SLBoolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLresult engine_obj_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  (void)self;
  if (iid == sl_IID_ENGINE) {
    if (pInterface) *pInterface = &g_engine_itf_ptr;
  } else {
    static void *stub_vtable[8];
    static void *stub_ptr;
    static int inited = 0;
    if (!inited) {
      for (int i = 0; i < 8; i++) stub_vtable[i] = (void *)stub_success;
      stub_ptr = stub_vtable;
      inited = 1;
    }
    if (pInterface) *pInterface = &stub_ptr;
  }
  return SL_RESULT_SUCCESS;
}

static void engine_obj_Destroy(void *self) {
  (void)self;
  if (g_audio_dev) {
    SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    g_audio_initialized = 0;
  }
}

static void init_engine(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;

  for (int i = 0; i < 8; i++) g_engine_obj_vtable[i] = (void *)stub_success;
  g_engine_obj_vtable[0] = (void *)engine_obj_Realize;
  g_engine_obj_vtable[3] = (void *)engine_obj_GetInterface;
  g_engine_obj_vtable[6] = (void *)engine_obj_Destroy;
  g_engine_obj_ptr = g_engine_obj_vtable;

  for (int i = 0; i < 16; i++) g_engine_itf_vtable[i] = (void *)stub_success;
  g_engine_itf_vtable[2] = (void *)engine_CreateAudioPlayer;
  g_engine_itf_vtable[7] = (void *)engine_CreateOutputMix;
  g_engine_itf_ptr = g_engine_itf_vtable;
}

/* Public API */
static void opensles_pump_locked(void);
static pthread_mutex_t g_pump_lock = PTHREAD_MUTEX_INITIALIZER;
void opensles_shim_pump_callbacks(void) {
  /* pode ser chamada do event-loop E da pump-thread: serializa; se já tem
   * alguém bombeando, pula (sem fila — a próxima volta pega). */
  if (pthread_mutex_trylock(&g_pump_lock) != 0) return;
  opensles_pump_locked();
  pthread_mutex_unlock(&g_pump_lock);
}
static void opensles_pump_locked(void) {
  /* anti-spam: streams Oboe em modo blocking-write registram callback mas os
   * dados vêm por Enqueue; chamar o callback (até 4×/4ms) faz o Oboe spammar
   * "fireDataCallback disabled" -> storm de log/I/O -> choppy. Se o callback
   * de um player não enfileira por N chamadas, marcamos cb_dead e paramos de
   * chamá-lo (os dados continuam vindo pelo Enqueue). Gate p/ debug. */
  /* DEFAULT OFF (v4, 2026-06-15): o cb_dead matava SFX one-shot game-wide.
   * Os 16 players OpenSLES são um POOL reusado pela engine (Enqueue+SetPlayState,
   * nunca Destroy). Os dados de SFX vêm PELO callback (callback->bq_Enqueue). Quando
   * um one-shot termina, o pump chama o callback 24x sem enfileirar -> cb_dead=1 ->
   * o callback NUNCA mais é chamado naquele slot. cb_dead só zera em player_reset_meta
   * (alloc_player, que só roda no startup) -> cada slot do pool fica ENVENENADO após
   * o 1o som -> em ~2min de combate os 16 morrem -> SFX somem (loops sobrevivem pq o
   * callback deles sempre tem dado). O storm de log do Oboe já é contido pela supressão
   * (commit dfef774), então o guard não é mais necessário. Opt-in p/ debug. */
  static int cbguard = -1;
  if (cbguard < 0) cbguard = getenv("SONIC_AUDIO_CBGUARD") ? 1 : 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    uint32_t readable = ring_readable(p);
    uint32_t callback_threshold = p->last_enqueue_size;
    if (callback_threshold == 0 || callback_threshold > (RING_BUFFER_SIZE / 2)) {
      callback_threshold = RING_BUFFER_SIZE / 4;
    }
    /* Request data earlier: use 2x threshold to keep buffer fuller */
    uint32_t refill_threshold = callback_threshold * 2;
    if (refill_threshold > RING_BUFFER_SIZE / 2) refill_threshold = RING_BUFFER_SIZE / 2;

    /* Call callback multiple times to fill buffer ahead */
    int max_calls = 4;
    while (p->callback && !(cbguard && p->cb_dead) && readable <= refill_threshold && max_calls > 0) {
      uint32_t counter_before = p->enqueue_counter;
      /* if (p->debug_callback_logs < 16 || counter_before % 64 == 0) {
        debugPrintf("opensles_shim: player %d callback readable=%u threshold=%u counter=%u\n",
                    i, readable, refill_threshold, counter_before);
        p->debug_callback_logs++;
      } */
      p->callback(&p->bq_ptr, p->callback_context);

      /* anti-spam: callback não produziu dado? conta. Muitos seguidos = modo
       * blocking (dados via Enqueue) -> marca morto e para de chamar.
       * SÓ rastreia/marca quando o guard está LIGADO (opt-in), senão cb_dead
       * fica sempre 0 e o 'dead=N' do [PERFAUD] é sinal limpo de saúde. */
      if (cbguard && p->enqueue_counter == counter_before) {
        if (++p->cb_noenq >= 24 && !p->cb_dead) {
          p->cb_dead = 1;
          debugPrintf("opensles_shim: player %d callback IMPRODUTIVO (24x sem enqueue) -> cb_dead (modo blocking/Oboe; dados via Enqueue)\n", i);
        }
      } else {
        p->cb_noenq = 0;
      }

      if (p->ever_enqueued && !p->decoder_done &&
          p->enqueue_counter == counter_before) {
        p->decoder_done = 1;
        /* debugPrintf("opensles_shim: player %d decoder_done after callback readable=%u counter=%u\n",
                    i, ring_readable(p), p->enqueue_counter); */
        break;
      }
      readable = ring_readable(p);
      max_calls--;
    }

    if (!p->callback && p->ever_enqueued && !p->decoder_done &&
        p->queued_count == 0 && readable == 0) {
      p->decoder_done = 1;
      /* debugPrintf("opensles_shim: player %d decoder_done after queue drain\n", i); */
    }

    // HEADATEND: fire play callback when decoder done and ring drained
    if (p->decoder_done && !p->headatend_fired) {
      if (ring_readable(p) == 0) {
        p->headatend_fired = 1;
        if (p->play_callback && (p->play_event_mask & SL_PLAYEVENT_HEADATEND)) {
          p->play_callback(&p->play_ptr, p->play_callback_context, SL_PLAYEVENT_HEADATEND);
          /* debugPrintf("opensles_shim: player %d HEADATEND fired (underruns=%u fadeouts=%u)\n",
                      i, p->underrun_count, p->fadeout_count); */
        }
        if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
        p->play_state = SL_PLAYSTATE_STOPPED;
        queue_reset(p);
        if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      }
    }
  }
}

SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;

  /* debugPrintf("opensles_shim: slCreateEngine\n"); */
  init_engine();
  if (pEngine) *pEngine = &g_engine_obj_ptr;
  return SL_RESULT_SUCCESS;
}
