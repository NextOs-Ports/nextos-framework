/*
 * Dead Trigger 2.1.0 OpenSL ES -> SDL2 audio bridge.
 *
 * This implements the Android sequence used by the FMOD output module bundled
 * in Unity 2019.4.40f1:
 *
 * slCreateEngine -> Engine.Realize/GetInterface -> CreateOutputMix/Realize
 * -> CreateAudioPlayer(AndroidSimpleBufferQueue + PCM)
 * -> AndroidConfiguration.SetConfiguration -> Player.Realize
 * -> GetInterface(PLAY/BUFFERQUEUE) -> RegisterCallback
 * -> Enqueue x8 -> SetPlayState(PLAYING).
 *
 * The object/vtable model and adaptive SDL output are derived from the
 * production-proven GTA SA NextOS OpenSL bridge. Audio is active by default.
 */
#define _GNU_SOURCE

#include "opensles_dt.h"
#include "framework_bridge.h"

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DT_AUDIO_QUEUE_CAPACITY 64
#define DT_AUDIO_CALLBACK_FRAMES 1024
#define DT_OBJECT_VTABLE_SIZE 10
#define DT_INTERFACE_VTABLE_SIZE 12

typedef struct {
    SLuint32 locator_type;
    SLuint32 buffer_count;
} dt_sl_buffer_queue_locator;

typedef struct {
    SLuint32 format_type;
    SLuint32 channels;
    SLuint32 sample_rate_millihertz;
    SLuint32 bits_per_sample;
    SLuint32 container_size;
    SLuint32 channel_mask;
    SLuint32 endianness;
} dt_sl_pcm_format;

typedef struct {
    void *locator;
    void *format;
} dt_sl_data_source;

typedef void (*dt_sl_queue_callback)(void *caller, void *context);
typedef void (*dt_sl_play_callback)(void *caller, void *context,
                                    SLuint32 event);

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t offset;
} dt_audio_buffer;

typedef struct {
    void *object_vtable[DT_OBJECT_VTABLE_SIZE];
    void *object;
    void *play_vtable[DT_INTERFACE_VTABLE_SIZE];
    void *play;
    void *queue_vtable[DT_INTERFACE_VTABLE_SIZE];
    void *queue;
    void *volume_vtable[DT_INTERFACE_VTABLE_SIZE];
    void *volume;
    void *configuration_vtable[DT_INTERFACE_VTABLE_SIZE];
    void *configuration;
    void *effect_vtable[DT_INTERFACE_VTABLE_SIZE];
    void *effect;

    pthread_mutex_t lock;
    int lock_ready;
    dt_audio_buffer buffers[DT_AUDIO_QUEUE_CAPACITY];
    unsigned queue_head;
    unsigned queue_count;
    unsigned queue_capacity;

    dt_sl_queue_callback queue_callback;
    void *queue_context;
    dt_sl_play_callback play_callback;
    void *play_context;
    SLuint32 play_event_mask;
    SLuint32 play_state;
    SLmillibel volume_millibels;

    SDL_AudioDeviceID device;
    SDL_AudioStream *conversion;
    SDL_AudioSpec source;
    SDL_AudioSpec output;
} dt_audio_player;

static dt_audio_player g_player;
static void *g_engine_object_vtable[DT_OBJECT_VTABLE_SIZE];
static void *g_engine_object;
static void *g_engine_interface_vtable[16];
static void *g_engine_interface;
static void *g_output_object_vtable[DT_OBJECT_VTABLE_SIZE];
static void *g_output_object;

static atomic_ullong g_pcm_bytes;
static atomic_uint g_pcm_peak;
static atomic_uint g_device_callbacks;
static atomic_int g_health_reported;

static const char g_id_engine;
static const char g_id_play;
static const char g_id_volume;
static const char g_id_queue;
static const char g_id_effect;
static const char g_id_engine_capabilities;
static const char g_id_configuration;
static const char g_id_environmental_reverb;

const SLInterfaceID sl_IID_ENGINE = &g_id_engine;
const SLInterfaceID sl_IID_PLAY = &g_id_play;
const SLInterfaceID sl_IID_VOLUME = &g_id_volume;
const SLInterfaceID sl_IID_BUFFERQUEUE = &g_id_queue;
const SLInterfaceID sl_IID_EFFECTSEND = &g_id_effect;
const SLInterfaceID sl_IID_ENGINECAPABILITIES = &g_id_engine_capabilities;
const SLInterfaceID sl_IID_ANDROIDCONFIGURATION = &g_id_configuration;
const SLInterfaceID sl_IID_ENVIRONMENTALREVERB =
    &g_id_environmental_reverb;

