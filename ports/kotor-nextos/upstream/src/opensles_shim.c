/*
 * opensles_shim.c -- OpenSL ES to SDL2 audio bridge
 *
 * Translates OpenSL ES BufferQueue audio players into SDL2 audio output.
 * Lock-free SPSC ring buffers between game thread and SDL audio thread.
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "opensles_shim.h"
#include "kotor_framework.h"
#include "so_util.h"
#include "util.h"

/* musica do jogo = MP3 via SL_DATALOCATOR_ANDROIDFD + MIME (o OpenSL real
 * decodifica sozinho); decodificamos com minimp3 no pump thread. */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include <fcntl.h>
#include <sys/stat.h>

/* --------------------------------------------------------- volume do sistema
 *
 * "Plano C" -- portado de sonic4ep2-nextos/src/sonic_audio.c (port AArch64 V6,
 * aprovado pelos testers em muOS/ROCKNIX/R36S).  O codigo e' arch-neutro: le
 * arquivo e multiplica um float, nada de aarch64 atravessa.
 *
 * Motivo (documentado em suportando_outros_devices/matriz-devices.md): no modo
 * "auto" o batocera/Knulli faz `rm -f /userdata/system/.asoundrc`, entao nao
 * existe softvol pros botoes de volume controlarem.  O card cru abre com o
 * volume travado, e os botoes -- que continuam funcionando e atualizando o
 * estado do sistema -- nao chegam no nosso stream.  A resposta certa ali e'
 * ganho por software seguindo o volume do sistema.
 *
 * OPT-IN (KOTOR_SYSVOL=1, ligado pelo adapter so' quando detecta batocera/
 * Knulli): nos devices em que o volume e' do sistema (pulse/softvol) isto
 * atenuaria em dobro.
 */
static volatile float g_sys_vol = 1.0f;
static int g_sysvol_on;

/* Um NUMERO cru 0-100 (ex.: /var/run/batocera-pending-volume, atualizado
 * IMEDIATAMENTE no aperto do botao; o batocera.conf so' commita ~2s depois). */
static int sysvol_read_bare_int(const char *path, float *out) {
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  char buf[64] = {0};
  size_t n = fread(buf, 1, sizeof buf - 1, fp);
  fclose(fp);
  if (!n) return 0;
  char *p = buf;
  while (*p == ' ' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return 0;
  int v = atoi(p);
  if (v < 0 || v > 100) return 0;
  *out = (float)v / 100.0f;
  return 1;
}

/* chave audio.volume=N de um arquivo de config (batocera.conf / system.cfg) */
static int sysvol_read_conf(const char *path, const char *key, float *out) {
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  const size_t kl = strlen(key);
  int got = 0;
  char line[256];
  while (fgets(line, sizeof line, fp)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == ';') continue;
    if (strncmp(p, key, kl) != 0) continue;
    char *eq = strchr(p, '=');
    if (!eq) continue;
    int v = atoi(eq + 1);
    if (v >= 0 && v <= 100) { *out = (float)v / 100.0f; got = 1; }
  }
  fclose(fp);
  return got;
}

/* Fallback device-agnostico: mixer ALSA Master, parseando o "[NN%]". */
static int sysvol_read_amixer(float *out) {
  FILE *fp = popen("amixer sget Master 2>/dev/null", "r");
  if (!fp) return 0;
  char line[256];
  int got = 0;
  while (!got && fgets(line, sizeof line, fp)) {
    for (char *b = strchr(line, '['); b; b = strchr(b + 1, '[')) {
      char *pct = strchr(b, '%');
      if (!pct || pct < b) continue;
      int v = atoi(b + 1);
      if (v >= 0 && v <= 100) { *out = (float)v / 100.0f; got = 1; }
      break;
    }
  }
  pclose(fp);
  return got;
}

static int sysvol_read(float *out) {
  const char *override = getenv("KOTOR_VOLUME_FILE");
  const char *key = getenv("KOTOR_VOLUME_KEY");
  if (!key || !*key) key = "audio.volume";
  if (override && *override) {
    if (sysvol_read_bare_int(override, out)) return 1;
    if (sysvol_read_conf(override, key, out)) return 1;
  }
  if (sysvol_read_bare_int("/var/run/batocera-pending-volume", out)) return 1;
  static const char *confs[] = {
      "/userdata/system/batocera.conf",             /* batocera / Knulli */
      "/storage/.config/system/configs/system.cfg", /* ROCKNIX/ArchR/JELOS */
      "/storage/.config/distribution/configs/distribution.conf",
  };
  for (unsigned i = 0; i < sizeof confs / sizeof confs[0]; ++i)
    if (sysvol_read_conf(confs[i], key, out)) return 1;
  return sysvol_read_amixer(out);
}

static void *sysvol_thread(void *arg) {
  (void)arg;
  float last = -1.0f;
  for (;;) {
    float v;
    if (sysvol_read(&v)) {
      g_sys_vol = v;
      if (v != last) {
        debugPrintf("opensles_shim: volume do sistema -> %.2f\n", v);
        last = v;
      }
    }
    usleep(250000); /* ~0.25s: responde rapido ao botao */
  }
  return NULL;
}

static void sysvol_start(void) {
  const char *value = getenv("KOTOR_SYSVOL");
  if (!value || strcmp(value, "0") == 0)
    return;
  float v0;
  if (sysvol_read(&v0)) g_sys_vol = v0;
  pthread_t thread;
  if (pthread_create(&thread, NULL, sysvol_thread, NULL) != 0) {
    debugPrintf("opensles_shim: volume por software indisponivel (thread)\n");
    return;
  }
  pthread_detach(thread);
  g_sysvol_on = 1;
  debugPrintf("opensles_shim: volume por software ON (vol=%.2f)\n", g_sys_vol);
}

