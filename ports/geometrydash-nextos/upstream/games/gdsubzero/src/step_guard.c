/*
 * step_guard.c -- a ceiling on how much game time one frame may simulate.
 *
 * GJBaseGameLayer::getModifiedDelta() is the engine's fixed-step accumulator:
 * it adds the frame's delta to a carry, cuts the total into 1/240 s ticks and
 * returns how much time those ticks cover; the caller then runs the collision
 * pass once per tick.  There is no ceiling on the tick count, so any long
 * frame -- and loading a level out of the 217 MB apk costs about a second --
 * asks the next frame to simulate that whole second in one go.  On this CPU
 * that never finishes: the game does not crash, nativeRender simply never
 * returns, and the picture freezes on "Attempt 1" while the audio thread
 * carries on.
 *
 * Every fixed-step engine needs this guard (the "spiral of death" clamp); this
 * build of the engine ships without one because a phone never stalls that long.
 * The original arithmetic is reproduced exactly -- read off the shipped code --
 * and only the tick count is capped.  The carry keeps the remainder, so time is
 * not invented; a stall is simply not repaid all at once.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>

#include "so_util.h"
#include "util.h"

/* Field offsets read from the shipped GJBaseGameLayer::getModifiedDelta. */
#define OFF_TIME_WARP 816    /* float  */
#define OFF_CARRY 12512      /* double */
#define OFF_SKIP_FRAMES 12596 /* int   */

static void (*e_startMusic)(void *self);
static void *(*e_sharedDirector)(void);

#define STEP_SECONDS (1.0 / 240.0)
#define MAX_TICKS 24 /* 0.1 s of simulation per frame, then catch up later */

static double gd_getModifiedDelta(void *self, float dt) {
  char *o = (char *)self;
  float time_warp = *(float *)(o + OFF_TIME_WARP);
  double carry = *(double *)(o + OFF_CARRY);
  int32_t *skip = (int32_t *)(o + OFF_SKIP_FRAMES);

  /* The first frames after a level loads are deliberately not simulated. */
  double add = (double)dt;
  if (*skip > 0) {
    (*skip)--;
    add = 0.0;
  }

  double step = STEP_SECONDS;
  if (time_warp < 1.0f)
    step *= (double)time_warp;

  /* The engine rounds the carry through a float before dividing; keeping that
   * makes the tick count identical to the original. */
  carry = (double)(float)(add + carry);

  double ticks_f = (step > 0.0) ? carry / step : 0.0;
  long ticks = (ticks_f > 0.0 || ticks_f < 0.0) ? lround(ticks_f) : 0;
  if (!isfinite(ticks_f))
    ticks = 0;

  if (ticks > MAX_TICKS) {
    static unsigned reported;
    if (reported++ < 8)
      debugPrintf("[passo] frame pediu %ld passos de fisica; limitado a %d "
                  "(sobra fica no acumulador)\n",
                  ticks, MAX_TICKS);
    ticks = MAX_TICKS;
  }

  double used = step * (double)ticks;
  *(double *)(o + OFF_CARRY) = carry - used;
  static unsigned seen;
  if (seen++ < 12) {
    float dir_dt = 0.0f, smooth = 0, playing = 0;
    double interval = 0.0;
    int counter = 0;
    if (e_sharedDirector) {
      char *d = (char *)e_sharedDirector();
      dir_dt = *(float *)(d + 160);
      smooth = *(unsigned char *)(d + 169);
      playing = *(unsigned char *)(d + 170);
      interval = *(double *)(d + 192);
      counter = *(int *)(d + 172);
    }
    debugPrintf("[passo] #%u dt=%.6f carry=%.6f ticks=%ld skip=%d | diretor: "
                "deltaTime=%.6f intervalo=%.6f smooth=%d flag=%d cont=%d\n",
                seen - 1, (double)dt, carry, ticks, *skip, (double)dir_dt,
                interval, (int)smooth, (int)playing, counter);
  }
  return used;
}


/* ------------------------------------------------------- level start probe --
 * The level's start is scheduled as an action: CCDelayTime(0.5) followed by a
 * CCCallFunc on PlayLayer::startGameDelayed.  Until it fires, the game layer
 * takes its "not playing" branch every frame -- the picture is drawn, nothing
 * moves.  The whole of startGameDelayed is "set the playing flag, then call
 * startMusic", so it is reproduced here to say out loud whether it ever runs.
 */
#define OFF_PLAYING (0x3000 + 232)


static void gd_startGameDelayed(void *self) {
  *((unsigned char *)self + OFF_PLAYING) = 1;
  debugPrintf("[nivel] startGameDelayed disparou -- o nivel comecou\n");
  if (e_startMusic)
    e_startMusic(self);
}

static void install_level_probe(void) {
  uintptr_t d = so_find_addr_safe("_ZN9PlayLayer16startGameDelayedEv");
  e_startMusic = (void *)so_find_addr_safe("_ZN9PlayLayer10startMusicEv");
  e_sharedDirector =
      (void *)so_find_addr_safe("_ZN7cocos2d10CCDirector14sharedDirectorEv");
  if (!d || !e_startMusic) {
    debugPrintf("[nivel] startGameDelayed/startMusic nao encontrados\n");
    return;
  }
  hook_arm64(d, (uintptr_t)&gd_startGameDelayed);
  debugPrintf("[nivel] sonda de inicio de nivel instalada\n");
}

/* Called once, after the engine is loaded and relocated. */
void gd_install_step_guard(void) {
  /* so_finalize() has already turned the text read-only by this point. */
  so_make_text_writable();
  uintptr_t a = so_find_addr_safe("_ZN15GJBaseGameLayer16getModifiedDeltaEf");
  if (!a) {
    debugPrintf("[passo] getModifiedDelta nao encontrado -- sem trava\n");
    install_level_probe();
    return;
  }
  hook_arm64(a, (uintptr_t)&gd_getModifiedDelta);
  debugPrintf("[passo] trava de passo instalada em getModifiedDelta (max %d "
              "passos/frame)\n",
              MAX_TICKS);
  install_level_probe();
}
