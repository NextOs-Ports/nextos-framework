/* nxinput_evdev_chord.h — SELECT+START exit chord (v2, 2026-08-18).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * POLICY (device-agnostic, learned the hard way on the H700 handheld family):
 *
 *  1. When an SDL_GameController is open, SDL IS THE AUTHORITY. The chord is
 *     `SDL_GameControllerGetButton(BACK) && ...(START)` polled by STATE on
 *     every open pad, plus the raw joystick buttons at the indices the
 *     mapping declares for back/start (covers a GameController layer that does
 *     not propagate). This is exactly what the published ports that exit fine
 *     on every CFW do (Sonic 4 EP2, Swordigo, ...). It fires on the edge, after
 *     NX_EXIT_CHORD_HOLD_POLLS consecutive polls (default 3 = ~50 ms at 60 Hz):
 *     no long hold. A one-second hold was reported as "does not exit".
 *
 *  2. Raw evdev is a FALLBACK used ONLY while no SDL pad is bound (pad without
 *     any mapping). Heuristics: TRIGGER_HAPPY1/2 (RK3326 family), then
 *     BTN_SELECT/BTN_START, then BTN_BASE3/4. Both keys must be seen on the
 *     SAME device.
 *
 *  3. NEVER watch literal BTN_SELECT/BTN_START while an SDL pad is bound: on
 *     one widespread H700 handheld family (several CFWs share the same vendor
 *     kernel) the vendor gpio-keys driver emits 0x13a/0x13b
 *     (BTN_SELECT/BTN_START) for the physical L2/R2 and 0x136/0x137
 *     (BTN_TL/BTN_TR) for the physical SELECT/START ("L2+R2 closes the game").
 *
 *  4. NEVER derive evdev codes from SDL button indices to drive the chord. SDL
 *     builds enumerate Linux buttons differently: vanilla SDL2 walks
 *     BTN_JOYSTICK..KEY_MAX first and then 0..BTN_JOYSTICK-1; one CFW family
 *     (and every firmware whose mapping table matches it) patches SDL2 to walk
 *     0..KEY_MAX ascending (`sdl2_input_as_retroarch_udev.patch`). Same device, same
 *     mapping string, different index->code table. Deriving with the wrong
 *     table silently turned SELECT+START into L3+L2 (v1 of this header,
 *     Forager 1.0.1/1.0.3). Both tables are kept here for DIAGNOSTICS only.
 *
 *  5. Diagnostics are mandatory: nx_exit_chord_log_controller() prints, once
 *     per pad, the SDL name/GUID/mapping/binds and, for every gamepad-like
 *     /dev/input/event*, its name, ids and the full list of key codes. One log
 *     from any device is enough to explain a chord bug without the hardware.
 *
 * Single header, C99/C++ friendly. In exactly one translation unit:
 *     #define NXINPUT_EVDEV_CHORD_IMPLEMENTATION
 *     #include "nxinput_evdev_chord.h"
 * Optionally define NXINPUT_EVDEV_CHORD_LOG(fmt, ...) before including to route
 * messages (default: fprintf(stderr)).
 *
 * Typical use (one call per frame, after SDL_PollEvent/SDL_GameControllerUpdate):
 *     nx_evdev_chord_open();                                  // once
 *     if (nx_exit_chord_update(pads, npads)) request_exit();  // every frame
 *     nx_evdev_chord_close();
 * `pads` may contain NULL entries. With no open pad the evdev fallback runs.
 */
#ifndef NXINPUT_EVDEV_CHORD_H
#define NXINPUT_EVDEV_CHORD_H