/* fnaSound cria 32 vozes no init; com 16 slots as vozes 17..32 reciclavam o
 * slot 0 (varias vozes do jogo apontando pro MESMO player = SFX corrompido). */
#define MAX_PLAYERS 48
/* 512KB por player: streams chegam em buffers de ~256B e SFX one-shot em
 * dezenas de KB; 4MB x 48 players tocaria ~190MB de RSS conforme o ring
 * circula (device de 1GB). Truncamento loga WARNING. */
#define RING_BUFFER_SIZE (512 * 1024)
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
/* Must stay SMALL: the game feeds streams as 4 in-flight buffers of ~256 bytes
 * (23ms of mono 22kHz audio); if one SDL mixing burst needs more than that
 * window, every callback underruns no matter how fast we pump. 512 frames =
 * 11.6ms bursts. */
#define SDL_AUDIO_SAMPLES 512

/* Interface ID storage */
static const int id_engine_tag = 1;
static const int id_play_tag = 2;
static const int id_volume_tag = 3;
static const int id_bufferqueue_tag = 4;
static const int id_effectsend_tag = 5;
static const int id_enginecap_tag = 6;
static const int id_envreverb_tag = 7;
static const int id_playbackrate_tag = 8;
static const int id_seek_tag = 9;
static const int id_record_tag = 10;
static const int id_androidconfig_tag = 11;

const SLInterfaceID sl_IID_ENGINE = &id_engine_tag;
const SLInterfaceID sl_IID_PLAY = &id_play_tag;
const SLInterfaceID sl_IID_RECORD = &id_record_tag;
const SLInterfaceID sl_IID_VOLUME = &id_volume_tag;
const SLInterfaceID sl_IID_BUFFERQUEUE = &id_bufferqueue_tag;
const SLInterfaceID sl_IID_ANDROIDCONFIGURATION = &id_androidconfig_tag;
const SLInterfaceID sl_IID_EFFECTSEND = &id_effectsend_tag;
const SLInterfaceID sl_IID_ENGINECAPABILITIES = &id_enginecap_tag;
const SLInterfaceID sl_IID_ENVIRONMENTALREVERB = &id_envreverb_tag;
const SLInterfaceID sl_IID_PLAYBACKRATE = &id_playbackrate_tag;
const SLInterfaceID sl_IID_SEEK = &id_seek_tag;

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
  volatile uint32_t buffers_completed; /* cumulative fully-consumed buffers (mixer) */
  volatile uint32_t callbacks_fired;   /* completion callbacks delivered (pump)  */
  uint32_t debug_enqueue_logs;
  uint32_t debug_callback_logs;
  uint32_t debug_play_callback_logs;
  int ever_enqueued;
  int headatend_fired;
  int decoder_done;
  volatile uint32_t empty_polls; /* consecutive dry callback polls (refill thread) */
  volatile uint32_t underrun_count;
  volatile uint32_t fadeout_count;
  volatile uint32_t frames_played;  /* total output frames mixed, for fade-in */

  volatile SLuint32 play_state;
  float volume;
  int active;
  uint64_t played_bytes;

  SLuint32 num_channels;
  SLuint32 sample_rate;
  SLuint32 bits_per_sample;

  /* player FD+MIME (musica mp3) */
  int is_fd;
  int fd;                    /* dup() do fd do jogo */
  uint64_t fd_off, fd_len;   /* janela do arquivo */
  uint64_t fd_pos;           /* posicao de leitura dentro da janela */
  mp3dec_ex_t *mp3;
  mp3dec_io_t mp3io;
  int mp3_open_failed;
  volatile int loop_enabled;
  volatile int fd_restart;   /* STOPPED->PLAYING: pump faz seek(0) */

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
  void *seek_vtable[8];
  void *seek_ptr;
} AudioPlayer;

static AudioPlayer g_players[MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_initialized = 0;
/* serializes ring writers: game threads (priming) vs audio thread (refill) */
static SDL_SpinLock g_enqueue_lock = 0;
/* serializa acesso ao estado mp3 dos players FD (pump decodifica; game thread
 * reseta/destroi) */
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;

static int audio_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("KOTOR_AUDIO_TRACE") != NULL;
  return enabled;
}

