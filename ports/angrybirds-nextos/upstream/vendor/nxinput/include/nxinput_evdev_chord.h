/* nxinput_evdev_chord.h — SELECT+START exit chord read from raw evdev, with the
 * physical SELECT/START key codes derived from the SDL_GameController mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why this exists (2026-08-17): every "raw evdev" exit chord in the house read
 * BTN_SELECT (0x13a) + BTN_START (0x13b) literally, with BTN_TRIGGER_HAPPY1/2
 * (0x2c0/0x2c1) as the RK3326 alternative. Some firmware/kernel pad drivers
 * (one widespread H700 family, per its own es_input.cfg) emit code 314/315
 * (= BTN_SELECT/BTN_START) for the physical **L2/R2** and 310/311
 * (= BTN_TL/BTN_TR) for the physical SELECT/START. Result on those devices:
 * "L2+R2 closes the game".
 *
 * The firmware mapping (SDL_GAMECONTROLLERCONFIG from ES/PortMaster) is the
 * only authority on which physical key is SELECT and which is START. So the
 * chord takes the SDL binds for BACK and START, converts each SDL button index
 * back to the evdev key code the same way SDL2's Linux joystick driver
 * enumerates them (BTN_JOYSTICK..KEY_MAX first, then 0..BTN_JOYSTICK-1) and
 * watches THOSE codes. Only when there is no SDL bind at all does it fall back
 * to the raw heuristics (TRIGGER_HAPPY1/2, then BTN_SELECT/START, then
 * BTN_BASE3/4), and it says so in the log.
 *
 * Single header, C99/C++ friendly. In exactly one translation unit:
 *     #define NXINPUT_EVDEV_CHORD_IMPLEMENTATION
 *     #include "nxinput_evdev_chord.h"
 * Optionally define NXINPUT_EVDEV_CHORD_LOG(fmt, ...) before including to
 * route messages (default: fprintf(stderr)).
 *
 * Usage:
 *     nx_evdev_chord_open();                       // scan /dev/input/event*
 *     nx_evdev_chord_bind_sdl(controller);          // once a pad is open
 *     if (nx_evdev_chord_poll()) request_exit();   // every frame
 *     nx_evdev_chord_close();
 * The chord fires once per press (edge), only when BOTH keys are down.
 */
#ifndef NXINPUT_EVDEV_CHORD_H
#define NXINPUT_EVDEV_CHORD_H

#ifdef __cplusplus
extern "C" {
#endif

struct _SDL_GameController;

/* Scan /dev/input/event0..31 for gamepad-like devices. Idempotent. */
void nx_evdev_chord_open(void);
/* Derive SELECT/START codes from the SDL mapping of `controller` (may be NULL:
 * keeps/falls back to raw heuristics). Safe to call again on hotplug. */
void nx_evdev_chord_bind_sdl(struct _SDL_GameController *controller);
/* Drain events; returns 1 on the frame the chord becomes fully pressed. */
int nx_evdev_chord_poll(void);
/* Number of evdev pads being watched (0 = chord depends on SDL only). */
int nx_evdev_chord_pad_count(void);
void nx_evdev_chord_close(void);

#ifdef __cplusplus
}
#endif

#ifdef NXINPUT_EVDEV_CHORD_IMPLEMENTATION

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <SDL.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef NXINPUT_EVDEV_CHORD_LOG
#define NXINPUT_EVDEV_CHORD_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef NX_EVDEV_CHORD_MAX_PADS
#define NX_EVDEV_CHORD_MAX_PADS 8
#endif

/* Codes are kernel ABI: stable regardless of the header vintage. */
#define NX_EVC_BTN_MISC 0x100
#define NX_EVC_BTN_JOYSTICK 0x120
#define NX_EVC_BTN_BASE3 0x128
#define NX_EVC_BTN_BASE4 0x129
#define NX_EVC_BTN_SOUTH 0x130
#define NX_EVC_BTN_SELECT 0x13a
#define NX_EVC_BTN_START 0x13b
#define NX_EVC_BTN_TRIGGER_HAPPY1 0x2c0
#define NX_EVC_BTN_TRIGGER_HAPPY2 0x2c1
#define NX_EVC_KEY_MAX 0x2ff
#define NX_EVC_NLONGS ((NX_EVC_KEY_MAX / (8 * (int)sizeof(unsigned long))) + 1)

