/*
 * opensles_shim.c -- OpenSL ES p/ o FF4 (Mali-450) com ÁUDIO REAL via SDL.
 *
 * Fluxo OpenSL: slCreateEngine -> Realize -> GetInterface(ENGINE) ->
 * CreateOutputMix -> CreateAudioPlayer(src=SimpleBufferQueue+PCM) -> Realize ->
 * GetInterface(PLAY / ANDROIDSIMPLEBUFFERQUEUE) -> RegisterCallback -> Enqueue.
 *
 * Emulação: CreateAudioPlayer lê o SLDataFormat_PCM (canais/taxa/bits), abre SDL
 * audio em modo fila. Enqueue(pcm) -> SDL_QueueAudio. Uma thread-pump chama o
 * callback do BufferQueue quando a fila do SDL baixa (a engine enfileira o próximo).
 *
 * SL_IID_* são VARIÁVEIS-PONTEIRO distintas (engine/play/bq) p/ GetInterface
 * diferenciar; imports.c aponta os SL_IID p/ elas.
 */
#define _GNU_SOURCE
#include "util.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

/* marcadores distintos de IID (valor dos símbolos SL_IID_*) */
static unsigned char m_engine[16], m_play[16], m_bq[16];
void *ff4a_iid_engine = &m_engine;
void *ff4a_iid_play = &m_play;
void *ff4a_iid_bq = &m_bq;

static void *g_obj_vt[16], *g_eng_vt[16], *g_gen_vt[16], *g_play_vt[16], *g_bq_vt[16];
static void *g_obj_vtp = g_obj_vt, *g_eng_vtp = g_eng_vt, *g_gen_vtp = g_gen_vt;
static void *g_play_vtp = g_play_vt, *g_bq_vtp = g_bq_vt;
void *g_obj_handle = &g_obj_vtp;
void *g_eng_handle = &g_eng_vtp;
void *g_gen_handle = &g_gen_vtp;
void *g_bq_handle = &g_bq_vtp;
void *g_play_handle = &g_play_vtp;

/* ---- áudio SDL: modelo PULL (callback + ring buffer) p/ pace em tempo real ---- */
static SDL_AudioDeviceID g_dev;
static int g_rate = 44100, g_channels = 2, g_bits = 16;
static void (*g_bq_cb)(void *, void *);
static void *g_bq_ctx;
static pthread_t g_pump;
static volatile int g_pump_run;

#define RING_CAP (256 * 1024) /* ~1s @32kHz stereo 16bit */
static unsigned char g_ring[RING_CAP];
static volatile int g_rhead, g_rtail; /* tail=escrita(Enqueue), head=leitura(SDL cb) */
static pthread_mutex_t g_rmx = PTHREAD_MUTEX_INITIALIZER;

static int ring_avail(void) {
  int d = g_rtail - g_rhead;
  return d < 0 ? d + RING_CAP : d;
}
/* SDL puxa aqui em TEMPO REAL (dita o ritmo). */
static void sdl_audio_cb(void *u, Uint8 *out, int len) {
  (void)u;
  pthread_mutex_lock(&g_rmx);
  int av = ring_avail();
  int n = len < av ? len : av;
  for (int i = 0; i < n; i++) {
    out[i] = g_ring[g_rhead];
    g_rhead = (g_rhead + 1) % RING_CAP;
  }
  pthread_mutex_unlock(&g_rmx);
  if (n < len) SDL_memset(out + n, 0, len - n); /* silêncio se faltar */
}
/* Enqueue escreve no ring (dropa o excedente p/ não estourar). */
static void ring_write(const unsigned char *b, int size) {
  pthread_mutex_lock(&g_rmx);
  int free_ = RING_CAP - 1 - ring_avail();
  int n = size < free_ ? size : free_;
  for (int i = 0; i < n; i++) {
    g_ring[g_rtail] = b[i];
    g_rtail = (g_rtail + 1) % RING_CAP;
  }
  pthread_mutex_unlock(&g_rmx);
}

/* pump: alimenta a engine só quando o ring baixa (paceado pelo consumo real) */
static void *pump_thread(void *arg) {
  (void)arg;
  int target = g_rate * g_channels * (g_bits / 8) * 3 / 10; /* ~0.3s de folga */
  while (g_pump_run) {
    /* alimenta em rajada só até o alvo, depois dorme (pace pelo consumo real do SDL) */
    while (g_pump_run && g_bq_cb && ring_avail() < target)
      g_bq_cb(g_bq_handle, g_bq_ctx); /* engine -> bq_Enqueue -> ring_write */
    usleep(5000); /* ~5ms: SDL drena ~640B; evita spin-call/over-feed */
  }
  return NULL;
}

static int sl_ok() { return 0; }
static void sl_void() {}

/* ---- SLObjectItf ---- */
static int obj_Realize(void *s, int a) { (void)s; (void)a; return 0; }
static int obj_GetState(void *s, unsigned *p) { (void)s; if (p) *p = 2; return 0; }
static int obj_GetInterface(void *self, const void *iid, void *pItf) {
  (void)self;
  void *h = g_gen_handle;
  if (iid == m_engine) h = g_eng_handle;
  else if (iid == m_play) h = g_play_handle;
  else if (iid == m_bq) h = g_bq_handle;
  if (pItf) *(void **)pItf = h;
  return 0;
}

