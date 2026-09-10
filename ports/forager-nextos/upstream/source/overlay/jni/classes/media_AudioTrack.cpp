#include <vector>
#include <SDL2/SDL.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "media_AudioTrack.h"

static int GetSDLFormat(int audioFormat)
{
    switch (audioFormat) {
    case ENCODING_PCM_16BIT: return AUDIO_S16;
    case ENCODING_PCM_8BIT: return AUDIO_U8;
    case ENCODING_PCM_FLOAT: return AUDIO_F32SYS;
    default: return AUDIO_U8;
    }
}

static int GetSDLFormatBytes(int audioFormat)
{
    switch (audioFormat) {
    case ENCODING_PCM_16BIT: return 2;
    case ENCODING_PCM_8BIT: return 1;
    case ENCODING_PCM_FLOAT: return 4;
    default: return 1;
    }
}

/* [NextOS/forager] diagnostico de mudez — so fala com GMLOADER_AUDIO_DEBUG setado */
static int audio_dbg(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GMLOADER_AUDIO_DEBUG");
        v = (e && *e) ? 1 : 0;
    }
    return v;
}

AudioTrack::AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{
    SDL_zero(desired);
    desired.freq = sampleRateInHz;
    desired.format = GetSDLFormat(audioFormat);
    desired.channels = (channelConfig == 4) ? 1 : 2;
    desired.samples = bufferSizeInBytes / (desired.channels * GetSDLFormatBytes(audioFormat));
    desired.callback = NULL;
    playing = 0;

    needed_bytes = bufferSizeInBytes;
    deviceId = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    this->mode = mode;

    if (audio_dbg())
        printf("[audio] AudioTrack(rate=%d fmt=0x%x ch=%d bufsz=%d) -> dev=%u obtained(rate=%d fmt=0x%x ch=%d)\n",
               sampleRateInHz, audioFormat, desired.channels, bufferSizeInBytes,
               deviceId, obtained.freq, obtained.format, obtained.channels);
}

static void AudioClass_ctor1(JNIEnv *env, jobject obj, jclass clazz, int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{
    AudioTrack *track = new (obj) AudioTrack(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode);
}

int AudioTrack::getMinBufferSize(JNIEnv *env, jclass clazz, int sampleRateInHz, int channelConfig, int audioFormat)
{
    return 2048 * GetSDLFormatBytes(audioFormat);
}

void AudioTrack::play(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    track->playing = 1;
    SDL_PauseAudioDevice(track->deviceId, 0);
}

void AudioTrack::stop(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    track->playing = 0;
    SDL_PauseAudioDevice(track->deviceId, 1);
}

void AudioTrack::release(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    SDL_ClearQueuedAudio(track->deviceId);
}

int AudioTrack::write(JNIEnv *env, jobject obj, jclass clazz, jbyteArray audioData, int offsetInBytes, int sizeInBytes)
{
    return AudioTrack::write(env, obj, clazz, audioData, offsetInBytes, sizeInBytes, WRITE_BLOCKING);
}


int AudioTrack::write(JNIEnv *env, jobject obj, jclass clazz, jbyteArray audioData, int offsetInBytes, int sizeInBytes, int writeMode)
{
    Class *clz = (Class*)clazz;
    AudioTrack *track = (AudioTrack*)obj;
    ArrayObject *data = (ArrayObject*)audioData;
    uintptr_t where = (uintptr_t)data->elements + offsetInBytes;

    if (audio_dbg()) {
        static int n = 0;
        if ((n++ % 50) == 0 && track->desired.format == AUDIO_S16) {
            const int16_t *s = (const int16_t *)where;
            int cnt = sizeInBytes / 2, peak = 0;
            for (int i = 0; i < cnt; i++) {
                int a = s[i] < 0 ? -s[i] : s[i];
                if (a > peak) peak = a;
            }
            printf("[audio] write #%d bytes=%d PICO=%d\n", n - 1, sizeInBytes, peak);
        }
    }

    int ret = SDL_QueueAudio(track->deviceId, (void*)where, sizeInBytes);

    if (track->playing == 0)
        AudioTrack::play(env, obj, clazz);

    /* [NextOS/forager] BLOQUEAR ate a fila ZERAR e o bug do MUDO: no NextOS o
     * libSDL2-2.0.so.0 e o sdl2-compat sobre SDL3, e SDL_GetQueuedAudioSize nunca
     * volta a 0 com o stream vivo — o thread de audio do runner escrevia UM buffer
     * de 46 ms e ficava preso aqui para sempre (provado por gdb: nanosleep dentro
     * do SDL_Delay). Semantica correta de write bloqueante: segurar o produtor
     * apenas enquanto ha mais de 2 buffers pendentes — nunca exigir dreno total
     * (dreno total = underrun ate no SDL2 real). */
    if (writeMode == WRITE_BLOCKING) {
        Uint32 max_pending = (Uint32)(track->needed_bytes > 0 ? track->needed_bytes * 2 : 8192);
        while (SDL_GetQueuedAudioSize(track->deviceId) > max_pending)
            SDL_Delay(1);
    }

    if (ret == 0)
        return sizeInBytes;
    else
        return 0;
}

const FieldId mediaAudioTrackClassFields[] = {
    {NULL},
};

const ManagedMethod mediaAudioTrackClassMethods[] = {
    REGISTER_INIT_METHOD(AudioTrack, AudioClass_ctor1, "(IIIIII)V"),
    REGISTER_STATIC_METHOD(AudioTrack, getMinBufferSize, "(III)I"),
    REGISTER_NONVIRTUAL(AudioTrack, play   , "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, stop   , "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, release, "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, write, "([BII)I", int, jbyteArray, int, int),
    REGISTER_NONVIRTUAL(AudioTrack, write, "([BIII)I", int, jbyteArray, int, int, int),
    NULL
};

Class AudioTrack::clazz = {
    .classpath = "android/media/AudioTrack",
    .classname = "AudioClass",
    .managed_methods = mediaAudioTrackClassMethods,
    .fields = mediaAudioTrackClassFields,
    .instance_size = sizeof(AudioTrack)
};

static const int registered = ClassRegistry::register_class(AudioTrack::clazz);
