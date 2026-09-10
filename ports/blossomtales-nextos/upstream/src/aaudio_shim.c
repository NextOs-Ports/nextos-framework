/*
 * aaudio_shim.c -- `libaaudio.so` virtual para o FMOD, com backend SDL2.
 *
 * O FMOD do APK abre a saida de audio por `dlopen("libaaudio.so")` e resolve um
 * conjunto pequeno e fechado de simbolos (23 no total, listados no .so). Sem
 * essa biblioteca, `Studio::System::initialize` devolve ERR_OUTPUT_INIT e o
 * `SoundHelper.Initialize()` do jogo lanca — nem chega ao primeiro frame.
 *
 * Em vez de forcar NOSOUND (jogo mudo) ou escrever um plugin de saida FMOD,
 * implementamos a propria AAudio: o FMOD registra um data callback, e nos o
 * dirigimos a partir do callback de audio do SDL. E o fluxo nativo do FMOD,
 * apenas com o dispositivo do NextOS embaixo.
 *
 * O FMOD nunca pede sample rate/canais/formato: ele abre o stream e depois
 * consulta `AAudioStream_get*`. Entao escolhemos o formato aqui (float 48 kHz
 * estereo, que e exatamente o que o mixer do FMOD produz) e informamos.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AAUDIO_OK 0
#define AAUDIO_ERROR_UNAVAILABLE (-899 - 13)
#define AAUDIO_ERROR_INTERNAL (-899 - 21)

#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_FORMAT_PCM_FLOAT 2

#define AAUDIO_DIRECTION_OUTPUT 0
#define AAUDIO_DIRECTION_INPUT 1

#define AAUDIO_STREAM_STATE_OPEN 1
#define AAUDIO_STREAM_STATE_STARTED 3
#define AAUDIO_STREAM_STATE_PAUSED 5
#define AAUDIO_STREAM_STATE_STOPPED 9

#define AAUDIO_CALLBACK_RESULT_CONTINUE 0

#define SB_SAMPLE_RATE 48000
#define SB_CHANNELS 2
#define SB_BURST_FRAMES 512

typedef int (*aaudio_data_callback_t)(void *stream, void *user, void *audio,
                                      int32_t num_frames);
typedef void (*aaudio_error_callback_t)(void *stream, void *user, int error);

struct sb_aaudio_builder {
    int direction;
    int32_t buffer_capacity;
    int performance_mode;
    int usage;
    aaudio_data_callback_t data_cb;
    void *data_user;
    aaudio_error_callback_t error_cb;
    void *error_user;
};

struct sb_aaudio_stream {
    struct sb_aaudio_builder cfg;
    uint32_t sdl_device;
    int state;
    int32_t buffer_size;
    int blocks;
    int peak;
};

/* ---- SDL2 resolvido em runtime (a lib do sistema ja esta pre-carregada) ---- */
typedef struct {
    int freq;
    unsigned short format;
    unsigned char channels;
    unsigned char silence;
    unsigned short samples;
    unsigned short padding;
    unsigned int size;
    void (*callback)(void *userdata, unsigned char *stream, int len);
    void *userdata;
} sb_sdl_audio_spec;

#define SB_SDL_AUDIO_F32SYS 0x8120  /* AUDIO_F32LSB (aarch64 e little-endian) */
#define SB_SDL_AUDIO_S16SYS 0x8010  /* AUDIO_S16LSB */

/* O FMOD nao chama setFormat: ele abre o stream e aceita o formato que o
 * dispositivo anuncia. A saida AAudio dele so trata PCM 16 bits, entao e isso
 * que anunciamos (e o que pedimos ao SDL). */
#define SB_BYTES_PER_SAMPLE 2
#define SB_SDL_INIT_AUDIO 0x00000010u

static int (*p_SDL_InitSubSystem)(unsigned int);
static uint32_t (*p_SDL_OpenAudioDevice)(const char *, int, const sb_sdl_audio_spec *,
                                         sb_sdl_audio_spec *, int);
static void (*p_SDL_PauseAudioDevice)(uint32_t, int);
static void (*p_SDL_CloseAudioDevice)(uint32_t);
static const char *(*p_SDL_GetError)(void);