#ifdef __cplusplus
extern "C" {
#endif

struct _SDL_GameController;

/* Scan /dev/input/event0..31 for gamepad-like devices. Idempotent. */
void nx_evdev_chord_open(void);
/* Version-neutral authority hand-off for SDL3 or another complete primary
 * mapping. active=1 mutes evdev; active=0 keeps the independent fallback. */
void nx_evdev_chord_set_primary_active(int active);
/* Tell the chord which SDL pad (if any) is authoritative. Non-NULL disables
 * the evdev fallback (SDL rules); NULL re-enables it. Idempotent, cheap. */
void nx_evdev_chord_bind_sdl(struct _SDL_GameController *controller);
/* evdev FALLBACK: returns 1 on the frame the raw chord becomes fully pressed
 * on one device. Always 0 while an SDL pad is bound. */
int nx_evdev_chord_poll(void);
/* Number of evdev pads being watched (0 = chord depends on SDL only). */
int nx_evdev_chord_pad_count(void);
void nx_evdev_chord_close(void);

/* SDL PRIMARY: polls BACK+START (state) and the raw joystick buttons at the
 * mapping's back/start indices on every non-NULL pad. Returns 1 on the edge
 * after NX_EXIT_CHORD_HOLD_POLLS consecutive positive polls. */
int nx_exit_chord_poll_sdl(struct _SDL_GameController *const *pads, int count);
/* Convenience: bind the first non-NULL pad, then SDL primary || evdev fallback. */
int nx_exit_chord_update(struct _SDL_GameController *const *pads, int count);
/* Diagnostics receipt (once per distinct controller pointer unless force). */
void nx_exit_chord_log_controller(struct _SDL_GameController *controller,
                                  int force);

#ifdef __cplusplus
}
#endif

#ifdef NXINPUT_EVDEV_CHORD_IMPLEMENTATION

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <SDL.h>
#include <errno.h>
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

#ifndef NX_EXIT_CHORD_HOLD_POLLS
#define NX_EXIT_CHORD_HOLD_POLLS 3
#endif

/* Codes are kernel ABI: stable regardless of the header vintage. */
#define NX_EVC_BTN_MISC 0x100
#define NX_EVC_BTN_JOYSTICK 0x120
#define NX_EVC_BTN_BASE3 0x128
#define NX_EVC_BTN_BASE4 0x129
#define NX_EVC_BTN_SOUTH 0x130
#define NX_EVC_BTN_BASE2_SELECT 0x136
#define NX_EVC_BTN_BASE2_START 0x137
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
  const char *source; /* "raw-trigger-happy" | "raw-select-start" | ... */
  unsigned char down_select;
  unsigned char down_start;
  unsigned long keybits[NX_EVC_NLONGS];
  char name[80];
  unsigned short bustype, vendor, product, version;
} nx_evc_pad;

static nx_evc_pad g_nx_evc_pads[NX_EVDEV_CHORD_MAX_PADS];
static int g_nx_evc_count = -1;
static int g_nx_evc_fired;
static int g_nx_evc_sdl_bound;           /* 1: SDL rules, evdev muted */
static int g_nx_evc_sdl_hold;            /* consecutive positive SDL polls */
static int g_nx_evc_sdl_fired;
static struct _SDL_GameController *g_nx_evc_logged[NX_EVDEV_CHORD_MAX_PADS];
static int g_nx_evc_logged_count;

static int nx_evc_bit(const unsigned long *bits, int code) {
  if (code < 0 || code > NX_EVC_KEY_MAX)
    return 0;
  return (int)((bits[code / (8 * (int)sizeof(unsigned long))] >>
                (code % (8 * (int)sizeof(unsigned long)))) &
               1UL);
}

/* DIAGNOSTICS ONLY. Vanilla SDL2 Linux joystick: button index N is the N-th
 * set key bit, enumerating BTN_JOYSTICK..KEY_MAX first and then 0..BTN_JOYSTICK-1. */
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

/* DIAGNOSTICS ONLY. SDL2 with the RetroArch-udev order patch (sdl2_input_as_retroarch_udev):
 * button index N is the N-th set key bit walking 0..KEY_MAX ascending. */
static int nx_evc_code_for_sdl_index_ascending(const unsigned long *bits,
                                               int index) {
  int code;
  int n = 0;
  if (index < 0)
    return -1;
  for (code = 0; code <= NX_EVC_KEY_MAX; ++code)
    if (nx_evc_bit(bits, code)) {
      if (n == index)
        return code;
      ++n;
    }
  return -1;
}

static int nx_evc_gpio_dense_table(const unsigned long *bits) {
  int code;
  for (code = 0x130; code <= 0x13c; ++code)
    if (!nx_evc_bit(bits, code))
      return 0;
  return 1;
}

