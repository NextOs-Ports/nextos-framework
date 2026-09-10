/*
 * aaudio_shim.c -- AAudio to SDL2.
 *
 * AAudio's playback contract is small: the caller builds a stream, hands over
 * a data callback and starts it; the system then calls back from its own audio
 * thread asking for numFrames of PCM.  SDL2's callback device has the same
 * shape, so the bridge is a translation, not an emulation of the mixer.
 *
 * The one contract detail that matters is the frame count.  AAudio promises
 * exactly framesPerDataCallback frames per call when the caller sets it, and
 * FMOD does set it; SDL hands us whatever its own buffer holds.  The callback
 * below therefore slices SDL's buffer into the promised chunk size instead of
 * forwarding it whole.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aaudio_shim.h"
#include "gb.h"

/* Public AAudio constants (NDK aaudio/AAudio.h). */
#define AAUDIO_OK                       0
#define AAUDIO_ERROR_NULL               (-901)
#define AAUDIO_ERROR_INVALID_STATE      (-895)
#define AAUDIO_ERROR_UNAVAILABLE        (-878)

#define AAUDIO_DIRECTION_OUTPUT         0
#define AAUDIO_DIRECTION_INPUT          1

#define AAUDIO_FORMAT_INVALID           (-1)
#define AAUDIO_FORMAT_UNSPECIFIED       0
#define AAUDIO_FORMAT_PCM_I16           1
#define AAUDIO_FORMAT_PCM_FLOAT         2

#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
#define AAUDIO_CALLBACK_RESULT_STOP     1

#define AAUDIO_STREAM_STATE_UNINITIALIZED 0
#define AAUDIO_STREAM_STATE_OPEN          2
#define AAUDIO_STREAM_STATE_STARTING      3
#define AAUDIO_STREAM_STATE_STARTED       4
#define AAUDIO_STREAM_STATE_STOPPING      9
#define AAUDIO_STREAM_STATE_STOPPED      10
#define AAUDIO_STREAM_STATE_CLOSED       12

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_stream_state_t;

typedef struct AAudioStream AAudioStream;

typedef int32_t (*aaudio_data_callback)(AAudioStream *stream, void *user,
                                        void *audio_data, int32_t num_frames);
typedef void (*aaudio_error_callback)(AAudioStream *stream, void *user,
                                      aaudio_result_t error);

typedef struct {
    int32_t device_id;
    int32_t sample_rate;
    int32_t channel_count;
    aaudio_format_t format;
    int32_t sharing_mode;
    int32_t direction;
    int32_t performance_mode;
    int32_t buffer_capacity;
    int32_t frames_per_callback;
    int32_t usage;
    aaudio_data_callback data_cb;
    void *data_user;
    aaudio_error_callback error_cb;
    void *error_user;
} aaudio_builder;

struct AAudioStream {
    SDL_AudioDeviceID device;
    int32_t sample_rate;
    int32_t channel_count;
    aaudio_format_t format;
    int32_t frame_bytes;
    int32_t frames_per_burst;
    int32_t frames_per_callback;
    int32_t buffer_capacity;
    int32_t buffer_size;
    int32_t device_id;
    int32_t xruns;
    unsigned long callbacks;
    float peak_seen;
    aaudio_data_callback data_cb;
    void *data_user;
    aaudio_error_callback error_cb;
    void *error_user;
    volatile int state;
};

/* AAudio streams are few and long-lived; one slot is all Unity ever opens,
 * and the spare exists so a re-open during a device change never fails. */
static AAudioStream *live_streams[4];

static void remember_stream(AAudioStream *stream)
{
    for (size_t i = 0; i < sizeof live_streams / sizeof *live_streams; i++)
        if (!live_streams[i]) {
            live_streams[i] = stream;
            return;
        }
}

static void forget_stream(AAudioStream *stream)
{
    for (size_t i = 0; i < sizeof live_streams / sizeof *live_streams; i++)
        if (live_streams[i] == stream)
            live_streams[i] = NULL;
}

/* Opt-in flow proof: an open device is not sound.  FP2_AUDIO_TRACE reports the
 * peak sample actually handed to the driver, which separates "FMOD is mixing
 * silence" from "the bridge is not delivering". */