static void queue_reset(AudioPlayer *p) {
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->played_bytes = 0;
  p->buffers_completed = 0;
  p->callbacks_fired = 0;
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
    p->buffers_completed++; /* drives one completion callback (refill thread) */
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

/* Buffer-queue refill runs on the SDL audio thread (matching real OpenSL ES,
 * whose callbacks fire from the system audio thread). The device lock is
 * already held inside the callback, so shim entry points reached from the
 * game's callback must not re-take it. */
static SDL_threadID g_audio_cb_tid = 0;
static int on_audio_thread(void) {
  return g_audio_cb_tid != 0 && g_audio_cb_tid == SDL_ThreadID();
}
static void device_lock(void)   { if (g_audio_dev && !on_audio_thread()) SDL_LockAudioDevice(g_audio_dev); }
static void device_unlock(void) { if (g_audio_dev && !on_audio_thread()) SDL_UnlockAudioDevice(g_audio_dev); }

static size_t fd_read_cb(void *buf, size_t size, void *user) {
  AudioPlayer *p = (AudioPlayer *)user;
  uint64_t remain = p->fd_len - p->fd_pos;
  if (size > remain) size = (size_t)remain;
  if (size == 0) return 0;
  ssize_t r = pread(p->fd, buf, size, (off_t)(p->fd_off + p->fd_pos));
  if (r <= 0) return 0;
  p->fd_pos += (uint64_t)r;
  return (size_t)r;
}

static int fd_seek_cb(uint64_t position, void *user) {
  AudioPlayer *p = (AudioPlayer *)user;
  if (position > p->fd_len) return -1;
  p->fd_pos = position;
  return 0;
}

/* decodifica mp3 pro ring ate ~0.5s de folga; roda no pump thread */
static void fd_refill(AudioPlayer *p) {
  pthread_mutex_lock(&g_fd_lock);
  if (!p->active || p->mp3_open_failed || p->fd < 0) goto out;

  if (!p->mp3) {
    p->mp3 = (mp3dec_ex_t *)calloc(1, sizeof(mp3dec_ex_t));
    if (!p->mp3) { p->mp3_open_failed = 1; goto out; }
    p->mp3io.read = fd_read_cb;
    p->mp3io.read_data = p;
    p->mp3io.seek = fd_seek_cb;
    p->mp3io.seek_data = p;
    p->fd_pos = 0;
    if (mp3dec_ex_open_cb(p->mp3, &p->mp3io, MP3D_SEEK_TO_SAMPLE)) {
      debugPrintf("audio: fdplayer %ld mp3 open FALHOU (len=%llu)\n",
                  (long)(p - g_players), (unsigned long long)p->fd_len);
      free(p->mp3); p->mp3 = NULL; p->mp3_open_failed = 1;
      goto out;
    }
    p->sample_rate = p->mp3->info.hz;
    p->num_channels = p->mp3->info.channels;
    p->bits_per_sample = 16;
    debugPrintf("audio: fdplayer %ld mp3 aberto: %uHz %uch samples=%llu loop=%d\n",
                (long)(p - g_players), p->sample_rate, p->num_channels,
                (unsigned long long)p->mp3->samples, p->loop_enabled);
  }

  if (p->fd_restart) {
    mp3dec_ex_seek(p->mp3, 0);
    p->fd_restart = 0;
  }

  uint32_t frame_bytes = (p->num_channels ? p->num_channels : 2) * 2;
  uint32_t high = (p->sample_rate ? p->sample_rate : 44100) * frame_bytes / 2; /* ~0.5s */
  if (high > RING_BUFFER_SIZE / 2) high = RING_BUFFER_SIZE / 2;

  int guard = 8, dry = 0;
  while (ring_readable(p) < high && guard-- > 0 &&
         p->play_state == SL_PLAYSTATE_PLAYING) {
    int16_t buf[4608];
    size_t got = mp3dec_ex_read(p->mp3, buf, 4608);
    if (got > 0) {
      SDL_AtomicLock(&g_enqueue_lock);
      ring_write(p, buf, (uint32_t)(got * sizeof(int16_t)));
      SDL_AtomicUnlock(&g_enqueue_lock);
    }
    if (got < 4608) { /* EOF (ou erro) */
      /* SEMPRE dar loop: música de menu/mundo (maintitle) é feita pra repetir.
       * NUNCA marcar decoder_done num FD player -> o jogo nunca vê o stream
       * "acabar" naturalmente, então nunca dispara o fnaStream_Destroy que
       * crasha (deref de vtable NULL, ~86s = fim do maintitle). Trocas de
       * faixa continuam via STOP/Destroy explícito do jogo (caminhos tratados). */
      mp3dec_ex_seek(p->mp3, 0);
      if (got == 0 && ++dry > 2) break; /* arquivo vazio: não spinnar */
    } else {
      dry = 0;
    }
  }
out:
  pthread_mutex_unlock(&g_fd_lock);
}

static void refill_player(AudioPlayer *p) {
  if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) return;

  if (p->is_fd) { fd_refill(p); return; }

  /* Completion-driven, like real OpenSL ES: the buffer-queue callback fires
   * exactly once per fully-consumed buffer -- that's the signal the game's
   * feeder uses to decode + enqueue the next chunk. */
  int guard = 64;
  while (p->callback && p->callbacks_fired != p->buffers_completed && guard-- > 0) {
    p->callbacks_fired++;
    uint32_t counter_before = p->enqueue_counter;
    p->callback(&p->bq_ptr, p->callback_context);
    if (p->enqueue_counter != counter_before)
      p->empty_polls = 0;
  }

  /* Bootstrap: o jogo poe o player em PLAYING com a fila VAZIA e espera ser
   * POLLADO pra entregar os primeiros buffers (comportamento do pump do
   * lswtcs, que funciona). Sem isso a musica (p27) nunca recebe um byte:
   * completion nunca ocorre e o starvation poll exigia ever_enqueued. */
  if (p->callback && !p->ever_enqueued) {
    p->callback(&p->bq_ptr, p->callback_context);
    return; /* sem EOS antes do primeiro enqueue */
  }

  /* Starvation poll: queue drained and no completions pending -> ask anyway at
   * pump rate; ~200ms of consecutive dry asks with an empty queue = EOS. */
  if (p->callback && p->queued_count == 0 && ring_readable(p) == 0 &&
      p->ever_enqueued && !p->decoder_done) {
    uint32_t counter_before = p->enqueue_counter;
    p->callback(&p->bq_ptr, p->callback_context);
    if (p->enqueue_counter == counter_before) {
      /* ~2s: menu/level transitions stall the game-side decoder for hundreds
       * of ms; killing the stream on those gaps chopped the music. A stream
       * that is genuinely over gets stopped by the game itself anyway. */
      if (++p->empty_polls > 500) {
        p->decoder_done = 1;
      }
    } else {
      p->empty_polls = 0;
    }
  }

  if (!p->callback && p->ever_enqueued && !p->decoder_done &&
      p->queued_count == 0 && ring_readable(p) == 0) {
    p->decoder_done = 1;
  }
}