static int sdl_ready(void) {
    static int checked, ok;
    if (checked) return ok;
    checked = 1;
    p_SDL_InitSubSystem = dlsym(RTLD_DEFAULT, "SDL_InitSubSystem");
    p_SDL_OpenAudioDevice = dlsym(RTLD_DEFAULT, "SDL_OpenAudioDevice");
    p_SDL_PauseAudioDevice = dlsym(RTLD_DEFAULT, "SDL_PauseAudioDevice");
    p_SDL_CloseAudioDevice = dlsym(RTLD_DEFAULT, "SDL_CloseAudioDevice");
    p_SDL_GetError = dlsym(RTLD_DEFAULT, "SDL_GetError");
    ok = p_SDL_InitSubSystem && p_SDL_OpenAudioDevice && p_SDL_PauseAudioDevice &&
         p_SDL_CloseAudioDevice;
    if (!ok) fprintf(stderr, "[aaudio] SDL2 indisponivel; audio ficara mudo\n");
    return ok;
}

int sb_aaudio_available(void) { return sdl_ready(); }

/* Trace por chamada (getState/getXRunCount rodam a cada bloco de audio). */
static int aaudio_trace(void) {
    static int checked, on;
    if (!checked) { checked = 1; const char *v = getenv("SB_AUDIO_TRACE"); on = v && *v && *v != '0'; }
    return on;
}
#define AA_TRACE(...) do { if (aaudio_trace()) fprintf(stderr, __VA_ARGS__); } while (0)

static void sb_sdl_callback(void *userdata, unsigned char *stream, int len) {
    struct sb_aaudio_stream *s = (struct sb_aaudio_stream *)userdata;
    int32_t frames = len / (SB_CHANNELS * SB_BYTES_PER_SAMPLE);
    if (!s || !s->cfg.data_cb || s->state != AAUDIO_STREAM_STATE_STARTED) {
        memset(stream, 0, (size_t)len);
        return;
    }
    /* O FMOD escreve o bloco inteiro; se recusar continuar, silenciamos. */
    if (s->cfg.data_cb(s, s->cfg.data_user, stream, frames) !=
        AAUDIO_CALLBACK_RESULT_CONTINUE) {
        memset(stream, 0, (size_t)len);
        s->state = AAUDIO_STREAM_STATE_STOPPED;
        return;
    }
    /* Pico por janela: e a prova de que o mixer do FMOD esta realmente
     * produzindo som, e nao apenas girando em silencio. */
    s->blocks++;
    const int16_t *pcm = (const int16_t *)stream;
    int samples = len / (int)sizeof(int16_t);
    for (int i = 0; i < samples; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > s->peak) s->peak = v;
    }
    if (s->blocks >= 375) {   /* ~4 s a 48 kHz com blocos de 512 quadros */
        fprintf(stderr, "[aaudio] %d blocos entregues, pico=%d/32767\n",
                s->blocks, s->peak);
        s->blocks = 0;
        s->peak = 0;
    }
}

/* ---- API AAudio ---- */
static int sb_AAudio_createStreamBuilder(struct sb_aaudio_builder **out) {
    if (!out) return AAUDIO_ERROR_INTERNAL;
    struct sb_aaudio_builder *b = calloc(1, sizeof *b);
    if (!b) return AAUDIO_ERROR_INTERNAL;
    b->direction = AAUDIO_DIRECTION_OUTPUT;
    b->buffer_capacity = SB_BURST_FRAMES * 4;
    *out = b;
    return AAUDIO_OK;
}
static int sb_AAudioStreamBuilder_delete(struct sb_aaudio_builder *b) {
    free(b);
    return AAUDIO_OK;
}
static void sb_AAudioStreamBuilder_setDirection(struct sb_aaudio_builder *b, int d) {
    if (b) b->direction = d;
}
static void sb_AAudioStreamBuilder_setBufferCapacityInFrames(
    struct sb_aaudio_builder *b, int32_t frames) {
    if (b && frames > 0) b->buffer_capacity = frames;
}
static void sb_AAudioStreamBuilder_setPerformanceMode(struct sb_aaudio_builder *b, int m) {
    if (b) b->performance_mode = m;
}
static void sb_AAudioStreamBuilder_setUsage(struct sb_aaudio_builder *b, int u) {
    if (b) b->usage = u;
}
static void sb_AAudioStreamBuilder_setDataCallback(struct sb_aaudio_builder *b,
                                                   aaudio_data_callback_t cb,
                                                   void *user) {
    if (b) { b->data_cb = cb; b->data_user = user; }
}
static void sb_AAudioStreamBuilder_setErrorCallback(struct sb_aaudio_builder *b,
                                                    aaudio_error_callback_t cb,
                                                    void *user) {
    if (b) { b->error_cb = cb; b->error_user = user; }
}