static int audio_trace_on(void)
{
#ifdef FP2_RELEASE_BUILD
    return 0;
#else
    static int cached = -1;
    if (cached < 0)
        cached = getenv("FP2_AUDIO_TRACE") != NULL;
    return cached;
#endif
}

static float buffer_peak(const AAudioStream *stream, const void *data,
                         int32_t frames)
{
    int32_t values = frames * stream->channel_count;
    float peak = 0.0f;
    if (stream->format == AAUDIO_FORMAT_PCM_FLOAT) {
        const float *samples = data;
        for (int32_t i = 0; i < values; i++) {
            float value = samples[i] < 0 ? -samples[i] : samples[i];
            if (value > peak)
                peak = value;
        }
    } else {
        const int16_t *samples = data;
        for (int32_t i = 0; i < values; i++) {
            int value = samples[i] < 0 ? -samples[i] : samples[i];
            if ((float)value > peak)
                peak = (float)value;
        }
        peak /= 32768.0f;
    }
    return peak;
}

static void sdl_callback(void *user, Uint8 *out, int len)
{
    AAudioStream *stream = user;
    if (!stream || !stream->data_cb || len <= 0) {
        if (out && len > 0)
            memset(out, 0, (size_t)len);
        return;
    }

    int32_t frame_bytes = stream->frame_bytes;
    int32_t frames_left = (int32_t)(len / frame_bytes);
    int32_t chunk = stream->frames_per_callback > 0
                  ? stream->frames_per_callback : frames_left;
    Uint8 *cursor = out;

    while (frames_left > 0) {
        int32_t frames = chunk < frames_left ? chunk : frames_left;
        int32_t result = stream->data_cb(stream, stream->data_user,
                                         cursor, frames);
        if (result != AAUDIO_CALLBACK_RESULT_CONTINUE) {
            memset(cursor, 0, (size_t)frames * (size_t)frame_bytes);
            stream->state = AAUDIO_STREAM_STATE_STOPPING;
        }
        if (audio_trace_on()) {
            stream->callbacks++;
            float peak = buffer_peak(stream, cursor, frames);
            if (peak > stream->peak_seen)
                stream->peak_seen = peak;
            if (stream->callbacks <= 4 || stream->callbacks % 200 == 0)
                fprintf(stderr,
                        "[audio/aa] cb=%lu frames=%d result=%d peak=%.4f "
                        "peak-max=%.4f\n",
                        stream->callbacks, frames, result, peak,
                        stream->peak_seen);
        }
        cursor += (size_t)frames * (size_t)frame_bytes;
        frames_left -= frames;
    }
}

static aaudio_result_t sh_AAudio_createStreamBuilder(aaudio_builder **out)
{
    if (!out)
        return AAUDIO_ERROR_NULL;
    aaudio_builder *builder = calloc(1, sizeof *builder);
    if (!builder)
        return AAUDIO_ERROR_UNAVAILABLE;
    builder->format = AAUDIO_FORMAT_UNSPECIFIED;
    builder->direction = AAUDIO_DIRECTION_OUTPUT;
    builder->device_id = 0;
    *out = builder;
    return AAUDIO_OK;
}

#define BUILDER_SETTER(suffix, field, type)                                  \
    static void sh_AAudioStreamBuilder_##suffix(aaudio_builder *b, type v)   \
    {                                                                        \
        if (b) b->field = v;                                                 \
    }

BUILDER_SETTER(setDeviceId, device_id, int32_t)
BUILDER_SETTER(setSampleRate, sample_rate, int32_t)
BUILDER_SETTER(setChannelCount, channel_count, int32_t)
BUILDER_SETTER(setFormat, format, aaudio_format_t)
BUILDER_SETTER(setSharingMode, sharing_mode, int32_t)
BUILDER_SETTER(setDirection, direction, int32_t)
BUILDER_SETTER(setPerformanceMode, performance_mode, int32_t)
BUILDER_SETTER(setBufferCapacityInFrames, buffer_capacity, int32_t)
BUILDER_SETTER(setFramesPerDataCallback, frames_per_callback, int32_t)
/* FMOD sets the AAudio "usage" attribute (game audio) and never touches rate,
 * channels or format: it opens with the device defaults and then asks the
 * stream what it got.  The shim has to answer those queries honestly. */
BUILDER_SETTER(setUsage, usage, int32_t)