typedef struct {
  int fd;
  int node;
  int code_select;
  int code_start;
  const char *source; /* "sdl-mapping" | "raw-trigger-happy" | ... */
  unsigned char down_select;
  unsigned char down_start;
  unsigned long keybits[NX_EVC_NLONGS];
  char name[80];
} nx_evc_pad;

static nx_evc_pad g_nx_evc_pads[NX_EVDEV_CHORD_MAX_PADS];
static int g_nx_evc_count = -1;
static int g_nx_evc_fired;

static int nx_evc_bit(const unsigned long *bits, int code) {
  if (code < 0 || code > NX_EVC_KEY_MAX)
    return 0;
  return (int)((bits[code / (8 * (int)sizeof(unsigned long))] >>
                (code % (8 * (int)sizeof(unsigned long)))) &
               1UL);
}

/* SDL2 Linux joystick: button index N is the N-th set key bit, enumerating
 * BTN_JOYSTICK..KEY_MAX first and then 0..BTN_JOYSTICK-1. */
static int nx_evc_code_for_sdl_index(const unsigned long *bits, int index) {
  int code;
  int n = 0;
  if (index < 0)
    return -1;
  for (code = NX_EVC_BTN_JOYSTICK; code <= NX_EVC_KEY_MAX; ++code)
    if (nx_evc_bit(bits, code)) {
      if (n == index)
        return code;
      ++n;
    }
  for (code = 0; code < NX_EVC_BTN_JOYSTICK; ++code)
    if (nx_evc_bit(bits, code)) {
      if (n == index)
        return code;
      ++n;
    }
  return -1;
}

static void nx_evc_apply_raw_fallback(nx_evc_pad *pad) {
  if (nx_evc_bit(pad->keybits, NX_EVC_BTN_TRIGGER_HAPPY1) &&
      nx_evc_bit(pad->keybits, NX_EVC_BTN_TRIGGER_HAPPY2)) {
    pad->code_select = NX_EVC_BTN_TRIGGER_HAPPY1;
    pad->code_start = NX_EVC_BTN_TRIGGER_HAPPY2;
    pad->source = "raw-trigger-happy";
  } else if (nx_evc_bit(pad->keybits, NX_EVC_BTN_SELECT) &&
             nx_evc_bit(pad->keybits, NX_EVC_BTN_START)) {
    pad->code_select = NX_EVC_BTN_SELECT;
    pad->code_start = NX_EVC_BTN_START;
    pad->source = "raw-select-start";
  } else if (nx_evc_bit(pad->keybits, NX_EVC_BTN_BASE3) &&
             nx_evc_bit(pad->keybits, NX_EVC_BTN_BASE4)) {
    pad->code_select = NX_EVC_BTN_BASE3;
    pad->code_start = NX_EVC_BTN_BASE4;
    pad->source = "raw-base3-base4";
  } else {
    pad->code_select = -1;
    pad->code_start = -1;
    pad->source = "none";
  }
}

static int nx_evc_is_gamepad(const unsigned long *bits) {
  return nx_evc_bit(bits, NX_EVC_BTN_SOUTH) ||
         nx_evc_bit(bits, NX_EVC_BTN_TRIGGER_HAPPY1) ||
         nx_evc_bit(bits, NX_EVC_BTN_SELECT) ||
         nx_evc_bit(bits, NX_EVC_BTN_BASE3);
}

void nx_evdev_chord_open(void) {
  int index;
  if (g_nx_evc_count >= 0)
    return;
  g_nx_evc_count = 0;
  g_nx_evc_fired = 0;
  for (index = 0; index < 32 && g_nx_evc_count < NX_EVDEV_CHORD_MAX_PADS;
       ++index) {
    char path[64];
    nx_evc_pad *pad = &g_nx_evc_pads[g_nx_evc_count];
    int fd;
    snprintf(path, sizeof path, "/dev/input/event%d", index);
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(pad, 0, sizeof *pad);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof pad->keybits), pad->keybits) < 0 ||
        !nx_evc_is_gamepad(pad->keybits)) {
      close(fd);
      continue;
    }
    pad->fd = fd;
    pad->node = index;
    strcpy(pad->name, "?");
    (void)ioctl(fd, EVIOCGNAME(sizeof pad->name - 1), pad->name);
    nx_evc_apply_raw_fallback(pad);
    NXINPUT_EVDEV_CHORD_LOG(
        "EXIT chord evdev: %s (%s) select=0x%x start=0x%x source=%s\n", path,
        pad->name, pad->code_select, pad->code_start, pad->source);
    g_nx_evc_count++;
  }
  if (!g_nx_evc_count)
    NXINPUT_EVDEV_CHORD_LOG("EXIT chord evdev: no readable gamepad; "
                            "SELECT+START depends on the SDL mapping only\n");
}