/* Dedicated refill thread: calling the game's buffer-queue callbacks from
 * inside the SDL audio callback deadlocks (SDL holds the device lock there,
 * the game callback takes engine mutexes, and game threads take those same
 * mutexes around SL calls that need the device lock). This thread invokes the
 * callbacks holding NO device lock, every ~4ms -- like the real OpenSL ES
 * notification thread. */
static pthread_t g_pump_thread;
static volatile int g_pump_running = 0;

static void *pump_thread_main(void *arg) {
  (void)arg;
  uint32_t trace_ticks = 0;
  while (g_pump_running) {
    for (int i = 0; i < MAX_PLAYERS; i++)
      refill_player(&g_players[i]);
    if (audio_trace_enabled() && ++trace_ticks >= 250) {
      trace_ticks = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        AudioPlayer *p = &g_players[i];
        if (!p->active)
          continue;
        debugPrintf("AUDSTAT p%d state=%u fd=%d %uHz/%uch vol=%.3f "
                    "ring=%u q=%u enq=%u done=%u cb=%u played=%llu underrun=%u\n",
                    i, p->play_state, p->is_fd, p->sample_rate,
                    p->num_channels, p->volume, ring_readable(p),
                    p->queued_count, p->enqueue_counter,
                    p->buffers_completed, p->callbacks_fired,
                    (unsigned long long)p->played_bytes, p->underrun_count);
      }
    }
    usleep(4000);
  }
  return NULL;
}

/* Mixes exactly out_samples int16 samples (out_samples <= SDL_AUDIO_SAMPLES*2).
 * The SDL callback below walks the whole period in blocks of this size: on
 * CFWs whose ALSA only opens with a larger period (RGB30/dArkOS negotiates
 * 1024 frames against our 512), a single capped pass filled only the first
 * half of every period and left the rest silent -- audible as choppy,
 * delayed audio with ALSA underruns. */