/* ---- SLEngineItf ---- */
static int eng_CreateAudioPlayer(void *self, void *pPlayer, void *pSrc, void *pSnk,
                                 unsigned n, const void *ids, const void *req) {
  (void)self; (void)pSnk; (void)n; (void)ids; (void)req;
  /* pSrc = SLDataSource{ void* pLocator; void* pFormat }; pFormat = SLDataFormat_PCM
   * uint32[]: [0]=type [1]=channels [2]=samplesPerSec(milliHz) [3]=bitsPerSample */
  if (pSrc) {
    void **ds = (void **)pSrc;
    unsigned *fmt = (unsigned *)ds[1];
    if (fmt && fmt[0] == 2 /* SL_DATAFORMAT_PCM */) {
      g_channels = (int)fmt[1] ? (int)fmt[1] : 2;
      g_rate = (int)(fmt[2] / 1000) ? (int)(fmt[2] / 1000) : 44100;
      g_bits = (int)fmt[3] ? (int)fmt[3] : 16;
    }
  }
  if (pPlayer) *(void **)pPlayer = g_obj_handle;
  debugPrintf("[SL] CreateAudioPlayer: %dHz %dch %dbit\n", g_rate, g_channels, g_bits);
  return 0;
}
static int eng_CreateOutputMix(void *s, void *pMix, unsigned n, const void *i, const void *r) {
  (void)s; (void)n; (void)i; (void)r;
  if (pMix) *(void **)pMix = g_obj_handle;
  return 0;
}
static int eng_CreateAudioRecorder(void *s, void *pRec, void *a, void *b, unsigned n,
                                   const void *i, const void *r) {
  (void)s; (void)a; (void)b; (void)n; (void)i; (void)r;
  if (pRec) *(void **)pRec = g_obj_handle;
  return 0;
}

/* ---- SLPlayItf: SetPlayState(0) inicia o device ---- */
static int play_SetPlayState(void *self, unsigned state) {
  (void)self;
  if (!g_dev) return 0;
  SDL_PauseAudioDevice(g_dev, state == 3 /* SL_PLAYSTATE_PLAYING */ ? 0 : 1);
  return 0;
}

/* ---- SLAndroidSimpleBufferQueueItf: Enqueue(0), RegisterCallback(3) ---- */
static pthread_mutex_t g_openmx = PTHREAD_MUTEX_INITIALIZER;
static int bq_Enqueue(void *self, const void *buf, unsigned size) {
  (void)self;
  /* open do device serializado: sem o lock, engine e pump entravam JUNTOS no
   * lazy-open -> DOIS devices SDL drenando o mesmo ring -> consumo 2x tempo
   * real -> musica em fast-forward picotado ("audio acelerado"). */
  pthread_mutex_lock(&g_openmx);
  if (!g_dev) {
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = g_rate;
    want.format = (g_bits == 8) ? AUDIO_U8 : AUDIO_S16SYS;
    want.channels = (Uint8)g_channels;
    want.samples = 1024;
    want.callback = sdl_audio_cb; /* modo PULL (tempo real) */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_dev) {
      g_rate = have.freq; g_channels = have.channels;
      SDL_PauseAudioDevice(g_dev, 0);
      const char *drv = SDL_GetCurrentAudioDriver();
      debugPrintf("[SL] SDL audio aberto: %dHz %dch driver=%s\n", g_rate,
                  g_channels, drv ? drv : "?");
    } else {
      debugPrintf("[SL] SDL_OpenAudioDevice FALHOU: %s\n", SDL_GetError());
    }
  }
  pthread_mutex_unlock(&g_openmx);
  if (g_dev && buf && size) ring_write((const unsigned char *)buf, (int)size);
  static unsigned long nq = 0, bytes = 0;
  nq++; bytes += size;
  if (getenv("FF4A_AUDIO_DEBUG") && (nq % 200) == 0)
    debugPrintf("[SL] Enqueue #%lu total=%luKB ring=%dB\n", nq, bytes / 1024,
                ring_avail());
  return 0;
}
static int bq_RegisterCallback(void *self, void *cb, void *ctx) {
  (void)self;
  g_bq_cb = (void (*)(void *, void *))cb;
  g_bq_ctx = ctx;
  if (!g_pump_run) {
    g_pump_run = 1;
    pthread_create(&g_pump, NULL, pump_thread, NULL);
  }
  debugPrintf("[SL] BufferQueue RegisterCallback\n");
  return 0;
}
static int bq_GetState(void *self, void *pState) {
  (void)self;
  if (pState) *(unsigned *)pState = 0;
  return 0;
}

static int g_init = 0;
static void build_vtables(void) {
  if (g_init) return;
  for (int i = 0; i < 16; i++) {
    g_obj_vt[i] = g_eng_vt[i] = g_gen_vt[i] = g_play_vt[i] = g_bq_vt[i] = (void *)sl_ok;
  }
  g_obj_vt[0] = (void *)obj_Realize;
  g_obj_vt[2] = (void *)obj_GetState;
  g_obj_vt[3] = (void *)obj_GetInterface;
  g_obj_vt[5] = (void *)sl_void;
  g_obj_vt[6] = (void *)sl_void;
  g_eng_vt[2] = (void *)eng_CreateAudioPlayer;
  g_eng_vt[3] = (void *)eng_CreateAudioRecorder;
  g_eng_vt[7] = (void *)eng_CreateOutputMix;
  g_play_vt[0] = (void *)play_SetPlayState;
  g_bq_vt[0] = (void *)bq_Enqueue;
  g_bq_vt[2] = (void *)bq_GetState;
  g_bq_vt[3] = (void *)bq_RegisterCallback;
  g_init = 1;
}

int slCreateEngine(void *pEngine, unsigned numOpt, const void *opt, unsigned numItf,
                   const void *ids, const void *req) {
  (void)numOpt; (void)opt; (void)numItf; (void)ids; (void)req;
  build_vtables();
  if (pEngine) *(void **)pEngine = g_obj_handle;
  debugPrintf("[SL] slCreateEngine OK (áudio SDL)\n");
  return 0;
}