static void sh_AAudioStreamBuilder_setDataCallback(aaudio_builder *b,
                                                   aaudio_data_callback cb,
                                                   void *user)
{
    if (!b)
        return;
    b->data_cb = cb;
    b->data_user = user;
}

static void sh_AAudioStreamBuilder_setErrorCallback(aaudio_builder *b,
                                                    aaudio_error_callback cb,
                                                    void *user)
{
    if (!b)
        return;
    b->error_cb = cb;
    b->error_user = user;
}

static aaudio_result_t sh_AAudioStreamBuilder_delete(aaudio_builder *b)
{
    free(b);
    return AAUDIO_OK;
}

static aaudio_result_t sh_AAudioStreamBuilder_openStream(aaudio_builder *b,
                                                         AAudioStream **out)
{
    if (!b || !out)
        return AAUDIO_ERROR_NULL;
    *out = NULL;

    /* Capture is not part of this port: answering "unavailable" is the honest
     * reply, and it is what a device without a microphone route returns. */
    if (b->direction != AAUDIO_DIRECTION_OUTPUT) {
        fprintf(stderr, "[audio/aa] input stream refused (direction=%d)\n",
                b->direction);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    if (!b->data_cb) {
        /* The blocking write API is a different contract and Unity never uses
         * it; failing here is better than opening a device nothing feeds. */
        fprintf(stderr, "[audio/aa] stream without data callback refused\n");
        return AAUDIO_ERROR_UNAVAILABLE;
    }

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio/aa] SDL audio init failed: %s\n",
                SDL_GetError());
        return AAUDIO_ERROR_UNAVAILABLE;
    }

    AAudioStream *stream = calloc(1, sizeof *stream);
    if (!stream)
        return AAUDIO_ERROR_UNAVAILABLE;

    int32_t rate = b->sample_rate > 0 ? b->sample_rate : 48000;
    int32_t channels = b->channel_count > 0 ? b->channel_count : 2;
    if (channels > 2)
        channels = 2;

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    memset(&have, 0, sizeof have);
    want.freq = rate;
    want.channels = (Uint8)channels;
    want.format = b->format == AAUDIO_FORMAT_PCM_FLOAT ? AUDIO_F32SYS
                                                       : AUDIO_S16SYS;
    /* AAudio's burst is the unit the caller expects to be asked for.  Honour
     * the requested callback size when there is one so FMOD's mixer block and
     * the device period agree. */
    int32_t samples = b->frames_per_callback > 0 ? b->frames_per_callback : 512;
    if (samples < 128)
        samples = 128;
    if (samples > 4096)
        samples = 4096;
    want.samples = (Uint16)samples;
    want.callback = sdl_callback;
    want.userdata = stream;

    /* SDL_AUDIO_ALLOW_FREQUENCY_CHANGE is deliberately absent: FMOD already
     * resampled to the rate it asked for, so a silently different rate would
     * play the whole game detuned. */
    SDL_AudioDeviceID device =
        SDL_OpenAudioDevice(NULL, 0, &want, &have,
                            SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!device) {
        fprintf(stderr, "[audio/aa] SDL_OpenAudioDevice failed: %s "
                        "(driver=%s, %d Hz, %d ch)\n",
                SDL_GetError(),
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                            : "(none)",
                rate, channels);
        free(stream);
        return AAUDIO_ERROR_UNAVAILABLE;
    }

    stream->device = device;
    stream->sample_rate = have.freq;
    stream->channel_count = have.channels;
    stream->format = b->format == AAUDIO_FORMAT_PCM_FLOAT
                   ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16;
    stream->frame_bytes = (int32_t)SDL_AUDIO_BITSIZE(have.format) / 8 *
                          have.channels;
    stream->frames_per_burst = have.samples;
    stream->frames_per_callback = b->frames_per_callback;
    stream->buffer_capacity = b->buffer_capacity > 0 ? b->buffer_capacity
                                                     : have.samples * 2;
    stream->buffer_size = have.samples * 2;
    stream->device_id = b->device_id;
    stream->data_cb = b->data_cb;
    stream->data_user = b->data_user;
    stream->error_cb = b->error_cb;
    stream->error_user = b->error_user;
    stream->state = AAUDIO_STREAM_STATE_OPEN;

    remember_stream(stream);
    fprintf(stderr,
            "[audio/aa] stream open: %d Hz, %d channel(s), %s, burst=%d, "
            "callback=%d frames (driver=%s)\n",
            stream->sample_rate, stream->channel_count,
            stream->format == AAUDIO_FORMAT_PCM_FLOAT ? "float32" : "s16",
            stream->frames_per_burst, stream->frames_per_callback,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    *out = stream;
    return AAUDIO_OK;
}