static void mix_block(int16_t *out, int out_samples) {
  static float mix_buf[SDL_AUDIO_SAMPLES * 2];
  memset(mix_buf, 0, out_samples * sizeof(float));

  uint32_t out_frames = out_samples / 2;

  /* Per-player temp buffer on stack */
  int16_t tmp[TMP_BUF_SAMPLES];

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
    /* ganhos altos: NextOS na TV ficava inaudivel com 0.35/0.8 + master 0.30
     * (OUT peak ~2400/32768); o soft-clip limiter segura os estouros. */
    if (src_channels == 1) {
      vol *= 0.5f;
    }

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
    /* queued_count feeds bq_GetState, which the game's stream feeder uses to
     * decide whether the queue is full; an unsynchronized decrement here races
     * queue_push and drifts the count up until the feeder starves the music. */
    SDL_AtomicLock(&g_enqueue_lock);
    queue_consume(p, got);
    SDL_AtomicUnlock(&g_enqueue_lock);
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
        mix_buf[f * 2]     += (float)tmp[f * 2]     * v;
        mix_buf[f * 2 + 1] += (float)tmp[f * 2 + 1] * v;
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
        mix_buf[f * 2]     += left * v;
        mix_buf[f * 2 + 1] += right * v;
        pos += step;
        mixed++;
      }
      p->frames_played += mixed;
    }
  }

  /* Soft-clip using tanh-style limiter - smooth, no discontinuities.
   * O ganho do sistema entra no mesmo lugar do master_gain, antes do
   * limitador: baixar o volume tambem afasta o sinal do soft-clip, que e' o
   * comportamento esperado de um controle de volume. */
  const float master_gain = g_sysvol_on ? g_sys_vol : 1.0f;
  const float threshold = 28000.0f;
  const float knee = 4000.0f;  /* transition zone */

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
}

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  g_audio_cb_tid = SDL_ThreadID();
  memset(stream, 0, len);

  int16_t *out = (int16_t *)stream;
  int remaining = len / (int)sizeof(int16_t);
  while (remaining > 0) {
    int chunk = remaining;
    if (chunk > SDL_AUDIO_SAMPLES * 2) chunk = SDL_AUDIO_SAMPLES * 2;
    chunk &= ~1; /* keep stereo frames whole */
    if (chunk == 0) break;
    mix_block(out, chunk);
    out += chunk;
    remaining -= chunk;
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
  want.samples = SDL_AUDIO_SAMPLES;
  want.callback = sdl_audio_callback;
  want.userdata = NULL;

  g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (g_audio_dev == 0) {
    debugPrintf("opensles_shim: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    g_audio_initialized = 1;
    return;
  }

  debugPrintf("opensles_shim: SDL audio opened: driver=%s %dHz %dch fmt=0x%x %d samples\n",
              SDL_GetCurrentAudioDriver(), have.freq, have.channels,
              (unsigned)have.format, have.samples);
  kotor_framework_observe_audio(g_audio_dev, &have);
  SDL_PauseAudioDevice(g_audio_dev, 0);
  g_pump_running = 1;
  pthread_create(&g_pump_thread, NULL, pump_thread_main, NULL);
  sysvol_start();
  g_audio_initialized = 1;
}

/* Reset player metadata without touching the 4MB ring buffer.
 * The ring head/tail tracking ensures we never read unwritten data. */
static void player_reset_meta(AudioPlayer *p) {
  /* limpar estado FD do ocupante anterior do slot */
  pthread_mutex_lock(&g_fd_lock);
  if (p->mp3) { mp3dec_ex_close(p->mp3); free(p->mp3); p->mp3 = NULL; }
  if (p->is_fd && p->fd >= 0) close(p->fd);
  p->is_fd = 0;
  p->fd = -1;
  p->fd_off = p->fd_len = p->fd_pos = 0;
  p->mp3_open_failed = 0;
  p->loop_enabled = 0;
  p->fd_restart = 0;
  pthread_mutex_unlock(&g_fd_lock);

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
  p->empty_polls = 0;
  p->underrun_count = 0;
  p->fadeout_count = 0;
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
      device_lock();
      player_reset_meta(p);
      device_unlock();
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
      device_lock();
      p->play_state = SL_PLAYSTATE_STOPPED;
      player_reset_meta(p);
      device_unlock();
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
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (p->is_fd && p->mp3 && p->sample_rate && p->num_channels) {
        uint64_t frames = p->mp3->samples / p->num_channels;
        if ((uintptr_t)pMsec > 0x100000)
          *pMsec = (SLmillisecond)(frames * 1000ULL / p->sample_rate);
        return SL_RESULT_SUCCESS;
      }
      break;
    }
  }
  if ((uintptr_t)pMsec > 0x100000) *pMsec = SL_TIME_UNKNOWN;
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
      if ((uintptr_t)pMsec > 0x100000) *pMsec = (SLmillisecond)position;
      return SL_RESULT_SUCCESS;
    }
  }
  if ((uintptr_t)pMsec > 0x100000) *pMsec = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (audio_trace_enabled())
        debugPrintf("audio: player %d SetPlayState %u -> %u (ring=%u q=%u)\n",
                    i, p->play_state, state, ring_readable(p),
                    p->queued_count);
      device_lock();
      if (state == SL_PLAYSTATE_STOPPED && p->play_state != SL_PLAYSTATE_STOPPED) {
        p->headatend_fired = 0;
        p->decoder_done = 0;
        p->empty_polls = 0;
        if (p->is_fd) p->fd_restart = 1; /* proximo PLAY recomeca o mp3 do zero */
        SDL_AtomicLock(&g_enqueue_lock);
        p->ring_head = 0;
        p->ring_tail = 0;
        queue_reset(p);
        SDL_AtomicUnlock(&g_enqueue_lock);
      }
      if (state == SL_PLAYSTATE_PLAYING && p->play_state != SL_PLAYSTATE_PLAYING) {
        p->frames_played = 0;
        p->underrun_count = 0;
        p->fadeout_count = 0;
      }
      /* FD player: PLAY de novo apos fim-de-faixa (mesmo sem STOPPED antes)
       * = re-tocar do inicio */
      if (p->is_fd && state == SL_PLAYSTATE_PLAYING && p->decoder_done) {
        SDL_AtomicLock(&g_enqueue_lock);
        p->ring_head = 0;
        p->ring_tail = 0;
        queue_reset(p);
        SDL_AtomicUnlock(&g_enqueue_lock);
        p->decoder_done = 0;
        p->headatend_fired = 0;
        p->fd_restart = 1;
      }
      p->play_state = state;
      device_unlock();
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      SLuint32 st = p->play_state;
      /* FD player em fim-de-faixa: REPORTAR stopped (o jogo polla isso pra
       * saber que a musica acabou e re-tocar), sem mutar estado interno nem
       * posicao -- mutacao assincrona daqui do shim causava SIGSEGV no
       * handler de fim-de-faixa do jogo. */
      if (p->is_fd && p->decoder_done && p->headatend_fired &&
          ring_readable(p) == 0)
        st = SL_PLAYSTATE_STOPPED;
      if ((uintptr_t)pState > 0x100000) *pState = st;
      return SL_RESULT_SUCCESS;
    }
  }
  if ((uintptr_t)pState > 0x100000) *pState = SL_PLAYSTATE_STOPPED;
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
        debugPrintf("opensles_shim: player %d play callback registered=%p ctx=%p caller=libTTapp.so+0x%lx\n",
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
  /* SLmillibel is 16-bit on the wire: 0x8000 is SL_MILLIBEL_MIN (mute),
   * not +32768. Sign-extend from 16 bits before converting. */
  int16_t level16 = (int16_t)(level & 0xFFFF);
  float linear;
  if (level16 <= -9600) linear = 0.0f;
  else linear = powf(10.0f, level16 / 2000.0f);

  /* Clamp insane values */
  if (linear > 2.0f) {
    debugPrintf("opensles_shim: WARNING: SetVolumeLevel level=%d -> linear=%f, clamping to 1.0\n",
                (int)level16, linear);
    linear = 1.0f;
  }

  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].volume_ptr == itf_ptr) {
      g_players[i].volume = linear;
      return SL_RESULT_SUCCESS;
    }
  }
  debugPrintf("opensles_shim: WARNING: SetVolumeLevel - no matching player for itf=%p\n", self);
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetVolumeLevel(void *self, SLmillibel *pLevel) {
  (void)self;
  if ((uintptr_t)pLevel > 0x100000) *pLevel = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetMaxVolumeLevel(void *self, SLmillibel *pMaxLevel) {
  (void)self;
  if ((uintptr_t)pMaxLevel > 0x100000) *pMaxLevel = 0;
  return SL_RESULT_SUCCESS;
}

/* SLBufferQueueItf methods */
static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      SDL_AtomicLock(&g_enqueue_lock);
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
        if (audio_trace_enabled() &&
            (p->debug_enqueue_logs < 8 || (p->enqueue_counter % 256) == 0)) {
          debugPrintf("audio: player %d Enqueue size=%u ring=%u q=%u count=%u\n",
                      i, written, ring_readable(p), p->queued_count,
                      p->enqueue_counter);
          p->debug_enqueue_logs++;
        }
      }
      SDL_AtomicUnlock(&g_enqueue_lock);
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
      device_lock();
      SDL_AtomicLock(&g_enqueue_lock);
      p->ring_head = 0;
      p->ring_tail = 0;
      queue_reset(p);
      p->enqueued_since_cb = 0;
      p->enqueue_counter = 0;
      p->ever_enqueued = 0;
      SDL_AtomicUnlock(&g_enqueue_lock);
      p->headatend_fired = 0;
      p->decoder_done = 0;
      p->empty_polls = 0;
      device_unlock();
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
      if ((uintptr_t)pState > 0x100000) {
        SLuint32 *state = (SLuint32 *)pState;
        state[0] = p->queued_count;
        /* spec: playIndex is CUMULATIVE (increments once per buffer played),
         * not modulo capacity -- feeders diff it to count completions. */
        state[1] = p->queued_head_index;
        if (p->queue_capacity && p->queued_count >= p->queue_capacity) {
          static uint32_t full_logs = 0;
          if (full_logs < 12) {
            full_logs++;
            debugPrintf("audio: player %d GetState FULL count=%u cap=%u ring=%u state=%u rate=%u ch=%u lastenq=%u\n",
                        i, p->queued_count, p->queue_capacity, ring_readable(p),
                        p->play_state, p->sample_rate, p->num_channels, p->last_enqueue_size);
          }
        }
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
      if (audio_trace_enabled())
        debugPrintf("audio: player %d RegisterBufferCallback cb=%p ctx=%p\n",
                    i, callback, pContext);
      /* uintptr_t ra = (uintptr_t)__builtin_return_address(0); */
      p->callback = callback;
      p->callback_context = pContext;
      /* if (text_base && ra >= (uintptr_t)text_base &&
          ra < (uintptr_t)text_base + text_size) {
        debugPrintf("opensles_shim: player %d buffer callback registered=%p ctx=%p caller=libTTapp.so+0x%lx\n",
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

/* SLPlaybackRateItf -- fnaSound_Init enumerates GetRateRange(index++) until it
 * finds a range covering 1000 permille or the call fails; a stub that returns
 * success without writing the out-params spins that loop forever. */
static SLresult pbrate_SetRate(void *self, SLpermille rate) {
  (void)self; (void)rate;
  return SL_RESULT_SUCCESS;
}

static SLresult pbrate_GetRate(void *self, SLpermille *pRate) {
  (void)self;
  if ((uintptr_t)pRate > 0x100000) *pRate = 1000;
  return SL_RESULT_SUCCESS;
}

static SLresult pbrate_GetProperties(void *self, SLuint32 *pProperties) {
  (void)self;
  if ((uintptr_t)pProperties > 0x100000) *pProperties = 1; /* SL_RATEPROP_NOPITCHCORAUDIO */
  return SL_RESULT_SUCCESS;
}

static SLresult pbrate_GetCapabilitiesOfRate(void *self, SLpermille rate, SLuint32 *pCapabilities) {
  (void)self; (void)rate;
  if ((uintptr_t)pCapabilities > 0x100000) *pCapabilities = 1;
  return SL_RESULT_SUCCESS;
}

static SLresult pbrate_GetRateRange(void *self, SLuint8 index, SLpermille *pMinRate,
                                     SLpermille *pMaxRate, SLpermille *pStepSize,
                                     SLuint32 *pCapabilities) {
  (void)self;
  if (index > 0) return SL_RESULT_PARAMETER_INVALID;
  if ((uintptr_t)pMinRate > 0x100000) *pMinRate = 500;
  if ((uintptr_t)pMaxRate > 0x100000) *pMaxRate = 2000;
  if ((uintptr_t)pStepSize > 0x100000) *pStepSize = 0;
  if ((uintptr_t)pCapabilities > 0x100000) *pCapabilities = 1;
  return SL_RESULT_SUCCESS;
}

static void *g_pbrate_vtable[8];
static void *g_pbrate_ptr;

/* SLSeekItf: por player. Nos FD players (mp3) o loop importa de verdade --
 * musica de menu usa SetLoop(true). BufferQueue players ignoram (loop vem do
 * jogo re-enfileirando). */
static AudioPlayer *player_from_seek_itf(void *self) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (&g_players[i].seek_ptr == itf_ptr) return &g_players[i];
  return NULL;
}