static void nx_evc_apply_raw_fallback(nx_evc_pad *pad) {
  if (nx_evc_bit(pad->keybits, NX_EVC_BTN_TRIGGER_HAPPY1) &&
      nx_evc_bit(pad->keybits, NX_EVC_BTN_TRIGGER_HAPPY2)) {
    pad->code_select = NX_EVC_BTN_TRIGGER_HAPPY1;
    pad->code_start = NX_EVC_BTN_TRIGGER_HAPPY2;
    pad->source = "raw-trigger-happy";
  } else if (nx_evc_gpio_dense_table(pad->keybits)) {
    /* Assinatura da tabela vendor gpio-keys: TODOS os 13 codigos 0x130..0x13c
     * presentes e contiguos. So nela 0x136/0x137 sao select/start e os codigos
     * oficiais 0x13a/0x13b sao L2/R2 fisicos (fechar com eles = L2+R2). Um pad
     * com furos na faixa (0x13a/0x13b oficiais de verdade; 0x136/0x137 = L1/R1)
     * cai no par oficial abaixo. */
    pad->code_select = NX_EVC_BTN_BASE2_SELECT;
    pad->code_start = NX_EVC_BTN_BASE2_START;
    pad->source = "raw-gpio-base-pair";
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

/* Full key list of one device: "0x72 0x73 0x130 ... 0x13c" (max 96 codes). */
static void nx_evc_format_keys(const unsigned long *bits, char *out,
                               size_t cap) {
  int code, n = 0;
  size_t used = 0;
  out[0] = 0;
  for (code = 0; code <= NX_EVC_KEY_MAX && used + 8 < cap; ++code) {
    if (!nx_evc_bit(bits, code))
      continue;
    if (++n > 96) {
      snprintf(out + used, cap - used, " ...");
      break;
    }
    used += (size_t)snprintf(out + used, cap - used, "%s0x%x", used ? " " : "",
                             code);
  }
}

/* Onda v2 (0.4.5): abrir UM node candidato. Fatorado para o rescan de
 * hotplug reutilizar -- antes a varredura acontecia uma unica vez no boot e
 * um pad plugado depois nunca entrava (SELECT+START morto se o mapping SDL
 * nao tivesse BACK/START). */
static int nx_evc_try_open_index(int index) {
  char path[64];
  char keys[640];
  struct input_id id;
  nx_evc_pad *pad;
  int fd;
  int i;
  if (g_nx_evc_count >= NX_EVDEV_CHORD_MAX_PADS)
    return 0;
  for (i = 0; i < g_nx_evc_count; ++i)
    if (g_nx_evc_pads[i].node == index)
      return 0; /* ja' aberto */
  pad = &g_nx_evc_pads[g_nx_evc_count];
  snprintf(path, sizeof path, "/dev/input/event%d", index);
  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return 0;
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  memset(pad, 0, sizeof *pad);
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof pad->keybits), pad->keybits) < 0 ||
      !nx_evc_is_gamepad(pad->keybits)) {
    close(fd);
    return 0;
  }
  pad->fd = fd;
  pad->node = index;
  strcpy(pad->name, "?");
  (void)ioctl(fd, EVIOCGNAME(sizeof pad->name - 1), pad->name);
  memset(&id, 0, sizeof id);
  if (ioctl(fd, EVIOCGID, &id) == 0) {
    pad->bustype = id.bustype;
    pad->vendor = id.vendor;
    pad->product = id.product;
    pad->version = id.version;
  }
  nx_evc_apply_raw_fallback(pad);
  nx_evc_format_keys(pad->keybits, keys, sizeof keys);
  NXINPUT_EVDEV_CHORD_LOG(
      "EXIT chord evdev: %s (%s) bus=0x%x vid=0x%x pid=0x%x ver=0x%x "
      "fallback select=0x%x start=0x%x source=%s\n",
      path, pad->name, pad->bustype, pad->vendor, pad->product,
      pad->version, pad->code_select, pad->code_start, pad->source);
  NXINPUT_EVDEV_CHORD_LOG("EXIT chord evdev: %s keys: %s\n", path, keys);
  g_nx_evc_count++;
  return 1;
}

