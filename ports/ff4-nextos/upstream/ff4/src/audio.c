// audio — shim de OpenSL ES sobre SDL_Audio.
//
// O engine (sqexsdlib / SdSoundSystem) faz o caminho classico do Android:
//   slCreateEngine -> Realize -> GetInterface(ENGINE)
//   -> CreateOutputMix -> Realize
//   -> CreateAudioPlayer(BufferQueue -> OutputMix) -> Realize
//   -> GetInterface(PLAY | ANDROIDSIMPLEBUFFERQUEUE | VOLUME)
//   -> RegisterCallback + Enqueue; a cada buffer consumido o callback enfileira
//      o proximo.
// Recusar slCreateEngine faz o engine abortar em CoreAudioOutInit, entao a
// implementacao precisa ser real.
//
// ⚠️ UM UNICO device SDL, aberto sob mutex. Dois devices drenando o mesmo ring
// foi a causa da "musica picotada / 2x mais rapida" no FF4A.
#include <SDL2/SDL.h>
/* Liga expf/powf a versao antiga que a mesma libm do aparelho ja' exporta:
 * a glibc 2.27 republicou esses pontos de entrada, o linker escolhe o mais
 * novo e nada aqui precisa dele. Ver framework/nxabi/include/nx_symver.h. */
#include "nx_symver.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_RESULT_SUCCESS              0x00000000u
#define SL_RESULT_PARAMETER_INVALID    0x0000000Du
#define SL_OBJECT_STATE_REALIZED       0x00000002u
#define SL_PLAYSTATE_STOPPED           0x00000001u
#define SL_PLAYSTATE_PAUSED            0x00000002u
#define SL_PLAYSTATE_PLAYING           0x00000003u

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef uint8_t SLboolean;

// SLDataFormat_PCM conforme a spec (taxa em MILI-hertz).
typedef struct {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} sl_pcm_t;

typedef struct {
    void *pLocator;
    void *pFormat;
} sl_data_t;

typedef enum { OBJ_ENGINE = 1, OBJ_OUTPUTMIX, OBJ_PLAYER } obj_kind_t;

#define MAX_PLAYERS 16
#define MAX_QUEUED 8

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} sl_buffer_t;

// ⚠️ Os dois primeiros campos TEM que casar com sl_generic_t (object_vt, kind):
// o obj_get_interface recebe o handle como generico e le o kind desse offset.
typedef struct sl_player {
    const void **object_vt;   // precisa ser o 1o campo: o handle e &object_vt
    int kind;                 // OBJ_PLAYER
    const void **play_vt;
    const void **bq_vt;
    const void **volume_vt;

    int used;
    int state;                // SL_PLAYSTATE_*
    float gain;

    int channels;
    int rate;
    int bits;

    sl_buffer_t queue[MAX_QUEUED];
    int q_head, q_count;

    void (*callback)(void *bq, void *ctx);
    void *ctx;
} sl_player_t;