static int sb_AAudioStreamBuilder_openStream(struct sb_aaudio_builder *b,
                                             struct sb_aaudio_stream **out) {
    if (!b || !out) return AAUDIO_ERROR_INTERNAL;
    if (b->direction != AAUDIO_DIRECTION_OUTPUT) {
        fprintf(stderr, "[aaudio] captura nao suportada\n");
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    if (!sdl_ready()) return AAUDIO_ERROR_UNAVAILABLE;

    fprintf(stderr, "[aaudio] openStream: dir=%d cap=%d perf=%d usage=%d cb=%p\n",
            b->direction, b->buffer_capacity, b->performance_mode, b->usage,
            (void *)b->data_cb);
    struct sb_aaudio_stream *s = calloc(1, sizeof *s);
    if (!s) return AAUDIO_ERROR_INTERNAL;
    s->cfg = *b;
    s->state = AAUDIO_STREAM_STATE_OPEN;

    p_SDL_InitSubSystem(SB_SDL_INIT_AUDIO);
    sb_sdl_audio_spec want, have;
    memset(&want, 0, sizeof want);
    want.freq = SB_SAMPLE_RATE;
    want.format = SB_SDL_AUDIO_S16SYS;
    want.channels = SB_CHANNELS;
    want.samples = SB_BURST_FRAMES;
    want.callback = sb_sdl_callback;
    want.userdata = s;
    /* Sem SDL_AUDIO_ALLOW_*: o callback do FMOD e escrito para o formato que
     * anunciamos em AAudioStream_getFormat, entao o device precisa entregar
     * exatamente float estereo 48 kHz. */
    s->sdl_device = p_SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s->sdl_device) {
        fprintf(stderr, "[aaudio] SDL_OpenAudioDevice falhou: %s\n",
                p_SDL_GetError ? p_SDL_GetError() : "?");
        free(s);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    s->buffer_size = have.samples > 0 ? have.samples : SB_BURST_FRAMES;
    fprintf(stderr, "[aaudio] stream aberto: %d Hz, %d ch, s16, %d frames/bloco\n",
            have.freq, have.channels, s->buffer_size);
    *out = s;
    return AAUDIO_OK;
}

static int sb_AAudioStream_close(struct sb_aaudio_stream *s) {
    fprintf(stderr, "[aaudio] close(%p)\n", (void *)s);
    if (!s) return AAUDIO_ERROR_INTERNAL;
    if (s->sdl_device) {
        p_SDL_PauseAudioDevice(s->sdl_device, 1);
        p_SDL_CloseAudioDevice(s->sdl_device);
    }
    free(s);
    return AAUDIO_OK;
}
static int sb_AAudioStream_requestStart(struct sb_aaudio_stream *s) {
    fprintf(stderr, "[aaudio] requestStart(%p)\n", (void *)s);
    if (!s) return AAUDIO_ERROR_INTERNAL;
    s->state = AAUDIO_STREAM_STATE_STARTED;
    p_SDL_PauseAudioDevice(s->sdl_device, 0);
    return AAUDIO_OK;
}
static int sb_AAudioStream_requestPause(struct sb_aaudio_stream *s) {
    fprintf(stderr, "[aaudio] requestPause\n");
    if (!s) return AAUDIO_ERROR_INTERNAL;
    p_SDL_PauseAudioDevice(s->sdl_device, 1);
    s->state = AAUDIO_STREAM_STATE_PAUSED;
    return AAUDIO_OK;
}
static int sb_AAudioStream_requestStop(struct sb_aaudio_stream *s) {
    fprintf(stderr, "[aaudio] requestStop\n");
    if (!s) return AAUDIO_ERROR_INTERNAL;
    p_SDL_PauseAudioDevice(s->sdl_device, 1);
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    return AAUDIO_OK;
}
static int sb_AAudioStream_getState(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getState\n");
    return s ? s->state : AAUDIO_STREAM_STATE_STOPPED;
}
static int32_t sb_AAudioStream_getSampleRate(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getSampleRate\n");
    (void)s; return SB_SAMPLE_RATE;
}
static int32_t sb_AAudioStream_getChannelCount(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getChannelCount\n");
    (void)s; return SB_CHANNELS;
}
static int sb_AAudioStream_getFormat(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getFormat\n");
    (void)s; return AAUDIO_FORMAT_PCM_I16;
}
static int32_t sb_AAudioStream_getFramesPerBurst(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getFramesPerBurst\n");
    return s ? s->buffer_size : SB_BURST_FRAMES;
}
static int32_t sb_AAudioStream_getBufferSizeInFrames(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getBufferSizeInFrames\n");
    return s ? s->buffer_size : SB_BURST_FRAMES;
}
static int32_t sb_AAudioStream_getBufferCapacityInFrames(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getBufferCapacityInFrames\n");
    return s ? s->cfg.buffer_capacity : SB_BURST_FRAMES * 4;
}
static int32_t sb_AAudioStream_getXRunCount(struct sb_aaudio_stream *s) {
    AA_TRACE("[aaudio] getXRunCount\n");
    (void)s; return 0;
}
static int32_t sb_AAudioStream_setBufferSizeInFrames(struct sb_aaudio_stream *s,
                                                     int32_t frames) {
    AA_TRACE("[aaudio] setBufferSizeInFrames(%d) -> %d\n", frames,
            s ? s->buffer_size : SB_BURST_FRAMES);
    /* O tamanho real e o do device SDL; devolver o valor efetivo mantem o
     * FMOD coerente em vez de deixa-lo achar que encolheu a latencia. */
    return s ? s->buffer_size : SB_BURST_FRAMES;
}
static int sb_AAudioStream_read(struct sb_aaudio_stream *s, void *buf,
                                int32_t frames, int64_t timeout) {
    (void)s; (void)timeout;
    if (buf && frames > 0)
        memset(buf, 0, (size_t)frames * SB_CHANNELS * SB_BYTES_PER_SAMPLE);
    return frames;
}

struct sb_aaudio_export { const char *name; void *fn; };
static const struct sb_aaudio_export g_exports[] = {
    {"AAudio_createStreamBuilder", (void *)sb_AAudio_createStreamBuilder},
    {"AAudioStreamBuilder_delete", (void *)sb_AAudioStreamBuilder_delete},
    {"AAudioStreamBuilder_openStream", (void *)sb_AAudioStreamBuilder_openStream},
    {"AAudioStreamBuilder_setDirection", (void *)sb_AAudioStreamBuilder_setDirection},
    {"AAudioStreamBuilder_setBufferCapacityInFrames",
     (void *)sb_AAudioStreamBuilder_setBufferCapacityInFrames},
    {"AAudioStreamBuilder_setPerformanceMode",
     (void *)sb_AAudioStreamBuilder_setPerformanceMode},
    {"AAudioStreamBuilder_setUsage", (void *)sb_AAudioStreamBuilder_setUsage},
    {"AAudioStreamBuilder_setDataCallback", (void *)sb_AAudioStreamBuilder_setDataCallback},
    {"AAudioStreamBuilder_setErrorCallback", (void *)sb_AAudioStreamBuilder_setErrorCallback},
    {"AAudioStream_close", (void *)sb_AAudioStream_close},
    {"AAudioStream_requestStart", (void *)sb_AAudioStream_requestStart},
    {"AAudioStream_requestPause", (void *)sb_AAudioStream_requestPause},
    {"AAudioStream_requestStop", (void *)sb_AAudioStream_requestStop},
    {"AAudioStream_getState", (void *)sb_AAudioStream_getState},
    {"AAudioStream_getSampleRate", (void *)sb_AAudioStream_getSampleRate},
    {"AAudioStream_getChannelCount", (void *)sb_AAudioStream_getChannelCount},
    {"AAudioStream_getFormat", (void *)sb_AAudioStream_getFormat},
    {"AAudioStream_getFramesPerBurst", (void *)sb_AAudioStream_getFramesPerBurst},
    {"AAudioStream_getBufferSizeInFrames", (void *)sb_AAudioStream_getBufferSizeInFrames},
    {"AAudioStream_getBufferCapacityInFrames",
     (void *)sb_AAudioStream_getBufferCapacityInFrames},
    {"AAudioStream_getXRunCount", (void *)sb_AAudioStream_getXRunCount},
    {"AAudioStream_setBufferSizeInFrames", (void *)sb_AAudioStream_setBufferSizeInFrames},
    {"AAudioStream_read", (void *)sb_AAudioStream_read},
};

void *sb_aaudio_dlsym(const char *name) {
    if (!name) return NULL;
    for (unsigned i = 0; i < sizeof g_exports / sizeof g_exports[0]; i++)
        if (strcmp(g_exports[i].name, name) == 0) {
                    AA_TRACE("[aaudio] resolve %s\n", name);
            return g_exports[i].fn;
        }
    fprintf(stderr, "[aaudio] simbolo nao implementado: %s\n", name);
    return NULL;
}
