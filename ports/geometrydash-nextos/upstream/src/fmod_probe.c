/*
 * fmod_probe.c -- watch the engine's calls into FMOD.
 *
 * The audio route is provably fine (PulseAudio shows the stream running and
 * unmuted) and FMOD's mixer is feeding blocks at real-time rate -- but every
 * block is pure silence.  So the question is upstream: does the engine manage
 * to open its sounds at all?  These wrappers sit in the import table the engine
 * is resolved against, so they see every call and its FMOD_RESULT, and then
 * hand the call straight to the real function in libfmod.
 *
 * Diagnostic only: with GD_FMOD_LOG unset it is a thin passthrough.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>

#include "so_util.h"
#include "util.h"

extern const char *gd_asset_path(const char *url, char *out, size_t outsz);

/* FMOD is handed "file:///android_asset/NAME"; there is no AssetManager here,
 * so the name is turned into a real path served out of game.apk. */
static const char *real_path(const char *name, char *buf, size_t bufsz) {
  const char *p = gd_asset_path(name, buf, bufsz);
  return p ? p : name;
}

static int probe_on(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("GD_FMOD_LOG");
    v = (e && *e && *e != '0');
  }
  return v;
}

/* Resolved from libfmod's own export table at load time. */
static int (*r_init)(void *self, int maxch, unsigned flags, void *drv);
static int (*r_createSound)(void *self, const char *name, unsigned mode,
                            void *exinfo, void **snd);
static int (*r_createStream)(void *self, const char *name, unsigned mode,
                             void *exinfo, void **snd);
static int (*r_playSound)(void *self, void *snd, void *grp, int paused,
                          void **chan);
static int (*r_setVolume)(void *self, float v);

/* FMOD_RESULT 0 is FMOD_OK; anything else is why the game is silent. */
static int p_init(void *self, int maxch, unsigned flags, void *drv) {
  int r = r_init ? r_init(self, maxch, flags, drv) : 0;
  debugPrintf("[fmod] System::init(maxch=%d, flags=0x%x) -> %d\n", maxch, flags,
              r);
  return r;
}

static int p_createSound(void *self, const char *name, unsigned mode,
                         void *exinfo, void **snd) {
  char buf[PATH_MAX];
  name = real_path(name, buf, sizeof(buf));
  int r = r_createSound ? r_createSound(self, name, mode, exinfo, snd) : 0;
  static unsigned n;
  if (r != 0 || (probe_on() && n < 40)) {
    n++;
    debugPrintf("[fmod] createSound(\"%s\", mode=0x%x) -> %d\n",
                name ? name : "(null)", mode, r);
  }
  return r;
}

static int p_createStream(void *self, const char *name, unsigned mode,
                          void *exinfo, void **snd) {
  char buf[PATH_MAX];
  name = real_path(name, buf, sizeof(buf));
  int r = r_createStream ? r_createStream(self, name, mode, exinfo, snd) : 0;
  static unsigned n;
  if (r != 0 || (probe_on() && n < 40)) {
    n++;
    debugPrintf("[fmod] createStream(\"%s\", mode=0x%x) -> %d\n",
                name ? name : "(null)", mode, r);
  }
  return r;
}

static int p_playSound(void *self, void *snd, void *grp, int paused,
                       void **chan) {
  int r = r_playSound ? r_playSound(self, snd, grp, paused, chan) : 0;
  static unsigned n;
  if (r != 0 || (probe_on() && n < 40)) {
    n++;
    debugPrintf("[fmod] playSound(som=%p, pausado=%d) -> %d\n", snd, paused, r);
  }
  return r;
}

static int p_setVolume(void *self, float v) {
  int r = r_setVolume ? r_setVolume(self, v) : 0;
  static unsigned n;
  if (probe_on() && n < 40) {
    n++;
    debugPrintf("[fmod] ChannelControl::setVolume(%.3f) -> %d\n", (double)v, r);
  }
  return r;
}

/* The engine resolves these names out of the table below; each entry keeps the
 * real address so the wrapper can forward. */
static const struct {
  const char *name;
  void **slot;
  void *wrapper;
} PROBES[] = {
    {"_ZN4FMOD6System4initEijPv", (void **)&r_init, (void *)&p_init},
    {"_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
     (void **)&r_createSound, (void *)&p_createSound},
    {"_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
     (void **)&r_createStream, (void *)&p_createStream},
    {"_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE",
     (void **)&r_playSound, (void *)&p_playSound},
    {"_ZN4FMOD14ChannelControl9setVolumeEf", (void **)&r_setVolume,
     (void *)&p_setVolume},
};
#define PROBE_COUNT ((int)(sizeof(PROBES) / sizeof(PROBES[0])))

/* Builds a table of overrides to place AHEAD of libfmod's own exports, given
 * that library's symbol snapshot.  Returns NULL when nothing matched. */
DynLibFunction *gd_fmod_probe_table(DynLibFunction *fmod, int fmod_n,
                                    int *out_n) {
  DynLibFunction *t = calloc((size_t)PROBE_COUNT, sizeof(DynLibFunction));
  int n = 0;
  for (int i = 0; i < PROBE_COUNT; i++) {
    uintptr_t real = 0;
    for (int k = 0; k < fmod_n; k++)
      if (fmod[k].symbol && !strcmp(fmod[k].symbol, PROBES[i].name)) {
        real = fmod[k].func;
        break;
      }
    if (!real) {
      debugPrintf("[fmod] %s nao existe em libfmod -- sem sonda\n",
                  PROBES[i].name);
      continue;
    }
    *PROBES[i].slot = (void *)real;
    t[n].symbol = (char *)PROBES[i].name;
    t[n].func = (uintptr_t)PROBES[i].wrapper;
    n++;
  }
  *out_n = n;
  if (!n) {
    free(t);
    return NULL;
  }
  debugPrintf("[fmod] %d sondas instaladas\n", n);
  return t;
}