static int trace_enabled(void) {
    static int initialized;
    static int enabled;
    if (!initialized) {
        enabled = getenv("DT_AUDIO_TRACE") != NULL;
        initialized = 1;
    }
    return enabled;
}

#define IID_MATCH(value, global_id) \
    ((value) == (global_id) || (value) == (const void *)&(global_id))

static SLresult success_stub(void) {
    return SL_RESULT_SUCCESS;
}

static SLresult unsupported_stub(void) {
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const char *find_pulse_socket(void) {
    static const char *const paths[] = {
        "/run/pulse/native",
        "/var/run/pulse/native",
    };
    struct stat status;
    for (size_t index = 0; index < sizeof paths / sizeof paths[0]; ++index)
        if (stat(paths[index], &status) == 0 && S_ISSOCK(status.st_mode))
            return paths[index];
    return NULL;
}

static int reinitialize_sdl_audio(const char *driver,
                                  const char *pulse_socket,
                                  const char *alsa_device) {
    if (SDL_WasInit(SDL_INIT_AUDIO))
        SDL_QuitSubSystem(SDL_INIT_AUDIO);

    if (driver && *driver) {
        setenv("SDL_AUDIODRIVER", driver, 1);
        setenv("SDL_AUDIO_DRIVER", driver, 1);
    } else {
        unsetenv("SDL_AUDIODRIVER");
        unsetenv("SDL_AUDIO_DRIVER");
    }

    if (pulse_socket) {
        char server[160];
        snprintf(server, sizeof server, "unix:%s", pulse_socket);
        setenv("PULSE_SERVER", server, 1);
    } else if (driver && !strcmp(driver, "alsa")) {
        unsetenv("PULSE_SERVER");
    }

    if (alsa_device && *alsa_device) {
        setenv("AUDIODEV", alsa_device, 1);
        setenv("SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE", alsa_device, 1);
    } else {
        unsetenv("AUDIODEV");
        unsetenv("SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE");
    }
    SDL_ClearError();
    return SDL_InitSubSystem(SDL_INIT_AUDIO);
}

static SDL_AudioDeviceID open_fixed_alsa(const SDL_AudioSpec *source,
                                         SDL_AudioSpec *output) {
    SDL_AudioSpec request = *source;
    const char *device = getenv("DT_ALSA_DEVICE");
    if (!device || !*device)
        device = "plughw:0,0";
    request.freq = 48000;
    request.format = AUDIO_S16SYS;
    request.channels = 2;
    memset(output, 0, sizeof *output);
    if (reinitialize_sdl_audio("alsa", NULL, device) != 0)
        return 0;
    SDL_AudioDeviceID id =
        SDL_OpenAudioDevice(NULL, 0, &request, output,
                            SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (id && output->freq == 48000 &&
        output->format == AUDIO_S16SYS && output->channels == 2)
        return id;
    if (id)
        SDL_CloseAudioDevice(id);
    return 0;
}

static SDL_AudioDeviceID open_audio_device(const SDL_AudioSpec *request,
                                           SDL_AudioSpec *output) {
    char initial_error[256] = "";
    memset(output, 0, sizeof *output);

    if (SDL_WasInit(SDL_INIT_AUDIO) ||
        SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioDeviceID id =
            SDL_OpenAudioDevice(NULL, 0, request, output,
                                SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        if (id)
            return id;
        snprintf(initial_error, sizeof initial_error, "%s", SDL_GetError());
    }

    const char *pulse_socket = find_pulse_socket();
    if (pulse_socket &&
        reinitialize_sdl_audio("pulseaudio", pulse_socket, NULL) == 0) {
        SDL_AudioDeviceID id =
            SDL_OpenAudioDevice(NULL, 0, request, output,
                                SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        if (id) {
            fprintf(stderr, "[audio] recuperado via PulseAudio (%s)\n",
                    pulse_socket);
            return id;
        }
    }

    if (reinitialize_sdl_audio(NULL, pulse_socket, NULL) == 0) {
        SDL_AudioDeviceID id =
            SDL_OpenAudioDevice(NULL, 0, request, output,
                                SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        if (id)
            return id;
    }

    SDL_AudioDeviceID id = open_fixed_alsa(request, output);
    if (id) {
        fprintf(stderr, "[audio] recuperado via ALSA S16/48 kHz\n");
        return id;
    }
    fprintf(stderr, "[audio] sem dispositivo: inicial=%s final=%s\n",
            *initial_error ? initial_error : "erro desconhecido",
            SDL_GetError());
    return 0;
}

static void note_pcm_health(const uint8_t *data, int length,
                            SDL_AudioFormat format) {
    if (!data || length <= 0)
        return;
    atomic_fetch_add(&g_pcm_bytes, (unsigned long long)length);
    if (format != AUDIO_S16SYS)
        return;
    const int16_t *samples = (const int16_t *)data;
    unsigned count = (unsigned)length / sizeof *samples;
    unsigned peak = 0;
    for (unsigned index = 0; index < count; ++index) {
        int value = samples[index];
        unsigned magnitude =
            (unsigned)(value < 0 ? -(int64_t)value : value);
        if (magnitude > peak)
            peak = magnitude;
    }
    unsigned previous = atomic_load(&g_pcm_peak);
    while (peak > previous &&
           !atomic_compare_exchange_weak(&g_pcm_peak, &previous, peak)) {
    }
}

static void invoke_finished_buffer(dt_audio_player *player) {
    dt_sl_queue_callback callback = player->queue_callback;
    void *context = player->queue_context;
    pthread_mutex_unlock(&player->lock);
    if (callback)
        callback(&player->queue, context);
    pthread_mutex_lock(&player->lock);
}

static void audio_callback(void *userdata, Uint8 *stream, int length) {
    dt_audio_player *player = userdata;
    int written = 0;
    pthread_mutex_lock(&player->lock);

    if (player->conversion) {
        while (SDL_AudioStreamAvailable(player->conversion) < length &&
               player->queue_count) {
            dt_audio_buffer *buffer =
                &player->buffers[player->queue_head];
            int remaining = (int)(buffer->size - buffer->offset);
            if (SDL_AudioStreamPut(player->conversion,
                                   buffer->data + buffer->offset,
                                   remaining) < 0)
                break;
            buffer->offset += (uint32_t)remaining;
            player->queue_head =
                (player->queue_head + 1) % DT_AUDIO_QUEUE_CAPACITY;
            --player->queue_count;
            invoke_finished_buffer(player);
        }
        int converted =
            SDL_AudioStreamGet(player->conversion, stream, length);
        if (converted > 0)
            written = converted;
    } else {
        while (written < length && player->queue_count) {
            dt_audio_buffer *buffer =
                &player->buffers[player->queue_head];
            int remaining = (int)(buffer->size - buffer->offset);
            int amount = length - written < remaining
                       ? length - written : remaining;
            memcpy(stream + written, buffer->data + buffer->offset,
                   (size_t)amount);
            written += amount;
            buffer->offset += (uint32_t)amount;
            if (buffer->offset == buffer->size) {
                player->queue_head =
                    (player->queue_head + 1) %
                    DT_AUDIO_QUEUE_CAPACITY;
                --player->queue_count;
                invoke_finished_buffer(player);
            }
        }
    }

    pthread_mutex_unlock(&player->lock);
    if (written < length)
        memset(stream + written, 0, (size_t)(length - written));
    note_pcm_health(stream, written, player->output.format);
    atomic_fetch_add(&g_device_callbacks, 1);
}

static void reset_player(void) {
    dt_audio_player *player = &g_player;
    if (player->device) {
        SDL_PauseAudioDevice(player->device, 1);
        SDL_CloseAudioDevice(player->device);
    }
    if (player->conversion)
        SDL_FreeAudioStream(player->conversion);
    if (player->lock_ready)
        pthread_mutex_destroy(&player->lock);
    memset(player, 0, sizeof *player);
    pthread_mutex_init(&player->lock, NULL);
    player->lock_ready = 1;
    player->play_state = SL_PLAYSTATE_STOPPED;
    atomic_store(&g_pcm_bytes, 0);
    atomic_store(&g_pcm_peak, 0);
    atomic_store(&g_device_callbacks, 0);
    atomic_store(&g_health_reported, 0);
}

static SLresult object_realize(void *self, SLBoolean asynchronous) {
    (void)self;
    (void)asynchronous;
    return SL_RESULT_SUCCESS;
}

static SLresult object_get_state(void *self, SLuint32 *state) {
    (void)self;
    if (state)
        *state = 2;
    return SL_RESULT_SUCCESS;
}

static SLresult configuration_set(void *self, const char *key,
                                  const void *value, SLuint32 size) {
    (void)self;
    (void)value;
    if (trace_enabled())
        fprintf(stderr, "[audio] AndroidConfiguration.Set(%s, %u bytes)\n",
                key ? key : "?", (unsigned)size);
    return SL_RESULT_SUCCESS;
}

static SLresult configuration_get(void *self, const char *key,
                                  SLuint32 *size, void *value) {
    (void)self;
    (void)key;
    (void)value;
    if (size)
        *size = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult player_get_interface(void *self, SLInterfaceID id,
                                     void **result) {
    (void)self;
    if (!result)
        return SL_RESULT_RESOURCE_ERROR;
    if (IID_MATCH(id, sl_IID_PLAY))
        *result = &g_player.play;
    else if (IID_MATCH(id, sl_IID_BUFFERQUEUE))
        *result = &g_player.queue;
    else if (IID_MATCH(id, sl_IID_VOLUME))
        *result = &g_player.volume;
    else if (IID_MATCH(id, sl_IID_ANDROIDCONFIGURATION))
        *result = &g_player.configuration;
    else if (IID_MATCH(id, sl_IID_EFFECTSEND))
        *result = &g_player.effect;
    else {
        *result = NULL;
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    return SL_RESULT_SUCCESS;
}

static void player_destroy(void *self) {
    (void)self;
    reset_player();
}

static SLresult play_set_state(void *self, SLuint32 state) {
    (void)self;
    g_player.play_state = state;
    if (g_player.device)
        SDL_PauseAudioDevice(g_player.device,
                             state == SL_PLAYSTATE_PLAYING ? 0 : 1);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_state(void *self, SLuint32 *state) {
    (void)self;
    if (state)
        *state = g_player.play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_duration(void *self, SLmillisecond *duration) {
    (void)self;
    if (duration)
        *duration = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_position(void *self, SLmillisecond *position) {
    (void)self;
    if (position)
        *position = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult play_register_callback(void *self, dt_sl_play_callback callback,
                                       void *context) {
    (void)self;
    g_player.play_callback = callback;
    g_player.play_context = context;
    return SL_RESULT_SUCCESS;
}

static SLresult play_set_event_mask(void *self, SLuint32 mask) {
    (void)self;
    g_player.play_event_mask = mask;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_set(void *self, SLmillibel value) {
    (void)self;
    g_player.volume_millibels = value;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get(void *self, SLmillibel *value) {
    (void)self;
    if (value)
        *value = g_player.volume_millibels;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_max(void *self, SLmillibel *value) {
    (void)self;
    if (value)
        *value = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult queue_enqueue(void *self, const void *data, SLuint32 size) {
    (void)self;
    if (!data || !size)
        return SL_RESULT_SUCCESS;
    pthread_mutex_lock(&g_player.lock);
    if (g_player.queue_count == DT_AUDIO_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&g_player.lock);
        return SL_RESULT_RESOURCE_ERROR;
    }
    unsigned tail =
        (g_player.queue_head + g_player.queue_count) %
        DT_AUDIO_QUEUE_CAPACITY;
    g_player.buffers[tail].data = data;
    g_player.buffers[tail].size = size;
    g_player.buffers[tail].offset = 0;
    ++g_player.queue_count;
    pthread_mutex_unlock(&g_player.lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_clear(void *self) {
    (void)self;
    if (g_player.device)
        SDL_LockAudioDevice(g_player.device);
    pthread_mutex_lock(&g_player.lock);
    g_player.queue_head = 0;
    g_player.queue_count = 0;
    if (g_player.conversion)
        SDL_AudioStreamClear(g_player.conversion);
    pthread_mutex_unlock(&g_player.lock);
    if (g_player.device)
        SDL_UnlockAudioDevice(g_player.device);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(void *self, SLuint32 *state) {
    (void)self;
    if (state) {
        state[0] = g_player.queue_count;
        state[1] = g_player.queue_head %
            (g_player.queue_capacity ? g_player.queue_capacity : 1);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult queue_register_callback(void *self,
                                        dt_sl_queue_callback callback,
                                        void *context) {
    (void)self;
    g_player.queue_callback = callback;
    g_player.queue_context = context;
    return SL_RESULT_SUCCESS;
}

static void initialize_player_vtables(void) {
    for (unsigned index = 0; index < DT_OBJECT_VTABLE_SIZE; ++index)
        g_player.object_vtable[index] = (void *)success_stub;
    g_player.object_vtable[0] = (void *)object_realize;
    g_player.object_vtable[2] = (void *)object_get_state;
    g_player.object_vtable[3] = (void *)player_get_interface;
    g_player.object_vtable[6] = (void *)player_destroy;
    g_player.object = g_player.object_vtable;

    for (unsigned index = 0; index < DT_INTERFACE_VTABLE_SIZE; ++index) {
        g_player.play_vtable[index] = (void *)success_stub;
        g_player.queue_vtable[index] = (void *)success_stub;
        g_player.volume_vtable[index] = (void *)success_stub;
        g_player.configuration_vtable[index] = (void *)success_stub;
        g_player.effect_vtable[index] = (void *)success_stub;
    }
    g_player.play_vtable[0] = (void *)play_set_state;
    g_player.play_vtable[1] = (void *)play_get_state;
    g_player.play_vtable[2] = (void *)play_get_duration;
    g_player.play_vtable[3] = (void *)play_get_position;
    g_player.play_vtable[4] = (void *)play_register_callback;
    g_player.play_vtable[5] = (void *)play_set_event_mask;
    g_player.play = g_player.play_vtable;

    g_player.queue_vtable[0] = (void *)queue_enqueue;
    g_player.queue_vtable[1] = (void *)queue_clear;
    g_player.queue_vtable[2] = (void *)queue_get_state;
    g_player.queue_vtable[3] = (void *)queue_register_callback;
    g_player.queue = g_player.queue_vtable;

    g_player.volume_vtable[0] = (void *)volume_set;
    g_player.volume_vtable[1] = (void *)volume_get;
    g_player.volume_vtable[2] = (void *)volume_get_max;
    g_player.volume = g_player.volume_vtable;

    g_player.configuration_vtable[0] = (void *)configuration_set;
    g_player.configuration_vtable[1] = (void *)configuration_get;
    g_player.configuration = g_player.configuration_vtable;
    g_player.effect = g_player.effect_vtable;
}

static SLresult engine_create_player(void *self, void **result,
                                     dt_sl_data_source *source, void *sink,
                                     SLuint32 interface_count,
                                     const SLInterfaceID *interface_ids,
                                     const SLBoolean *required) {
    (void)self;
    (void)sink;
    (void)interface_count;
    (void)interface_ids;
    (void)required;

    reset_player();
    initialize_player_vtables();
    g_player.source.freq = 48000;
    g_player.source.format = AUDIO_S16SYS;
    g_player.source.channels = 2;
    g_player.source.samples = DT_AUDIO_CALLBACK_FRAMES;
    g_player.source.callback = audio_callback;
    g_player.source.userdata = &g_player;

    if (source && source->locator) {
        dt_sl_buffer_queue_locator *locator = source->locator;
        if (locator->locator_type ==
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE)
            g_player.queue_capacity = locator->buffer_count;
    }
    if (source && source->format) {
        dt_sl_pcm_format *format = source->format;
        if (format->format_type == SL_DATAFORMAT_PCM) {
            if (format->channels)
                g_player.source.channels = (Uint8)format->channels;
            if (format->sample_rate_millihertz)
                g_player.source.freq =
                    (int)(format->sample_rate_millihertz / 1000);
            if (format->bits_per_sample == 8)
                g_player.source.format = AUDIO_U8;
        }
    }

    g_player.device =
        open_audio_device(&g_player.source, &g_player.output);
    if (!g_player.device) {
        dt_framework_audio_failed();
        if (result)
            *result = NULL;
        return SL_RESULT_RESOURCE_ERROR;
    }
    if (g_player.output.freq != g_player.source.freq ||
        g_player.output.format != g_player.source.format ||
        g_player.output.channels != g_player.source.channels) {
        g_player.conversion = SDL_NewAudioStream(
            g_player.source.format, g_player.source.channels,
            g_player.source.freq, g_player.output.format,
            g_player.output.channels, g_player.output.freq);
        if (!g_player.conversion) {
            SDL_CloseAudioDevice(g_player.device);
            g_player.device = 0;
            if (result)
                *result = NULL;
            return SL_RESULT_RESOURCE_ERROR;
        }
    }
    SDL_PauseAudioDevice(g_player.device, 1);
    fprintf(stderr,
            "[audio] OpenSL PCM %dch/%dHz -> %s %dch/%dHz (dev=%u)\n",
            g_player.source.channels, g_player.source.freq,
            SDL_GetCurrentAudioDriver()
                ? SDL_GetCurrentAudioDriver() : "sem-driver",
            g_player.output.channels, g_player.output.freq,
            g_player.device);
    dt_framework_audio_opened(g_player.device, &g_player.output);
    if (result)
        *result = &g_player.object;
    return SL_RESULT_SUCCESS;
}

static SLresult output_get_interface(void *self, SLInterfaceID id,
                                     void **result) {
    (void)self;
    (void)id;
    if (result)
        *result = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void output_destroy(void *self) {
    (void)self;
}

static SLresult engine_create_output(void *self, void **result,
                                     SLuint32 interface_count,
                                     const SLInterfaceID *interface_ids,
                                     const SLBoolean *required) {
    (void)self;
    (void)interface_count;
    (void)interface_ids;
    (void)required;
    for (unsigned index = 0; index < DT_OBJECT_VTABLE_SIZE; ++index)
        g_output_object_vtable[index] = (void *)success_stub;
    g_output_object_vtable[0] = (void *)object_realize;
    g_output_object_vtable[2] = (void *)object_get_state;
    g_output_object_vtable[3] = (void *)output_get_interface;
    g_output_object_vtable[6] = (void *)output_destroy;
    g_output_object = g_output_object_vtable;
    if (result)
        *result = &g_output_object;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_get_interface(void *self, SLInterfaceID id,
                                     void **result) {
    (void)self;
    if (!result)
        return SL_RESULT_RESOURCE_ERROR;
    if (IID_MATCH(id, sl_IID_ENGINE)) {
        *result = &g_engine_interface;
        return SL_RESULT_SUCCESS;
    }
    *result = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void engine_destroy(void *self) {
    (void)self;
}

static void initialize_engine(void) {
    for (unsigned index = 0; index < DT_OBJECT_VTABLE_SIZE; ++index)
        g_engine_object_vtable[index] = (void *)success_stub;
    g_engine_object_vtable[0] = (void *)object_realize;
    g_engine_object_vtable[2] = (void *)object_get_state;
    g_engine_object_vtable[3] = (void *)engine_get_interface;
    g_engine_object_vtable[6] = (void *)engine_destroy;
    g_engine_object = g_engine_object_vtable;

    for (unsigned index = 0;
         index < sizeof g_engine_interface_vtable /
                     sizeof g_engine_interface_vtable[0];
         ++index)
        g_engine_interface_vtable[index] = (void *)unsupported_stub;
    g_engine_interface_vtable[2] = (void *)engine_create_player;
    g_engine_interface_vtable[7] = (void *)engine_create_output;
    g_engine_interface = g_engine_interface_vtable;
}

SLresult slCreateEngine_shim(void **engine, SLuint32 option_count,
                             const void *options, SLuint32 interface_count,
                             const SLInterfaceID *interface_ids,
                             const SLBoolean *interface_required) {
    (void)option_count;
    (void)options;
    (void)interface_count;
    (void)interface_ids;
    (void)interface_required;
    initialize_engine();
    if (engine)
        *engine = &g_engine_object;
    fprintf(stderr, "[audio] slCreateEngine -> SDL2\n");
    return SL_RESULT_SUCCESS;
}

void opensles_shim_pump_callbacks(void) {
    unsigned callbacks = atomic_load(&g_device_callbacks);
    unsigned peak = atomic_load(&g_pcm_peak);
    if (callbacks && peak &&
        !atomic_exchange(&g_health_reported, 1)) {
        fprintf(stderr,
                "[audio] PCM ativo: %llu bytes, pico=%u, callbacks=%u\n",
                (unsigned long long)atomic_load(&g_pcm_bytes),
                peak, callbacks);
    }
}

int opensles_shim_engine_active(void) {
    return g_player.device != 0;
}