static SLresult seek_SetPosition(void *self, SLmillisecond pos, SLuint32 seekMode) {
  (void)seekMode;
  AudioPlayer *p = player_from_seek_itf(self);
  if (p && p->is_fd && pos == 0) p->fd_restart = 1; /* caso comum: rewind */
  return SL_RESULT_SUCCESS;
}

static SLresult seek_SetLoop(void *self, SLBoolean loopEnable,
                              SLmillisecond startPos, SLmillisecond endPos) {
  (void)startPos; (void)endPos;
  AudioPlayer *p = player_from_seek_itf(self);
  if (p) {
    p->loop_enabled = (loopEnable != 0);
    debugPrintf("audio: player %ld SetLoop(%d)\n", (long)(p - g_players),
                (int)(loopEnable != 0));
  }
  return SL_RESULT_SUCCESS;
}

static SLresult seek_GetLoop(void *self, SLBoolean *pLoopEnabled,
                              SLmillisecond *pStartPos, SLmillisecond *pEndPos) {
  AudioPlayer *p = player_from_seek_itf(self);
  if ((uintptr_t)pLoopEnabled > 0x100000)
    *pLoopEnabled = (p && p->loop_enabled) ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  if ((uintptr_t)pStartPos > 0x100000) *pStartPos = 0;
  if ((uintptr_t)pEndPos > 0x100000) *pEndPos = SL_TIME_UNKNOWN;
  return SL_RESULT_SUCCESS;
}