static aaudio_result_t sh_AAudioStream_requestStart(AAudioStream *stream)
{
    if (!stream)
        return AAUDIO_ERROR_NULL;
    SDL_PauseAudioDevice(stream->device, 0);
    stream->state = AAUDIO_STREAM_STATE_STARTED;
    return AAUDIO_OK;
}

static aaudio_result_t sh_AAudioStream_requestStop(AAudioStream *stream)
{
    if (!stream)
        return AAUDIO_ERROR_NULL;
    SDL_PauseAudioDevice(stream->device, 1);
    stream->state = AAUDIO_STREAM_STATE_STOPPED;
    return AAUDIO_OK;
}

static aaudio_result_t sh_AAudioStream_close(AAudioStream *stream)
{
    if (!stream)
        return AAUDIO_ERROR_NULL;
    /* SDL_CloseAudioDevice waits for the callback to return, so the data
     * callback can never run against a freed stream. */
    SDL_PauseAudioDevice(stream->device, 1);
    SDL_CloseAudioDevice(stream->device);
    stream->state = AAUDIO_STREAM_STATE_CLOSED;
    forget_stream(stream);
    free(stream);
    return AAUDIO_OK;
}

static aaudio_result_t sh_AAudioStream_waitForStateChange(
    AAudioStream *stream, aaudio_stream_state_t input,
    aaudio_stream_state_t *next, int64_t timeout)
{
    (void)input;
    (void)timeout;
    if (!stream)
        return AAUDIO_ERROR_NULL;
    if (next)
        *next = stream->state;
    return AAUDIO_OK;
}

static int32_t sh_AAudioStream_getBufferSizeInFrames(AAudioStream *s)
{
    return s ? s->buffer_size : AAUDIO_ERROR_NULL;
}

static aaudio_result_t sh_AAudioStream_setBufferSizeInFrames(AAudioStream *s,
                                                             int32_t frames)
{
    if (!s)
        return AAUDIO_ERROR_NULL;
    if (frames < s->frames_per_burst)
        frames = s->frames_per_burst;
    if (frames > s->buffer_capacity)
        frames = s->buffer_capacity;
    s->buffer_size = frames;
    return frames;                      /* AAudio returns the size it applied */
}

static int32_t sh_AAudioStream_getFramesPerBurst(AAudioStream *s)
{
    return s ? s->frames_per_burst : AAUDIO_ERROR_NULL;
}

static int32_t sh_AAudioStream_getBufferCapacityInFrames(AAudioStream *s)
{
    return s ? s->buffer_capacity : AAUDIO_ERROR_NULL;
}

static int32_t sh_AAudioStream_getSampleRate(AAudioStream *s)
{
    return s ? s->sample_rate : 0;
}

static int32_t sh_AAudioStream_getChannelCount(AAudioStream *s)
{
    return s ? s->channel_count : 0;
}

static aaudio_format_t sh_AAudioStream_getFormat(AAudioStream *s)
{
    return s ? s->format : AAUDIO_FORMAT_INVALID;
}

static aaudio_stream_state_t sh_AAudioStream_getState(AAudioStream *s)
{
    return s ? (aaudio_stream_state_t)s->state
             : AAUDIO_STREAM_STATE_UNINITIALIZED;
}

/* Output-only bridge: a capture stream never opens, so a read can only come
 * from a caller probing the API.  Report "no input" instead of pretending. */
static aaudio_result_t sh_AAudioStream_read(AAudioStream *s, void *buffer,
                                            int32_t num_frames,
                                            int64_t timeout_ns)
{
    (void)s; (void)buffer; (void)num_frames; (void)timeout_ns;
    return AAUDIO_ERROR_UNAVAILABLE;
}

static int32_t sh_AAudioStream_getXRunCount(AAudioStream *s)
{
    return s ? s->xruns : AAUDIO_ERROR_NULL;
}

