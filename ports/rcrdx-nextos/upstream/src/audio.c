/*
 * audio.c -- the Java AudioTrack surface the game's SDL writes into.
 *
 * Measured, not assumed: the SDL inside libRCRDX.so lists exactly two audio
 * drivers, "android" and "dummy", and there is no OpenSL ES symbol imported
 * anywhere in the binary -- libOpenSLES.so sits in DT_NEEDED but nothing calls
 * it.  So the native flow is the old Android one: SDL_OpenAudioDevice picks
 * its android driver, which calls SDLActivity.audioInit(rate, is16Bit,
 * isStereo, desiredFrames), allocates a short[] or byte[] of the size that
 * call returns, and then hands each filled buffer to
 * SDLActivity.audioWriteShortBuffer / audioWriteByteBuffer from its own audio
 * thread.
 *
 * This file is the AudioTrack on the other side of those three calls: one
 * output device on the SYSTEM SDL2 (so the driver choice stays with the
 * system, as the rules require -- nothing here sets SDL_AUDIODRIVER) fed by
 * SDL_QueueAudio.  AudioTrack.write blocks when the track is full, and SDL's
 * audio thread depends on that back-pressure for its pacing, so the write
 * waits while the queue is deep.
 *
 * The wait is against a HIGH-WATER MARK, never against "queue empty":
 * NextOS' SDL2 is sdl2-compat over SDL3 and SDL_GetQueuedAudioSize there never
 * returns 0, so a drain loop would hang after one buffer and the game would
 * run mute.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "nx_elf.h"
#include "rcr.h"

static SDL_AudioDeviceID dev;
static SDL_AudioSpec spec;
static size_t buffer_bytes;
static unsigned long writes;
static unsigned long waits;

const char *rcr_sys_audiodriver;

/*
 * main.c takes SDL_AUDIODRIVER out of the environment before the game's own
 * SDL can read it (the game's SDL only knows "android"/"dummy" and refuses
 * anything else outright, which is what silences the game when the ES exports
 * EmuELEC's "pulseaudio,alsa").  The choice still belongs to the system: if
 * that value names a driver the SYSTEM SDL actually has, it goes back in place
 * for the init below.  A list, or a name this SDL does not have, is simply
 * left out and SDL probes in its own order -- nothing here forces a driver.
 */
static void restore_driver_env(void)
{
    if (!rcr_sys_audiodriver)
        return;
    int n = SDL_GetNumAudioDrivers();
    for (int i = 0; i < n; i++) {
        const char *have = SDL_GetAudioDriver(i);
        if (have && strcmp(have, rcr_sys_audiodriver) == 0) {
            setenv("SDL_AUDIODRIVER", rcr_sys_audiodriver, 1);
            return;
        }
    }
    fprintf(stderr, "[rcr] audio: SDL_AUDIODRIVER=\"%s\" nao e um driver desta "
                    "SDL; deixando o SDL escolher sozinho\n",
            rcr_sys_audiodriver);
}

int rcr_audio_open(int rate, int is16bit, int stereo, int frames)
{
    if (dev) {
        SDL_CloseAudioDevice(dev);
        dev = 0;
    }
    restore_driver_env();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[rcr] SDL_INIT_AUDIO: %s\n", SDL_GetError());
        return -1;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = rate > 0 ? rate : 44100;
    want.format = is16bit ? AUDIO_S16SYS : AUDIO_U8;
    want.channels = (Uint8)(stereo ? 2 : 1);
    /* The game asks for a period; SDL's android driver uses whatever comes
     * back as its buffer size, so honour the request and let the system
     * device round it. */
    want.samples = (Uint16)(frames > 0 ? frames : 2048);
    want.callback = NULL;               /* queue-fed, like AudioTrack */

    dev = SDL_OpenAudioDevice(NULL, 0, &want, &spec, 0);
    if (!dev) {
        fprintf(stderr, "[rcr] audio: SDL_OpenAudioDevice failed: %s\n",
                SDL_GetError());
        return -1;
    }
    buffer_bytes = (size_t)spec.samples * spec.channels *
                   (size_t)(SDL_AUDIO_BITSIZE(spec.format) / 8);
    SDL_PauseAudioDevice(dev, 0);
    /* 🚨 The probe the rules ask for: audio that finds no backend runs mute
     * without complaining, so say out loud which driver actually opened. */
    fprintf(stderr, "[rcr] audio: driver=%s %dHz %s %s, %d frames "
                    "(%zu bytes/buffer), pedido %dHz %s %s %d frames\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            spec.freq, spec.channels == 2 ? "stereo" : "mono",
            SDL_AUDIO_BITSIZE(spec.format) == 16 ? "16-bit" : "8-bit",
            spec.samples, buffer_bytes, rate, stereo ? "stereo" : "mono",
            is16bit ? "16-bit" : "8-bit", frames);
    /* 🚨 Measured in Android_JNI_OpenAudioDevice: this SDL treats a NON-ZERO
     * return from audioInit as failure ("SDL audio: error on AudioTrack
     * initialization!") and the game then exits.  Zero is success; the buffer
     * itself is allocated on the native side from the frame count it asked
     * for, not from anything we return. */
    return 0;
}

void rcr_audio_write(const void *data, size_t bytes)
{
    if (!dev || !data || !bytes)
        return;
    if (SDL_QueueAudio(dev, data, (Uint32)bytes) != 0) {
        fprintf(stderr, "[rcr] audio: SDL_QueueAudio: %s\n", SDL_GetError());
        return;
    }
    writes++;

    /* Back-pressure, high-water mark only.  Four buffers of slack is what the
     * game's own period gives an AudioTrack; the ceiling on the wait keeps a
     * stalled device from freezing the game's audio thread. */
    size_t high = buffer_bytes ? buffer_bytes * 4 : bytes * 4;
    for (int i = 0; i < 200 && SDL_GetQueuedAudioSize(dev) > high; i++) {
        waits++;
        SDL_Delay(1);
    }

    if (rcr_verbose_audio && (writes % 500) == 0)
        fprintf(stderr, "[rcr] audio: %lu buffers, fila %u B, %lu esperas\n",
                writes, (unsigned)SDL_GetQueuedAudioSize(dev), waits);
}

void rcr_audio_close(void)
{
    if (!dev)
        return;
    fprintf(stderr, "[rcr] audio: closing after %lu buffers\n", writes);
    SDL_CloseAudioDevice(dev);
    dev = 0;
}

unsigned long rcr_audio_writes(void) { return writes; }
