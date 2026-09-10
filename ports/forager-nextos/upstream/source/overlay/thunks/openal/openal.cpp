#ifdef USE_OPENAL_THUNKS

#include <errno.h>
#include <stdlib.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx-creative.h>
#include <AL/efx-presets.h>
#include <AL/efx.h>
#include <pthread.h>

#include "platform.h"
#include "thunk_gen.h"
#include "so_util.h"

// Crashes the application, don't want to deal with this.
ABI_ATTR static void alBufferMarkNeedsFreed_impl(ALuint buffer)
{
    return;
}

ABI_ATTR static void alSourced_impl(ALuint source, ALenum param, ALdouble value)
{
    // Apparently, there are some missing LOOP_START and LOOP_END properties,
    // if a game has problems with these, then we'll have to start shipping
    // vendorized builds of OpenAL to emulate this behavior.
    // alSourcedSOFT(source, param, value);
}

/* [NextOS/forager] Diagnostico de mudez: SO loga com GMLOADER_AUDIO_DEBUG setado.
 * Sem a env var e um branch morto — o caminho de release nao muda (regra #26).
 * O objetivo e ver os VALORES de ganho/posicao que cruzam a fronteira softfp->hardfp
 * e os formatos dos buffers: ganho lixo (~1e-38) = bug de ABI; ganho 1.0 = conteudo. */
static int audio_dbg(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GMLOADER_AUDIO_DEBUG");
        v = (e && *e) ? 1 : 0;
    }
    return v;
}

ABI_ATTR static void alSourcef_dbg(ALuint src, ALenum param, ALfloat val)
{
    if (audio_dbg())
        printf("[audio] alSourcef(src=%u, param=0x%x, val=%g)\n", src, param, (double)val);
    alSourcef(src, param, val);
}

ABI_ATTR static void alListenerf_dbg(ALenum param, ALfloat val)
{
    if (audio_dbg())
        printf("[audio] alListenerf(param=0x%x, val=%g)\n", param, (double)val);
    alListenerf(param, val);
}

ABI_ATTR static void alSource3f_dbg(ALuint src, ALenum param, ALfloat a, ALfloat b, ALfloat c)
{
    if (audio_dbg())
        printf("[audio] alSource3f(src=%u, param=0x%x, %g, %g, %g)\n",
               src, param, (double)a, (double)b, (double)c);
    alSource3f(src, param, a, b, c);
}

ABI_ATTR static void alBufferData_dbg(ALuint buf, ALenum fmt, const ALvoid *data,
                                      ALsizei size, ALsizei freq)
{
    if (audio_dbg())
        printf("[audio] alBufferData(buf=%u, fmt=0x%x, size=%d, freq=%d)\n",
               buf, fmt, (int)size, (int)freq);
    alBufferData(buf, fmt, data, size, freq);
}

ABI_ATTR static void alSourcePlay_dbg(ALuint src)
{
    if (audio_dbg())
        printf("[audio] alSourcePlay(src=%u)\n", src);
    alSourcePlay(src);
}