static int32_t sh_AAudioStream_getDeviceId(AAudioStream *s)
{
    return s ? s->device_id : AAUDIO_ERROR_NULL;
}

/* MMap is an exclusive-mode fast path this bridge does not have.  Claiming it
 * would only make the caller size buffers for a latency SDL cannot deliver. */
static int32_t sh_AAudioStream_isMMapUsed(AAudioStream *s)
{
    (void)s;
    return 0;
}

void fp2_aaudio_stop(void)
{
    for (size_t i = 0; i < sizeof live_streams / sizeof *live_streams; i++)
        if (live_streams[i])
            sh_AAudioStream_close(live_streams[i]);
}

#define A(n, f) { n, (void *)(uintptr_t)(f) }

static const nx_import aa_tab[] = {
    A("AAudio_createStreamBuilder", sh_AAudio_createStreamBuilder),
    A("AAudioStreamBuilder_delete", sh_AAudioStreamBuilder_delete),
    A("AAudioStreamBuilder_openStream", sh_AAudioStreamBuilder_openStream),
    A("AAudioStreamBuilder_setBufferCapacityInFrames",
      sh_AAudioStreamBuilder_setBufferCapacityInFrames),
    A("AAudioStreamBuilder_setChannelCount",
      sh_AAudioStreamBuilder_setChannelCount),
    A("AAudioStreamBuilder_setDataCallback",
      sh_AAudioStreamBuilder_setDataCallback),
    A("AAudioStreamBuilder_setDeviceId", sh_AAudioStreamBuilder_setDeviceId),
    A("AAudioStreamBuilder_setDirection", sh_AAudioStreamBuilder_setDirection),
    A("AAudioStreamBuilder_setErrorCallback",
      sh_AAudioStreamBuilder_setErrorCallback),
    A("AAudioStreamBuilder_setFormat", sh_AAudioStreamBuilder_setFormat),
    A("AAudioStreamBuilder_setFramesPerDataCallback",
      sh_AAudioStreamBuilder_setFramesPerDataCallback),
    A("AAudioStreamBuilder_setPerformanceMode",
      sh_AAudioStreamBuilder_setPerformanceMode),
    A("AAudioStreamBuilder_setSampleRate",
      sh_AAudioStreamBuilder_setSampleRate),
    A("AAudioStreamBuilder_setSharingMode",
      sh_AAudioStreamBuilder_setSharingMode),
    A("AAudioStream_close", sh_AAudioStream_close),
    A("AAudioStream_getBufferCapacityInFrames",
      sh_AAudioStream_getBufferCapacityInFrames),
    A("AAudioStream_getBufferSizeInFrames",
      sh_AAudioStream_getBufferSizeInFrames),
    A("AAudioStream_getDeviceId", sh_AAudioStream_getDeviceId),
    A("AAudioStream_getFramesPerBurst", sh_AAudioStream_getFramesPerBurst),
    A("AAudioStream_getXRunCount", sh_AAudioStream_getXRunCount),
    A("AAudioStream_getSampleRate", sh_AAudioStream_getSampleRate),
    A("AAudioStream_getChannelCount", sh_AAudioStream_getChannelCount),
    A("AAudioStream_getFormat", sh_AAudioStream_getFormat),
    A("AAudioStream_getState", sh_AAudioStream_getState),
    A("AAudioStream_read", sh_AAudioStream_read),
    A("AAudioStreamBuilder_setUsage", sh_AAudioStreamBuilder_setUsage),
    A("AAudioStream_isMMapUsed", sh_AAudioStream_isMMapUsed),
    A("AAudioStream_requestStart", sh_AAudioStream_requestStart),
    A("AAudioStream_requestStop", sh_AAudioStream_requestStop),
    A("AAudioStream_setBufferSizeInFrames",
      sh_AAudioStream_setBufferSizeInFrames),
    A("AAudioStream_waitForStateChange", sh_AAudioStream_waitForStateChange),
};

const nx_import *fp2_aaudio_table(size_t *n)
{
    *n = sizeof aa_tab / sizeof *aa_tab;
    return aa_tab;
}

void *fp2_aaudio_sym(const char *name)
{
    for (size_t i = 0; i < sizeof aa_tab / sizeof *aa_tab; i++)
        if (strcmp(aa_tab[i].name, name) == 0)
            return aa_tab[i].addr;
    return NULL;
}