void nx_evdev_chord_open(void) {
  int index;
  if (g_nx_evc_count >= 0)
    return;
  g_nx_evc_count = 0;
  g_nx_evc_fired = 0;
  for (index = 0; index < 32; ++index)
    (void)nx_evc_try_open_index(index);
  if (!g_nx_evc_count)
    NXINPUT_EVDEV_CHORD_LOG("EXIT chord evdev: no readable gamepad; "
                            "SELECT+START depends on the SDL mapping only\n");
}

/* 0.4.1: o pad SDL so pode assumir o chord se o mapping REALMENTE liga BACK e
 * START (bind != NONE nos dois). Caso de campo: um frontend exporta um mapping
 * com crc mas SEM back/start -> o caminho SDL nunca dispararia e o fallback
 * evdev ficava mutado = SELECT+START morto. Com bind incompleto o fallback
 * evdev PERMANECE ativo (a decisao continua por CAPACIDADE, nunca por nome). */
static int nx_evc_sdl_can_chord(struct _SDL_GameController *c) {
  SDL_GameControllerButtonBind bb, sb;
  if (!c)
    return 0;
  bb = SDL_GameControllerGetBindForButton(c, SDL_CONTROLLER_BUTTON_BACK);
  sb = SDL_GameControllerGetBindForButton(c, SDL_CONTROLLER_BUTTON_START);
  return bb.bindType != SDL_CONTROLLER_BINDTYPE_NONE &&
         sb.bindType != SDL_CONTROLLER_BINDTYPE_NONE;
}

void nx_evdev_chord_bind_sdl(struct _SDL_GameController *controller) {
  int bound = controller != NULL && nx_evc_sdl_can_chord(controller);

  nx_evdev_chord_set_primary_active(bound);
  if (controller)
    nx_exit_chord_log_controller(controller, 0);
}

void nx_evdev_chord_set_primary_active(int active) {
  int bound = active ? 1 : 0;

  if (g_nx_evc_count < 0)
    nx_evdev_chord_open();
  if (bound != g_nx_evc_sdl_bound) {
    int i;
    g_nx_evc_sdl_bound = bound;
    g_nx_evc_fired = 0;
    for (i = 0; i < g_nx_evc_count; ++i) {
      g_nx_evc_pads[i].down_select = 0;
      g_nx_evc_pads[i].down_start = 0;
    }
    NXINPUT_EVDEV_CHORD_LOG(
        bound ? "EXIT chord: SDL pad binds BACK+START -> SDL state assumes; "
                "evdev fallback muted\n"
              : "EXIT chord: SDL pad absent or mapping lacks BACK/START -> "
                "evdev raw fallback ACTIVE (%d device(s))\n",
        g_nx_evc_count);
  }
}