void nx_evdev_chord_bind_sdl(struct _SDL_GameController *controller) {
  SDL_GameControllerButtonBind back, start;
  int i;
  if (g_nx_evc_count < 0)
    nx_evdev_chord_open();
  if (!controller || g_nx_evc_count <= 0)
    return;
  back = SDL_GameControllerGetBindForButton(controller,
                                            SDL_CONTROLLER_BUTTON_BACK);
  start = SDL_GameControllerGetBindForButton(controller,
                                             SDL_CONTROLLER_BUTTON_START);
  if (back.bindType != SDL_CONTROLLER_BINDTYPE_BUTTON ||
      start.bindType != SDL_CONTROLLER_BINDTYPE_BUTTON) {
    NXINPUT_EVDEV_CHORD_LOG("EXIT chord evdev: SDL mapping has no button bind "
                            "for BACK/START; keeping raw codes\n");
    return;
  }
  for (i = 0; i < g_nx_evc_count; ++i) {
    nx_evc_pad *pad = &g_nx_evc_pads[i];
    int sel = nx_evc_code_for_sdl_index(pad->keybits, back.value.button);
    int sta = nx_evc_code_for_sdl_index(pad->keybits, start.value.button);
    if (sel < 0 || sta < 0 || sel == sta) {
      NXINPUT_EVDEV_CHORD_LOG("EXIT chord evdev: event%d (%s) cannot map SDL "
                              "back=b%d start=b%d; keeping %s\n",
                              pad->node, pad->name, back.value.button,
                              start.value.button, pad->source);
      continue;
    }
    if (sel != pad->code_select || sta != pad->code_start ||
        strcmp(pad->source, "sdl-mapping") != 0) {
      pad->code_select = sel;
      pad->code_start = sta;
      pad->source = "sdl-mapping";
      pad->down_select = 0;
      pad->down_start = 0;
      NXINPUT_EVDEV_CHORD_LOG("EXIT chord evdev: event%d (%s) select=0x%x "
                              "start=0x%x source=sdl-mapping (back=b%d "
                              "start=b%d)\n",
                              pad->node, pad->name, sel, sta,
                              back.value.button, start.value.button);
    }
  }
}

int nx_evdev_chord_poll(void) {
  struct input_event ev;
  int i;
  int any_down = 0;
  if (g_nx_evc_count <= 0)
    return 0;
  for (i = 0; i < g_nx_evc_count; ++i) {
    nx_evc_pad *pad = &g_nx_evc_pads[i];
    while (read(pad->fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
      unsigned char down;
      if (ev.type != EV_KEY)
        continue;
      down = ev.value != 0; /* 1 press, 2 autorepeat */
      if ((int)ev.code == pad->code_select)
        pad->down_select = down;
      else if ((int)ev.code == pad->code_start)
        pad->down_start = down;
    }
    if (pad->down_select && pad->down_start)
      any_down = 1;
  }
  if (any_down && !g_nx_evc_fired) {
    g_nx_evc_fired = 1;
    return 1;
  }
  if (!any_down)
    g_nx_evc_fired = 0;
  return 0;
}

int nx_evdev_chord_pad_count(void) {
  return g_nx_evc_count > 0 ? g_nx_evc_count : 0;
}

void nx_evdev_chord_close(void) {
  int i;
  for (i = 0; i < g_nx_evc_count && i < NX_EVDEV_CHORD_MAX_PADS; ++i)
    close(g_nx_evc_pads[i].fd);
  g_nx_evc_count = -1;
  g_nx_evc_fired = 0;
}

#endif /* NXINPUT_EVDEV_CHORD_IMPLEMENTATION */
#endif /* NXINPUT_EVDEV_CHORD_H */