static sl_player_t g_players[MAX_PLAYERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_dev;
static int g_dev_rate = 44100;
static int g_dev_channels = 2;

// ---------------------------------------------------------------------------
// Mixagem: soma todos os players tocando, convertendo para o formato do device.
// ---------------------------------------------------------------------------
static void mix_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    memset(stream, 0, len);
    int frames = len / (int)(sizeof(int16_t) * g_dev_channels);
    int16_t *out = (int16_t *)stream;

    pthread_mutex_lock(&g_lock);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        sl_player_t *pl = &g_players[p];
        if (!pl->used || pl->state != SL_PLAYSTATE_PLAYING) continue;

        for (int f = 0; f < frames; f++) {
            // Puxa o proximo buffer quando o atual acaba, avisando o engine.
            while (pl->q_count > 0 && pl->queue[pl->q_head].pos >= pl->queue[pl->q_head].size) {
                pl->q_head = (pl->q_head + 1) % MAX_QUEUED;
                pl->q_count--;
                if (pl->callback) {
                    pthread_mutex_unlock(&g_lock);
                    pl->callback(&pl->bq_vt, pl->ctx);
                    pthread_mutex_lock(&g_lock);
                }
            }
            if (pl->q_count == 0) break;

            sl_buffer_t *b = &pl->queue[pl->q_head];
            const int16_t *src = (const int16_t *)(b->data + b->pos);
            size_t left = (b->size - b->pos) / sizeof(int16_t);
            if (left < (size_t)pl->channels) break;

            for (int c = 0; c < g_dev_channels; c++) {
                int sc = pl->channels == 1 ? 0 : c;
                int v = out[f * g_dev_channels + c] + (int)(src[sc] * pl->gain);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                out[f * g_dev_channels + c] = (int16_t)v;
            }
            b->pos += sizeof(int16_t) * pl->channels;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void ensure_device(int rate, int channels) {
    pthread_mutex_lock(&g_lock);
    if (!g_dev) {  // UM unico open, sempre
        SDL_AudioSpec want, have;
        memset(&want, 0, sizeof want);
        want.freq = rate;
        want.format = AUDIO_S16SYS;
        want.channels = (Uint8)(channels > 2 ? 2 : channels);
        want.samples = 1024;
        want.callback = mix_callback;
        g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_dev) {
            g_dev_rate = have.freq;
            g_dev_channels = have.channels;
            SDL_PauseAudioDevice(g_dev, 0);
            fprintf(stderr, "[snd] device SDL aberto: %d Hz, %d canais\n", g_dev_rate,
                    g_dev_channels);
        } else {
            fprintf(stderr, "[snd] SDL_OpenAudioDevice falhou: %s\n", SDL_GetError());
        }
    }
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// SLAndroidSimpleBufferQueueItf: Enqueue, Clear, GetState, RegisterCallback
// ---------------------------------------------------------------------------
static sl_player_t *player_from_bq(void *self) {
    // self aponta para o campo bq_vt; recuamos para a base da struct.
    return (sl_player_t *)((uint8_t *)self - offsetof(sl_player_t, bq_vt));
}

static SLresult bq_enqueue(void *self, const void *buffer, SLuint32 size) {
    sl_player_t *pl = player_from_bq(self);
    pthread_mutex_lock(&g_lock);
    if (pl->q_count >= MAX_QUEUED) {
        pthread_mutex_unlock(&g_lock);
        return SL_RESULT_PARAMETER_INVALID;
    }
    int slot = (pl->q_head + pl->q_count) % MAX_QUEUED;
    pl->queue[slot].data = buffer;
    pl->queue[slot].size = size;
    pl->queue[slot].pos = 0;
    pl->q_count++;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_clear(void *self) {
    sl_player_t *pl = player_from_bq(self);
    pthread_mutex_lock(&g_lock);
    pl->q_head = pl->q_count = 0;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

typedef struct {
    SLuint32 count;
    SLuint32 index;
} sl_bq_state_t;

static SLresult bq_get_state(void *self, sl_bq_state_t *state) {
    sl_player_t *pl = player_from_bq(self);
    if (state) {
        state->count = (SLuint32)pl->q_count;
        state->index = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult bq_register_callback(void *self, void (*cb)(void *, void *), void *ctx) {
    sl_player_t *pl = player_from_bq(self);
    pthread_mutex_lock(&g_lock);
    pl->callback = cb;
    pl->ctx = ctx;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static const void *k_bq_vt[] = {bq_enqueue, bq_clear, bq_get_state, bq_register_callback};

// ---------------------------------------------------------------------------
// SLPlayItf
// ---------------------------------------------------------------------------
static sl_player_t *player_from_play(void *self) {
    return (sl_player_t *)((uint8_t *)self - offsetof(sl_player_t, play_vt));
}

static SLresult play_set_state(void *self, SLuint32 state) {
    sl_player_t *pl = player_from_play(self);
    pl->state = (int)state;
    if (state == SL_PLAYSTATE_PLAYING) ensure_device(pl->rate, pl->channels);
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_state(void *self, SLuint32 *state) {
    if (state) *state = (SLuint32)player_from_play(self)->state;
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_duration(void *self, SLuint32 *ms) {
    (void)self;
    if (ms) *ms = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_position(void *self, SLuint32 *ms) {
    (void)self;
    if (ms) *ms = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult play_ok2(void *a, void *b) { (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult play_ok3(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    return SL_RESULT_SUCCESS;
}

static const void *k_play_vt[] = {
    play_set_state, play_get_state, play_get_duration, play_get_position,
    play_ok3,  // RegisterCallback
    play_ok2,  // SetCallbackEventsMask
    play_ok2,  // GetCallbackEventsMask
    play_ok2,  // SetMarkerPosition
    play_ok2,  // ClearMarkerPosition
    play_ok2,  // GetMarkerPosition
    play_ok2,  // SetPositionUpdatePeriod
    play_ok2,  // GetPositionUpdatePeriod
};

// ---------------------------------------------------------------------------
// SLVolumeItf — o engine ajusta volume por canal (setSoundVolume)
// ---------------------------------------------------------------------------
static sl_player_t *player_from_volume(void *self) {
    return (sl_player_t *)((uint8_t *)self - offsetof(sl_player_t, volume_vt));
}
static SLresult vol_set_level(void *self, SLint32 mb) {
    // millibels -> ganho linear
    sl_player_t *pl = player_from_volume(self);
    pl->gain = mb <= -9600 ? 0.0f : powf(10.0f, (float)mb / 2000.0f);
    if (pl->gain > 1.0f) pl->gain = 1.0f;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_level(void *self, SLint32 *mb) {
    (void)self;
    if (mb) *mb = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_max(void *self, SLint32 *mb) {
    (void)self;
    if (mb) *mb = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_ok2(void *a, void *b) { (void)a; (void)b; return SL_RESULT_SUCCESS; }

static const void *k_volume_vt[] = {
    vol_set_level, vol_get_level, vol_get_max,
    vol_ok2,  // SetMute
    vol_ok2,  // GetMute
    vol_ok2,  // EnableStereoPosition
    vol_ok2,  // IsEnabledStereoPosition
    vol_ok2,  // SetStereoPosition
    vol_ok2,  // GetStereoPosition
};

// ---------------------------------------------------------------------------
// SLObjectItf — comum a engine, outputmix e player
// ---------------------------------------------------------------------------
// Identificamos o tipo pelo primeiro campo do objeto.
typedef struct {
    const void **object_vt;
    int kind;
    const void **engine_vt;
} sl_generic_t;

extern const void *k_engine_vt[];

static SLresult obj_realize(void *self, SLboolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_resume(void *self, SLboolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_get_state(void *self, SLuint32 *state) {
    (void)self;
    if (state) *state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

// Os SL_IID_* sao os enderecos exportados por imports.c; comparamos por
// identidade contra os que o engine pede. Como nao temos os valores reais,
// distinguimos pela ORDEM em que o engine pede as interfaces de cada objeto:
// engine -> ENGINE; player -> PLAY / BUFFERQUEUE / VOLUME.
extern const void *g_iid_engine, *g_iid_play, *g_iid_bq, *g_iid_volume;
extern const void *sl_var_BUFFERQUEUE;

static SLresult obj_get_interface(void *self, const void *iid, void *out) {
    sl_generic_t *g = self;
    if (!out) return SL_RESULT_PARAMETER_INVALID;

    if (g->kind == OBJ_ENGINE) {
        *(void **)out = &g->engine_vt;
        return SL_RESULT_SUCCESS;
    }
    if (g->kind == OBJ_PLAYER) {
        sl_player_t *pl = (sl_player_t *)self;
        if (iid == g_iid_play) { *(void **)out = &pl->play_vt; return SL_RESULT_SUCCESS; }
        if (iid == g_iid_bq || iid == sl_var_BUFFERQUEUE) {
            *(void **)out = &pl->bq_vt;
            return SL_RESULT_SUCCESS;
        }
        if (iid == g_iid_volume) { *(void **)out = &pl->volume_vt; return SL_RESULT_SUCCESS; }
        fprintf(stderr, "[snd] GetInterface: IID %p nao reconhecida (play=%p bq=%p vol=%p)\n",
                iid, g_iid_play, g_iid_bq, g_iid_volume);
        return SL_RESULT_PARAMETER_INVALID;
    }
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult obj_register_callback(void *self, void *cb, void *ctx) {
    (void)self; (void)cb; (void)ctx;
    return SL_RESULT_SUCCESS;
}
static void obj_abort(void *self) { (void)self; }

static void obj_destroy(void *self) {
    sl_generic_t *g = self;
    if (g->kind == OBJ_PLAYER) {
        sl_player_t *pl = (sl_player_t *)self;
        pthread_mutex_lock(&g_lock);
        pl->used = 0;
        pl->state = SL_PLAYSTATE_STOPPED;
        pl->q_head = pl->q_count = 0;
        pthread_mutex_unlock(&g_lock);
    }
}
static SLresult obj_ok2(void *a, void *b) { (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_ok3(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_ok4(void *a, void *b, void *c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    return SL_RESULT_SUCCESS;
}

static const void *k_object_vt[] = {
    obj_realize, obj_resume, obj_get_state, obj_get_interface, obj_register_callback,
    obj_abort, obj_destroy,
    obj_ok3,  // SetPriority
    obj_ok2,  // GetPriority
    obj_ok4,  // SetLossOfControlInterfaces
};

// ---------------------------------------------------------------------------
// SLEngineItf
// ---------------------------------------------------------------------------
static sl_generic_t g_outputmix;

static SLresult eng_create_output_mix(void *self, void **obj, SLuint32 n, const void *ids,
                                      const void *req) {
    (void)self; (void)n; (void)ids; (void)req;
    g_outputmix.object_vt = k_object_vt;
    g_outputmix.kind = OBJ_OUTPUTMIX;
    *obj = &g_outputmix;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_create_audio_player(void *self, void **obj, sl_data_t *src, sl_data_t *snk,
                                        SLuint32 n, const void *ids, const void *req) {
    (void)self; (void)snk; (void)n; (void)ids; (void)req;
    pthread_mutex_lock(&g_lock);
    sl_player_t *pl = NULL;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].used) { pl = &g_players[i]; break; }
    }
    if (!pl) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[snd] sem slot de player livre\n");
        return SL_RESULT_PARAMETER_INVALID;
    }
    memset(pl, 0, sizeof *pl);
    pl->object_vt = k_object_vt;
    pl->kind = OBJ_PLAYER;
    pl->play_vt = k_play_vt;
    pl->bq_vt = k_bq_vt;
    pl->volume_vt = k_volume_vt;
    pl->used = 1;
    pl->state = SL_PLAYSTATE_STOPPED;
    pl->gain = 1.0f;
    pl->channels = 2;
    pl->rate = 44100;
    pl->bits = 16;

    if (src && src->pFormat) {
        sl_pcm_t *pcm = src->pFormat;
        if (pcm->numChannels >= 1 && pcm->numChannels <= 2) pl->channels = (int)pcm->numChannels;
        if (pcm->samplesPerSec >= 8000000u && pcm->samplesPerSec <= 192000000u) {
            pl->rate = (int)(pcm->samplesPerSec / 1000);  // milihertz -> Hz
        }
        if (pcm->bitsPerSample) pl->bits = (int)pcm->bitsPerSample;
    }
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr, "[snd] player criado: %d Hz, %d canais, %d bits\n", pl->rate, pl->channels,
            pl->bits);
    ensure_device(pl->rate, pl->channels);
    *obj = pl;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void *a, void *b, void *c, void *d, void *e, void *f, void *g) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return SL_RESULT_PARAMETER_INVALID;
}
static SLresult eng_query_num(void *self, SLuint32 type, SLuint32 *num) {
    (void)self; (void)type;
    if (num) *num = 0;
    return SL_RESULT_SUCCESS;
}

const void *k_engine_vt[] = {
    eng_unsupported,          // CreateLEDDevice
    eng_unsupported,          // CreateVibraDevice
    eng_create_audio_player,  // CreateAudioPlayer
    eng_unsupported,          // CreateAudioRecorder
    eng_unsupported,          // CreateMidiPlayer
    eng_unsupported,          // CreateListener
    eng_unsupported,          // Create3DGroup
    eng_create_output_mix,    // CreateOutputMix
    eng_unsupported,          // CreateMetadataExtractor
    eng_unsupported,          // CreateExtensionObject
    eng_query_num,            // QueryNumSupportedInterfaces
    eng_unsupported,          // QuerySupportedInterfaces
    eng_query_num,            // QueryNumSupportedExtensions
    eng_unsupported,          // QuerySupportedExtension
    eng_unsupported,          // IsExtensionSupported
};

// ---------------------------------------------------------------------------
static sl_generic_t g_engine;

uint32_t sl_create_engine(void **engine, uint32_t num_opts, const void *opts,
                          uint32_t num_ifaces, const void *ifaces, const void *req) {
    (void)num_opts; (void)opts; (void)num_ifaces; (void)ifaces; (void)req;
    if (!engine) return SL_RESULT_PARAMETER_INVALID;
    g_engine.object_vt = k_object_vt;
    g_engine.kind = OBJ_ENGINE;
    g_engine.engine_vt = k_engine_vt;
    *engine = &g_engine;
    fprintf(stderr, "[snd] slCreateEngine OK\n");
    return SL_RESULT_SUCCESS;
}