int nx_evdev_chord_poll(void) {
  static unsigned nx_evc_poll_tick;
  struct input_event ev;
  int i;
  int any_down = 0;
  /* Onda v2 (0.4.5): hotplug. Um pad plugado depois do boot entra na lista
   * (rescan barato, ~1x/10s a 60 Hz); um pad ARRANCADO com botao apertado
   * sai da lista com o estado LIMPO -- antes o read() devolvia -1 igual ao
   * fila-vazia, down_select/down_start ficavam presos em 1 e o proximo poll
   * disparava uma saida espuria. */
  ++nx_evc_poll_tick;
  if ((nx_evc_poll_tick % 600u) == 0u &&
      g_nx_evc_count >= 0 && g_nx_evc_count < NX_EVDEV_CHORD_MAX_PADS) {
    for (i = 0; i < 32; ++i)
      (void)nx_evc_try_open_index(i);
  }
  if (g_nx_evc_count <= 0)
    return 0;
  for (i = 0; i < g_nx_evc_count; ++i) {
    nx_evc_pad *pad = &g_nx_evc_pads[i];
    ssize_t rc;
    /* Always drain (never let the queue grow), but only track when unbound. */
    for (;;) {
      rc = read(pad->fd, &ev, sizeof ev);
      if (rc != (ssize_t)sizeof ev)
        break;
      {
        unsigned char down;
        if (g_nx_evc_sdl_bound || ev.type != EV_KEY)
          continue;
        down = ev.value != 0; /* 1 press, 2 autorepeat */
        if ((int)ev.code == pad->code_select)
          pad->down_select = down;
        else if ((int)ev.code == pad->code_start)
          pad->down_start = down;
      }
    }
    if (rc < 0 && errno == ENODEV) {
      /* ENODEV e' o errno real de um pad ARRANCADO (kernel evdev); EBADF e
       * afins ficam de fora -- aparecem em harness com fd sintetico e nunca
       * significam hotplug. */
      NXINPUT_EVDEV_CHORD_LOG(
          "EXIT chord evdev: /dev/input/event%d (%s) removido (ENODEV); "
          "estado limpo\n", pad->node, pad->name);
      close(pad->fd);
      g_nx_evc_pads[i] = g_nx_evc_pads[g_nx_evc_count - 1];
      g_nx_evc_count--;
      i--;
      continue;
    }
    if (pad->down_select && pad->down_start)
      any_down = 1;
  }
  if (g_nx_evc_sdl_bound)
    return 0;
  if (any_down && !g_nx_evc_fired) {
    g_nx_evc_fired = 1;
    NXINPUT_EVDEV_CHORD_LOG("EXIT chord: fired by evdev fallback\n");
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
  g_nx_evc_sdl_bound = 0;
  g_nx_evc_sdl_hold = 0;
  g_nx_evc_sdl_fired = 0;
  g_nx_evc_logged_count = 0;
}

static int nx_evc_pad_chord_down(struct _SDL_GameController *c) {
  SDL_GameControllerButtonBind bb, sb;
  SDL_Joystick *j;
  if (!c)
    return 0;
  if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))
    return 1;
  /* Raw joystick at the mapping's own indices: same physical keys, no
   * dependency on the GameController dispatch. Never uses literal codes. */
  j = SDL_GameControllerGetJoystick(c);
  bb = SDL_GameControllerGetBindForButton(c, SDL_CONTROLLER_BUTTON_BACK);
  sb = SDL_GameControllerGetBindForButton(c, SDL_CONTROLLER_BUTTON_START);
  if (j && bb.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
      sb.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
      bb.value.button != sb.value.button &&
      SDL_JoystickGetButton(j, bb.value.button) &&
      SDL_JoystickGetButton(j, sb.value.button))
    return 1;
  return 0;
}

int nx_exit_chord_poll_sdl(struct _SDL_GameController *const *pads,
                           int count) {
  int i, down = 0;
  for (i = 0; i < count && pads; ++i)
    if (nx_evc_pad_chord_down(pads[i])) {
      down = 1;
      break;
    }
  if (!down) {
    g_nx_evc_sdl_hold = 0;
    g_nx_evc_sdl_fired = 0;
    return 0;
  }
  if (g_nx_evc_sdl_hold < NX_EXIT_CHORD_HOLD_POLLS)
    g_nx_evc_sdl_hold++;
  if (g_nx_evc_sdl_hold >= NX_EXIT_CHORD_HOLD_POLLS && !g_nx_evc_sdl_fired) {
    g_nx_evc_sdl_fired = 1;
    NXINPUT_EVDEV_CHORD_LOG("EXIT chord: fired by SDL BACK+START\n");
    return 1;
  }
  return 0;
}

int nx_exit_chord_update(struct _SDL_GameController *const *pads, int count) {
  int i;
  struct _SDL_GameController *first = NULL;
  for (i = 0; i < count && pads; ++i)
    if (pads[i]) {
      first = pads[i];
      break;
    }
  nx_evdev_chord_bind_sdl(first);
  if (nx_exit_chord_poll_sdl(pads, count))
    return 1;
  return nx_evdev_chord_poll();
}

static void nx_evc_log_bind(struct _SDL_GameController *c, const char *label,
                            SDL_GameControllerButtonBind b, char *out,
                            size_t cap) {
  (void)c;
  if (b.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON)
    snprintf(out, cap, "%s=b%d", label, b.value.button);
  else if (b.bindType == SDL_CONTROLLER_BINDTYPE_AXIS)
    snprintf(out, cap, "%s=a%d", label, b.value.axis);
  else if (b.bindType == SDL_CONTROLLER_BINDTYPE_HAT)
    snprintf(out, cap, "%s=h%d.%d", label, b.value.hat.hat,
             b.value.hat.hat_mask);
  else
    snprintf(out, cap, "%s=none", label);
}

