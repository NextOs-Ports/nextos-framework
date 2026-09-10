/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * V5 / H2-H5 host harness: a PURE SDL2 consumer with the real C6 glue linked
 * in (exactly as the so-loader ports do), against the SDL2 this process
 * maps, over a real uinput pad the oracle driver created.
 *
 * It does NOT know the expected result. It only reports, per SDL event:
 *   EDGE instance=<id> semantic=<sdl button name> pressed=<0|1> seq=<n>
 *   AXIS instance=<id> semantic=<axis name> value=<sint16>
 *   CHORD instance=<id> (SELECT+START on the SAME instance, edge)
 * and the receipts the glue emits (NXC6-PROVIDER / NXC6-DOMAIN / NXC6-SEAM).
 * The Python driver (tests/v5/run-host-oracle.py) chooses the stimulus from
 * the physical profile and judges these lines with nxoracle_v5.
 *
 * Usage: harness_sdl2_provider <seconds> [expected-name-substring]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../engine-glue/nxc6_glue.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *bname(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "a"; case SDL_CONTROLLER_BUTTON_B: return "b";
    case SDL_CONTROLLER_BUTTON_X: return "x"; case SDL_CONTROLLER_BUTTON_Y: return "y";
    case SDL_CONTROLLER_BUTTON_BACK: return "back"; case SDL_CONTROLLER_BUTTON_GUIDE: return "guide";
    case SDL_CONTROLLER_BUTTON_START: return "start"; case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "leftstick";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "rightstick"; case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "leftshoulder";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "rightshoulder"; case SDL_CONTROLLER_BUTTON_DPAD_UP: return "dpup";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "dpdown"; case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "dpleft";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "dpright"; default: return "other";
  }
}
static const char *aname(int a) {
  switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX: return "leftx"; case SDL_CONTROLLER_AXIS_LEFTY: return "lefty";
    case SDL_CONTROLLER_AXIS_RIGHTX: return "rightx"; case SDL_CONTROLLER_AXIS_RIGHTY: return "righty";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "lefttrigger"; case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "righttrigger";
    default: return "other";
  }
}

int main(int argc, char **argv) {
  double seconds = argc > 1 ? atof(argv[1]) : 8.0;
  const char *want = argc > 2 ? argv[2] : NULL;
  SDL_GameController *pads[8] = {0};
  int sel[8] = {0}, sta[8] = {0}, chord[8] = {0};
  unsigned seq = 0;
  struct timespec t0, now;

  setvbuf(stdout, NULL, _IOLBF, 0);
  /* 0.11.1: the provider-aware staging (the ONE call a port makes before
   * SDL_Init). NXC6_STAGE_V5=1 selects it; without it the driver stages by
   * hand (NXC6_STAGED_MAPPING) exactly as the 0.11.0 ports did. */
  if (getenv("NXC6_STAGE_V5") != NULL) {
    static char staged[65536]; size_t n = 0;
    int rc = nxc6_stage_before_init(staged, sizeof staged, &n);
    printf("STAGE rc=%d staged_bytes=%lu env_still_set=%d\n", rc, (unsigned long)n, getenv("SDL_GAMECONTROLLERCONFIG") != NULL);
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
    printf("FATAL SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  printf("HARNESS sdl=%d.%d.%d rev=%s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetRevision());
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_JOYDEVICEADDED) {
        int i = e.jdevice.which;
        char guid[40];
        const char *name = SDL_JoystickNameForIndex(i);
        const char *path = SDL_JoystickPathForIndex(i);
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
        if (want && (!name || !strstr(name, want))) continue;
        /* The in-port admission: BEFORE SDL_IsGameController/Open, as fp2/nc/bt do. */
        if (!nxc6_admit_before_announce_named(SDL_JoystickGetDeviceInstanceID(i), guid, path, name)) {
          printf("ADMIT index=%d guid=%s result=refused\n", i, guid);
          continue;
        }
        if (!SDL_IsGameController(i)) { printf("ADMIT index=%d guid=%s result=not-gamecontroller\n", i, guid); continue; }
        {
          SDL_GameController *c = SDL_GameControllerOpen(i);
          if (c) {
            SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
            char *m = SDL_GameControllerMapping(c);
            int slot; for (slot = 0; slot < 8 && pads[slot]; slot++) {}
            if (slot < 8) pads[slot] = c;
            printf("OPEN instance=%d guid=%s mapping=%s\n", (int)id, guid, m ? m : "-");
            SDL_free(m);
          }
        }
      } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        int pressed = e.type == SDL_CONTROLLERBUTTONDOWN;
        int id = e.cbutton.which, s;
        printf("EDGE instance=%d semantic=%s pressed=%d seq=%u\n", id, bname(e.cbutton.button), pressed, ++seq);
        for (s = 0; s < 8; s++) {
          if (pads[s] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[s])) == id) {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) sel[s] = pressed;
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) sta[s] = pressed;
            if (sel[s] && sta[s] && !chord[s]) { chord[s] = 1; printf("CHORD instance=%d seq=%u\n", id, ++seq); }
            if (!(sel[s] && sta[s])) chord[s] = 0;
          }
        }
      } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        printf("AXIS instance=%d semantic=%s value=%d seq=%u\n", e.caxis.which, aname(e.caxis.axis), e.caxis.value, ++seq);
      } else if (e.type == SDL_JOYDEVICEREMOVED) {
        int s;
        for (s = 0; s < 8; s++) {
          if (pads[s] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[s])) == e.jdevice.which) {
            SDL_GameControllerClose(pads[s]); pads[s] = NULL; sel[s] = sta[s] = chord[s] = 0;
            nxc6_forget(e.jdevice.which);
            printf("REMOVED instance=%d\n", e.jdevice.which);
          }
        }
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9 > seconds) break;
    SDL_Delay(4);
  }
  { int s; for (s = 0; s < 8; s++) if (pads[s]) SDL_GameControllerClose(pads[s]); }
  SDL_Quit();
  printf("HARNESS exit=0\n");
  return 0;
}
