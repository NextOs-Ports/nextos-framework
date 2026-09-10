/*
 * audio.c -- the Java FMODAudioDevice/AudioTrack loop on native NextOS.
 *
 * Unity still owns the mixer.  Its original Android natives publish the
 * sample rate, DSP block size and channel count through fmodGetInfo, then fill
 * a DirectByteBuffer through fmodProcess.  Android would run that loop from
 * FMODAudioDevice.run() and write each block to AudioTrack; here a pthread
 * performs the same sequence and queues the unchanged signed-16 PCM to SDL.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nx_elf.h"
#include "gb.h"
#include "framework_bridge.h"
#include "jni_refs.h"

static pthread_t audio_thread;
static int audio_run;
static int audio_started;

typedef int (*fmod_process_fn)(void *, void *, void *);
typedef int (*fmod_get_info_fn)(void *, void *, int);

static int fmod_process_scoped(fmod_process_fn function, void *env,
                               void *device, void *bytebuffer)
{
    int scope = gb_jni_ref_scope_begin();
    if (scope < 0)
        return -1;
    int result = function(env, device, bytebuffer);
    gb_jni_ref_scope_end(scope);
    return result;
}

static int fmod_get_info_scoped(fmod_get_info_fn function, void *env,
                                void *device, int selector)
{
    int scope = gb_jni_ref_scope_begin();
    if (scope < 0)
        return INT_MIN;
    int result = function(env, device, selector);
    gb_jni_ref_scope_end(scope);
    return result;
}

static int silent_driver(const char *name)
{
    return !name || strcmp(name, "dummy") == 0 || strcmp(name, "disk") == 0;
}

static int pcm_peak_s16(const int16_t *samples, int count)
{
    int peak = 0;
    for (int i = 0; i < count; i++) {
        int value = samples[i];
        if (value < 0)
            value = value == -32768 ? 32768 : -value;
        if (value > peak)
            peak = value;
    }
    return peak;
}

static void log_audio_drivers(void)
{
    int count = SDL_GetNumAudioDrivers();
    fprintf(stderr, "[audio] SDL drivers:");
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDriver(i);
        if (name)
            fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n");
}

static SDL_AudioDeviceID open_audio(const SDL_AudioSpec *want,
                                    SDL_AudioSpec *have)
{
    const char *driver = SDL_GetCurrentAudioDriver();
    if (silent_driver(driver))
        return 0;
    return SDL_OpenAudioDevice(NULL, 0, want, have, 0);
}

static void *audio_main(void *unused)
{
    (void)unused;
    void *thread_env = gb_jni_env();
    fmod_process_fn fmod_process = NULL;
    fmod_get_info_fn fmod_get_info = NULL;

    while (__atomic_load_n(&audio_run, __ATOMIC_ACQUIRE)) {
        fmod_process = (fmod_process_fn)
            gb_jni_native("org/fmod/FMODAudioDevice", "fmodProcess");
        fmod_get_info = (fmod_get_info_fn)
            gb_jni_native("org/fmod/FMODAudioDevice", "fmodGetInfo");
        if (fmod_process && fmod_get_info)
            break;
        usleep(20000);
    }
    if (!fmod_process || !fmod_get_info)
        return NULL;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
        return NULL;
    }
    log_audio_drivers();
    fprintf(stderr, "[audio] backend selected: %s\n",
            SDL_GetCurrentAudioDriver()
                ? SDL_GetCurrentAudioDriver() : "(none)");

    void *device = gb_jni_fmod_device();
    void *bytebuffer = gb_jni_fmod_bytebuffer();
    void *pcm = gb_jni_fmod_pcm();
    unsigned open_failures = 0;
    int trace = getenv("GB_AUDIO_TRACE") != NULL;

    while (__atomic_load_n(&audio_run, __ATOMIC_ACQUIRE)) {
        if (!gb_jni_fmod_should_run() ||
            fmod_get_info_scoped(fmod_get_info, thread_env, device, 3) != 1) {
            usleep(10000);
            continue;
        }

        int rate = fmod_get_info_scoped(fmod_get_info, thread_env, device, 0);
        int block = fmod_get_info_scoped(fmod_get_info, thread_env, device, 1);
        int buffers = fmod_get_info_scoped(fmod_get_info, thread_env, device, 2);
        int channels = fmod_get_info_scoped(fmod_get_info, thread_env, device, 4);
        int64_t byte_count =
            (int64_t)block * channels * (int)sizeof(int16_t);
        if (rate < 8000 || rate > 192000 ||
            block < 16 || block > 16384 ||
            channels < 1 || channels > 2 ||
            byte_count < 64 ||
            byte_count > gb_jni_fmod_pcm_capacity()) {
            fprintf(stderr,
                    "[audio] invalid FMOD format: rate=%d block=%d "
                    "buffers=%d channels=%d bytes=%ld capacity=%d\n",
                    rate, block, buffers, channels, (long)byte_count,
                    gb_jni_fmod_pcm_capacity());
            usleep(100000);
            continue;
        }

        int bytes = (int)byte_count;
        gb_jni_fmod_set_buffer_size(bytes);
        if (buffers < 2)
            buffers = 2;
        if (buffers > 16)
            buffers = 16;

        SDL_AudioSpec want;
        SDL_AudioSpec have;
        memset(&want, 0, sizeof want);
        memset(&have, 0, sizeof have);
        want.freq = rate;
        want.format = AUDIO_S16SYS;
        want.channels = (Uint8)channels;
        want.samples = (Uint16)(block < 128 ? 128 :
                                block > 4096 ? 4096 : block);

        SDL_AudioDeviceID output = open_audio(&want, &have);
        if (!output) {
            open_failures++;
            if (open_failures <= 3 || open_failures % 20 == 0)
                fprintf(stderr,
                        "[audio] SDL_OpenAudioDevice failed #%u: %s\n",
                        open_failures, SDL_GetError());
            usleep(open_failures < 8 ? 250000 : 500000);
            continue;
        }
        if (gb_framework_publish_audio(output, &have) != 0) {
            fprintf(stderr,
                    "[audio] framework rejected opened backend/spec\n");
            SDL_CloseAudioDevice(output);
            return NULL;
        }
        if (open_failures)
            fprintf(stderr, "[audio] output recovered after %u attempts\n",
                    open_failures);
        open_failures = 0;

        Uint32 target = (Uint32)bytes * (Uint32)buffers;
        unsigned long calls = 0;
        while (__atomic_load_n(&audio_run, __ATOMIC_ACQUIRE) &&
               gb_jni_fmod_should_run() &&
               fmod_get_info_scoped(
                   fmod_get_info, thread_env, device, 3) == 1 &&
               SDL_GetQueuedAudioSize(output) < target) {
            int result = fmod_process_scoped(
                fmod_process, thread_env, device, bytebuffer);
            if (result != 0 ||
                SDL_QueueAudio(output, pcm, (Uint32)bytes) != 0)
                break;
            calls++;
            if (trace && calls <= 8)
                fprintf(stderr, "[audio] mix=%lu result=%d peak=%d\n",
                        calls, result,
                        pcm_peak_s16(pcm, bytes / (int)sizeof(int16_t)));
        }
        SDL_PauseAudioDevice(output, 0);
        fprintf(stderr,
                "[audio] active: %d Hz, %d channel(s), block=%d, queue=%d "
                "(SDL %d Hz/%d channel(s), driver=%s)\n",
                rate, channels, block, buffers, have.freq, have.channels,
                SDL_GetCurrentAudioDriver()
                    ? SDL_GetCurrentAudioDriver() : "?");

        while (__atomic_load_n(&audio_run, __ATOMIC_ACQUIRE) &&
               gb_jni_fmod_should_run() &&
               fmod_get_info_scoped(
                   fmod_get_info, thread_env, device, 3) == 1) {
            if ((calls & 255UL) == 0 &&
                (fmod_get_info_scoped(
                     fmod_get_info, thread_env, device, 0) != rate ||
                 fmod_get_info_scoped(
                     fmod_get_info, thread_env, device, 1) != block ||
                 fmod_get_info_scoped(
                     fmod_get_info, thread_env, device, 4) != channels))
                break;
            if (SDL_GetQueuedAudioSize(output) >= target) {
                usleep(1000);
                continue;
            }
            int result = fmod_process_scoped(
                fmod_process, thread_env, device, bytebuffer);
            if (result == 0) {
                int peak = trace
                    ? pcm_peak_s16(pcm, bytes / (int)sizeof(int16_t)) : 0;
                if (SDL_QueueAudio(output, pcm, (Uint32)bytes) != 0)
                    break;
                calls++;
                if (trace && (calls <= 8 || calls % 256 == 0))
                    fprintf(stderr, "[audio] mix=%lu result=%d peak=%d\n",
                            calls, result, peak);
            } else {
                usleep(5000);
            }
        }

        SDL_PauseAudioDevice(output, 1);
        SDL_ClearQueuedAudio(output);
        SDL_CloseAudioDevice(output);
        fprintf(stderr, "[audio] output stopped/restarting\n");
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return NULL;
}

int gb_audio_start(void *env)
{
    (void)env;
    if (getenv("GB_NO_AUDIO"))
        return 0;
    if (audio_started)
        return 1;
    __atomic_store_n(&audio_run, 1, __ATOMIC_RELEASE);
    int rc = pthread_create(&audio_thread, NULL, audio_main, NULL);
    if (rc != 0) {
        __atomic_store_n(&audio_run, 0, __ATOMIC_RELEASE);
        fprintf(stderr, "[audio] cannot create FMOD thread: %s\n",
                strerror(rc));
        return 0;
    }
    audio_started = 1;
    fprintf(stderr, "[audio] FMOD AudioTrack thread created\n");
    return 1;
}

void gb_audio_stop(void)
{
    if (!audio_started)
        return;
    __atomic_store_n(&audio_run, 0, __ATOMIC_RELEASE);
    pthread_join(audio_thread, NULL);
    audio_started = 0;
}