void nx_exit_chord_log_controller(struct _SDL_GameController *c, int force) {
  SDL_Joystick *j;
  char guid[64];
  char *mapping;
  char bback[24], bstart[24], blt[24], brt[24], bguide[24];
  SDL_GameControllerButtonBind back, start;
  int i;
  if (!c)
    return;
  if (!force) {
    for (i = 0; i < g_nx_evc_logged_count; ++i)
      if (g_nx_evc_logged[i] == c)
        return;
    if (g_nx_evc_logged_count < NX_EVDEV_CHORD_MAX_PADS)
      g_nx_evc_logged[g_nx_evc_logged_count++] = c;
  }
  j = SDL_GameControllerGetJoystick(c);
  guid[0] = 0;
  if (j)
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof guid);
  mapping = SDL_GameControllerMapping(c);
  back = SDL_GameControllerGetBindForButton(c, SDL_CONTROLLER_BUTTON_BACK);
  start = SDL_GameControllerGetBindForButton(c, SDL_CONTROLLER_BUTTON_START);
  nx_evc_log_bind(c, "back", back, bback, sizeof bback);
  nx_evc_log_bind(c, "start", start, bstart, sizeof bstart);
  nx_evc_log_bind(c, "lefttrigger",
                  SDL_GameControllerGetBindForAxis(
                      c, SDL_CONTROLLER_AXIS_TRIGGERLEFT),
                  blt, sizeof blt);
  nx_evc_log_bind(c, "righttrigger",
                  SDL_GameControllerGetBindForAxis(
                      c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT),
                  brt, sizeof brt);
  nx_evc_log_bind(c, "guide",
                  SDL_GameControllerGetBindForButton(
                      c, SDL_CONTROLLER_BUTTON_GUIDE),
                  bguide, sizeof bguide);
  NXINPUT_EVDEV_CHORD_LOG(
      "EXIT chord SDL: pad '%s' guid=%s buttons=%d axes=%d hats=%d | %s %s %s "
      "%s %s\n",
      SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?", guid,
      j ? SDL_JoystickNumButtons(j) : -1, j ? SDL_JoystickNumAxes(j) : -1,
      j ? SDL_JoystickNumHats(j) : -1, bback, bstart, blt, brt, bguide);
  NXINPUT_EVDEV_CHORD_LOG("EXIT chord SDL: mapping=%s\n",
                          mapping ? mapping : "(none)");
  if (mapping)
    SDL_free(mapping);
  /* Both index->code tables, so the log tells which SDL flavour the CFW runs
   * (vanilla vs ascending) without touching the device. */
  if (back.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
      start.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) {
    for (i = 0; i < g_nx_evc_count; ++i) {
      nx_evc_pad *pad = &g_nx_evc_pads[i];
      NXINPUT_EVDEV_CHORD_LOG(
          "EXIT chord SDL: event%d (%s) if vanilla-SDL: back=b%d->0x%x "
          "start=b%d->0x%x | if ascending-SDL (retroarch-udev patch): "
          "back->0x%x start->0x%x (diagnostic only; chord uses SDL state)\n",
          pad->node, pad->name, back.value.button,
          nx_evc_code_for_sdl_index(pad->keybits, back.value.button),
          start.value.button,
          nx_evc_code_for_sdl_index(pad->keybits, start.value.button),
          nx_evc_code_for_sdl_index_ascending(pad->keybits, back.value.button),
          nx_evc_code_for_sdl_index_ascending(pad->keybits,
                                              start.value.button));
    }
  } else {
    NXINPUT_EVDEV_CHORD_LOG("EXIT chord SDL: mapping has no button bind for "
                            "BACK/START; only the GameController state and the "
                            "evdev fallback (when unbound) can fire\n");
  }
}

#endif /* NXINPUT_EVDEV_CHORD_IMPLEMENTATION */
#endif /* NXINPUT_EVDEV_CHORD_H */