static void init_static_player_itfs(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;

  for (int i = 0; i < 8; i++) g_pbrate_vtable[i] = (void *)stub_success;
  g_pbrate_vtable[0] = (void *)pbrate_SetRate;
  g_pbrate_vtable[1] = (void *)pbrate_GetRate;
  /* [2] SetPropertyConstraints: accept anything */
  g_pbrate_vtable[3] = (void *)pbrate_GetProperties;
  g_pbrate_vtable[4] = (void *)pbrate_GetCapabilitiesOfRate;
  g_pbrate_vtable[5] = (void *)pbrate_GetRateRange;
  g_pbrate_ptr = g_pbrate_vtable;

}

/* Android-specific player configuration used by FMOD before Realize().
 * Stream type/performance hints do not affect the SDL backend, but accepting
 * SetConfiguration is required for FMOD to keep the OpenSL output selected. */
static SLresult androidconfig_SetConfiguration(void *self, const char *key,
                                                const void *value,
                                                SLuint32 value_size) {
  (void)self;
  if (audio_trace_enabled()) {
    uint32_t v = 0;
    if (value && value_size >= sizeof(v))
      memcpy(&v, value, sizeof(v));
    debugPrintf("audio: AndroidConfig Set key=%s size=%u value=%u\n",
                ((uintptr_t)key > 0x100000u) ? key : "?",
                value_size, v);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult androidconfig_GetConfiguration(void *self, const char *key,
                                                SLuint32 *value_size,
                                                void *value) {
  (void)self;
  (void)key;
  if (value_size && value && *value_size >= sizeof(uint32_t)) {
    uint32_t v = 0;
    memcpy(value, &v, sizeof(v));
    *value_size = sizeof(v);
  }
  return SL_RESULT_SUCCESS;
}

static void *g_androidconfig_vtable[2] = {
  (void *)androidconfig_SetConfiguration,
  (void *)androidconfig_GetConfiguration
};
static void *g_androidconfig_ptr = g_androidconfig_vtable;

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
      } else if (iid == sl_IID_PLAYBACKRATE) {
        debugPrintf("opensles_shim: player %d GetInterface(PLAYBACKRATE)\n", i);
        init_static_player_itfs();
        *pInterface = &g_pbrate_ptr;
      } else if (iid == sl_IID_SEEK) {
        debugPrintf("opensles_shim: player %d GetInterface(SEEK)\n", i);
        *pInterface = &p->seek_ptr;
      } else if (iid == sl_IID_ANDROIDCONFIGURATION) {
        if (audio_trace_enabled())
          debugPrintf("audio: player %d GetInterface(ANDROIDCONFIGURATION)\n", i);
        *pInterface = &g_androidconfig_ptr;
      } else {
        if (audio_trace_enabled())
          debugPrintf("audio: player %d GetInterface(unknown=%p)\n", i, iid);
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
      if (audio_trace_enabled())
        debugPrintf("audio: player %d Destroy state=%u ring=%u enq=%u played=%llu\n",
                    i, p->play_state, ring_readable(p), p->enqueue_counter,
                    (unsigned long long)p->played_bytes);
      device_lock();
      p->play_state = SL_PLAYSTATE_STOPPED;
      p->active = 0;
      device_unlock();
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

  for (int i = 0; i < 8; i++) p->seek_vtable[i] = (void *)stub_success;
  p->seek_vtable[0] = (void *)seek_SetPosition;
  p->seek_vtable[1] = (void *)seek_SetLoop;
  p->seek_vtable[2] = (void *)seek_GetLoop;
  p->seek_ptr = p->seek_vtable;
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
  debugPrintf("SL: CreateOutputMix(self=%p pMix=%p)\n", self, (void *)pMix);
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

  debugPrintf("SL: CreateAudioPlayer(self=%p pPlayer=%p pSrc=%p pSnk=%p)\n",
              self, (void *)pPlayer, pAudioSrc, pAudioSnk);
  ensure_audio_initialized();

  AudioPlayer *p = alloc_player();
  if (!p) {
    debugPrintf("opensles_shim: CreateAudioPlayer FATAL: no player slots\n");
    if (pPlayer) *pPlayer = NULL;
    return SL_RESULT_RESOURCE_ERROR;
  }

  if ((uintptr_t)pAudioSrc > 0x100000) {
    SLDataSource *src = (SLDataSource *)pAudioSrc;
    void *ploc = src->pLocator, *pfmt = src->pFormat;
    debugPrintf("SL: audioSrc pLocator=%p pFormat=%p\n", ploc, pfmt);
    if ((uintptr_t)ploc > 0x100000) {
      SLDataLocator_BufferQueue *loc = (SLDataLocator_BufferQueue *)ploc;
      if (audio_trace_enabled())
        debugPrintf("SL: locator type=0x%x arg=%u interfaces=%u\n",
                    loc->locatorType, loc->numBuffers, numInterfaces);
      if (loc->locatorType == SL_DATALOCATOR_BUFFERQUEUE) {
        p->queue_capacity = loc->numBuffers;
      } else if (loc->locatorType == 0x800007bc /* SL_DATALOCATOR_ANDROIDFD */) {
        /* { SLuint32 type; SLint32 fd; SLAint64 offset; SLAint64 length; } */
        typedef struct { SLuint32 t; int32_t fd; int64_t off; int64_t len; } AndroidFDLoc;
        AndroidFDLoc *fdloc = (AndroidFDLoc *)ploc;
        int nfd = dup(fdloc->fd);
        if (nfd >= 0) {
          uint64_t len = (uint64_t)fdloc->len;
          if (fdloc->len <= 0) { /* USE_FILE_SIZE */
            struct stat st;
            len = (fstat(nfd, &st) == 0) ? (uint64_t)st.st_size : 0;
          }
          p->is_fd = 1;
          p->fd = nfd;
          p->fd_off = (uint64_t)(fdloc->off > 0 ? fdloc->off : 0);
          p->fd_len = len;
          debugPrintf("audio: fdplayer %ld ANDROIDFD fd=%d(dup=%d) off=%llu len=%llu\n",
                      (long)(p - g_players), fdloc->fd, nfd,
                      (unsigned long long)p->fd_off, (unsigned long long)p->fd_len);
        } else {
          debugPrintf("audio: ANDROIDFD dup(%d) FALHOU\n", fdloc->fd);
        }
      }
    }
    if ((uintptr_t)pfmt > 0x100000) {
      SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)pfmt;
      if (audio_trace_enabled())
        debugPrintf("SL: format type=%u ch=%u milliHz=%u bits=%u container=%u "
                    "mask=0x%x endian=%u\n",
                    fmt->formatType, fmt->numChannels, fmt->samplesPerSec,
                    fmt->bitsPerSample, fmt->containerSize,
                    fmt->channelMask, fmt->endianness);
      if (fmt->formatType == SL_DATAFORMAT_PCM) {
        p->num_channels = fmt->numChannels;
        p->sample_rate = fmt->samplesPerSec / 1000;
        p->bits_per_sample = fmt->bitsPerSample;
      } else if (fmt->formatType == 0x1 /* SL_DATAFORMAT_MIME */) {
        const char *mime = ((const char **)pfmt)[1];
        if ((uintptr_t)mime > 0x100000)
          debugPrintf("audio: fdplayer %ld MIME=\"%s\"\n", (long)(p - g_players), mime);
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
  debugPrintf("SL: engineObj.GetInterface(iid=%p ENGINE=%p pIf=%p)\n",
              (void *)iid, (void *)sl_IID_ENGINE, (void *)pInterface);
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
  /*
   * The KOTOR/FMOD Android backend releases its temporary SL engine object
   * after creating the output players, while those players remain live and
   * continue feeding their buffer queues.  Real OpenSL keeps that native
   * output path valid for the surviving objects.  Closing our process-global
   * SDL device here silently removes the Pulse sink while gameplay continues:
   * voices disappear and dialogue advances without the spoken-line duration.
   *
   * This shim has one process-lifetime engine/device rather than independent
   * ref-counted engine instances, so defer the actual close to process exit.
   */
  debugPrintf("opensles_shim: engine Destroy deferred (audio device=%u)\n",
              (unsigned)g_audio_dev);
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
void opensles_shim_pump_callbacks(void) {
  static unsigned pump_calls = 0;
  if ((++pump_calls % 900) == 0) { /* ~30s @30fps: underrun visibility */
    for (int i = 0; i < MAX_PLAYERS; i++) {
      AudioPlayer *p = &g_players[i];
      if (p->active && p->underrun_count)
        debugPrintf("audio: player %d underruns=%u (ring=%u)\n",
                    i, p->underrun_count, ring_readable(p));
    }
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    /* ring refill now happens on the SDL audio thread (refill_player);
     * here we only run end-of-stream housekeeping that must not execute
     * inside the audio callback. */

    // HEADATEND: fire play callback when decoder done and ring drained
    if (p->decoder_done && !p->headatend_fired) {
      if (ring_readable(p) == 0) {
        p->headatend_fired = 1;
        if (p->play_callback && (p->play_event_mask & SL_PLAYEVENT_HEADATEND)) {
          p->play_callback(&p->play_ptr, p->play_callback_context, SL_PLAYEVENT_HEADATEND);
        }
        /* FD players (musica mp3): OpenSL REAL fica em PLAYING com o head
         * parado no fim -- e a posicao CONGELA. Forcar STOPPED + reset aqui
         * zerava GetPosition e mudava o estado sob os pes do jogo -> SIGSEGV
         * no handler de fim-de-faixa (~86s, fim do maintitle.mp3). Quem para/
         * recomeca e o JOGO. BufferQueue players mantem o comportamento do
         * lswtcs (fnaStream depende do STOPPED pra reciclar o stream). */
        if (!p->is_fd) {
          device_lock();
          p->play_state = SL_PLAYSTATE_STOPPED;
          SDL_AtomicLock(&g_enqueue_lock);
          queue_reset(p);
          SDL_AtomicUnlock(&g_enqueue_lock);
          device_unlock();
        }
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

  debugPrintf("SL: slCreateEngine(pEngine=%p)\n", (void *)pEngine);
  init_engine();
  if (pEngine) *pEngine = &g_engine_obj_ptr;
  return SL_RESULT_SUCCESS;
}