DynLibFunction symtable_openal[] = {
    NO_THUNK("alBufferMarkNeedsFreed", (uintptr_t)&alBufferMarkNeedsFreed_impl),
    NO_THUNK("_Z22alBufferMarkNeedsFreedj", (uintptr_t)&alBufferMarkNeedsFreed_impl),

    NO_THUNK("alSourceBus", (uintptr_t)&alBufferMarkNeedsFreed_impl),
    NO_THUNK("_Z11alSourceBusjPv", (uintptr_t)&alBufferMarkNeedsFreed_impl),
    NO_THUNK("alSourced", (uintptr_t)&alSourced_impl),
    NO_THUNK("_Z9alSourcedjid", (uintptr_t)&alSourced_impl),
    NO_THUNK("alSourcedSOFT", (uintptr_t)&alSourced_impl),

    THUNK_DIRECT(alDopplerFactor),
    THUNK_DIRECT(alDopplerVelocity),
    THUNK_DIRECT(alSpeedOfSound),
    THUNK_DIRECT(alDistanceModel),
    THUNK_DIRECT(alEnable),
    THUNK_DIRECT(alDisable),
    THUNK_DIRECT(alGetBooleanv),
    THUNK_DIRECT(alGetIntegerv),
    THUNK_DIRECT(alGetFloatv),
    THUNK_DIRECT(alGetDoublev),
    NO_THUNK("alListenerf", (uintptr_t)&alListenerf_dbg),
    THUNK_DIRECT(alListener3f),
    THUNK_DIRECT(alListenerfv),
    THUNK_DIRECT(alListeneri),
    THUNK_DIRECT(alListener3i),
    THUNK_DIRECT(alListeneriv),
    THUNK_DIRECT(alGetListenerf),
    THUNK_DIRECT(alGetListener3f),
    THUNK_DIRECT(alGetListenerfv),
    THUNK_DIRECT(alGetListeneri),
    THUNK_DIRECT(alGetListener3i),
    THUNK_DIRECT(alGetListeneriv),
    THUNK_DIRECT(alGenSources),
    THUNK_DIRECT(alDeleteSources),
    NO_THUNK("alSourcef", (uintptr_t)&alSourcef_dbg),
    NO_THUNK("alSource3f", (uintptr_t)&alSource3f_dbg),
    THUNK_DIRECT(alSourcefv),
    THUNK_DIRECT(alSourcei),
    THUNK_DIRECT(alSource3i),
    THUNK_DIRECT(alSourceiv),
    THUNK_DIRECT(alGetSourcef),
    THUNK_DIRECT(alGetSource3f),
    THUNK_DIRECT(alGetSourcefv),
    THUNK_DIRECT(alGetSourcei),
    THUNK_DIRECT(alGetSource3i),
    THUNK_DIRECT(alGetSourceiv),
    THUNK_DIRECT(alSourcePlayv),
    THUNK_DIRECT(alSourceStopv),
    THUNK_DIRECT(alSourceRewindv),
    THUNK_DIRECT(alSourcePausev),
    NO_THUNK("alSourcePlay", (uintptr_t)&alSourcePlay_dbg),
    THUNK_DIRECT(alSourceStop),
    THUNK_DIRECT(alSourceRewind),
    THUNK_DIRECT(alSourcePause),
    THUNK_DIRECT(alSourceQueueBuffers),
    THUNK_DIRECT(alSourceUnqueueBuffers),
    THUNK_DIRECT(alGenBuffers),
    THUNK_DIRECT(alDeleteBuffers),
    NO_THUNK("alBufferData", (uintptr_t)&alBufferData_dbg),
    THUNK_DIRECT(alBufferf),
    THUNK_DIRECT(alBuffer3f),
    THUNK_DIRECT(alBufferfv),
    THUNK_DIRECT(alBufferi),
    THUNK_DIRECT(alBuffer3i),
    THUNK_DIRECT(alBufferiv),
    THUNK_DIRECT(alGetBufferf),
    THUNK_DIRECT(alGetBuffer3f),
    THUNK_DIRECT(alGetBufferfv),
    THUNK_DIRECT(alGetBufferi),
    THUNK_DIRECT(alGetBuffer3i),
    THUNK_DIRECT(alGetBufferiv),
    THUNK_DIRECT(alcGetIntegerv),
    THUNK_DIRECT(alcCaptureStart),
    THUNK_DIRECT(alcCaptureStop),
    THUNK_DIRECT(alcCaptureSamples),
    THUNK_DIRECT(alIsEnabled),
    THUNK_DIRECT(alGetBoolean),
    THUNK_DIRECT(alGetInteger),
    THUNK_DIRECT(alGetFloat),
    THUNK_DIRECT(alGetDouble),
    THUNK_DIRECT(alGetError),
    THUNK_DIRECT(alIsExtensionPresent),
    THUNK_DIRECT(alGetProcAddress),
    THUNK_DIRECT(alGetEnumValue),
    THUNK_DIRECT(alIsSource),
    THUNK_DIRECT(alIsBuffer),
    THUNK_DIRECT(alcOpenDevice),
    THUNK_DIRECT(alcCloseDevice),
    THUNK_DIRECT(alcGetError),
    THUNK_DIRECT(alcIsExtensionPresent),
    THUNK_DIRECT(alcGetProcAddress),
    THUNK_DIRECT(alcGetEnumValue),
    THUNK_DIRECT(alcCaptureOpenDevice),
    THUNK_DIRECT(alcCaptureCloseDevice),
    THUNK_DIRECT(alcProcessContext),
    THUNK_DIRECT(alcSuspendContext),
    THUNK_DIRECT(alcDestroyContext),
    THUNK_DIRECT(alcCreateContext),
    THUNK_DIRECT(alcMakeContextCurrent),
    THUNK_DIRECT(alcGetCurrentContext),
    THUNK_DIRECT(alcGetContextsDevice),
    NULL
};

#endif