/*
 * audio.c -- the OpenSL ES surface liblime's SDL audio backend opens.
 *
 * SDL 2.26 built for Android picks its openslES driver before the Java
 * AudioTrack one, so the native flow here is: SDL calls slCreateEngine, asks
 * the engine for an output mix and an AudioPlayer with a SimpleBufferQueue,
 * and then feeds that queue from its own audio thread.  The shim answers that
 * exact contract and drains the queue into one SDL2 output device on this
 * side, leaving the driver choice to the system SDL as the rules require --
 * nothing here sets SDL_AUDIODRIVER.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "nx_elf.h"
#include "tr.h"
#include "opensles_shim.h"

/* Two interface IDs SDL names but never uses for anything we have to model:
 * the Android configuration interface (stream type hints) and the record
 * interface (SDL only asks for it when capture is opened, which this game
 * never does).  They still need distinct addresses so GetInterface can tell
 * them apart from the ones we do implement. */
static const int id_androidconfig_tag = 8;
static const int id_record_tag = 9;
static const SLInterfaceID sl_IID_ANDROIDCONFIGURATION_v = &id_androidconfig_tag;
static const SLInterfaceID sl_IID_RECORD_v = &id_record_tag;

#define A(n, f) { n, (void *)(uintptr_t)(f) }

static const nx_import tab[] = {
    A("slCreateEngine", slCreateEngine_shim),
    A("SL_IID_ENGINE", &sl_IID_ENGINE),
    A("SL_IID_PLAY", &sl_IID_PLAY),
    A("SL_IID_VOLUME", &sl_IID_VOLUME),
    A("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &sl_IID_BUFFERQUEUE),
    A("SL_IID_BUFFERQUEUE", &sl_IID_BUFFERQUEUE),
    A("SL_IID_EFFECTSEND", &sl_IID_EFFECTSEND),
    A("SL_IID_ENGINECAPABILITIES", &sl_IID_ENGINECAPABILITIES),
    A("SL_IID_ENVIRONMENTALREVERB", &sl_IID_ENVIRONMENTALREVERB),
    A("SL_IID_ANDROIDCONFIGURATION", &sl_IID_ANDROIDCONFIGURATION_v),
    A("SL_IID_RECORD", &sl_IID_RECORD_v),
};

const nx_import *tr_audio_table(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}

void *tr_audio_sym(const char *name)
{
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return NULL;
}
