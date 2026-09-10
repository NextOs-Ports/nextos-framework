/*
 * pad_ordinal_fix.h -- NextOS mapping for old ordinal gamepads.
 *
 * Old Amlogic kernels can expose a generic HID report as BTN_GAMEPAD+n in
 * report order instead of semantic BTN_SOUTH/EAST/... codes.  BTN_C/BTN_Z in
 * the evdev bitmap identify that layout.  Controller databases authored on a
 * modern kernel then assign the wrong physical buttons, notably the triggers.
 *
 * Register the mapping before SDL_IsGameController().  The mapping is copied
 * from the proven GTA SA NextOS port; the game-specific prefix only controls
 * the opt-out/manual override environment variables.
 */
#ifndef PAD_ORDINAL_FIX_H
#define PAD_ORDINAL_FIX_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <SDL2/SDL.h>

#define PAD_ORD_NBITS(x) (((x) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

static int pad_ord_test_bit(const unsigned long *b, int i) {
  return !!((b[i / (8 * sizeof(long))] >> (i % (8 * sizeof(long)))) & 1);
}

static int pad_ord_abs_rank(const unsigned long *absb, int code) {
  if (!pad_ord_test_bit(absb, code)) return -1;
  int r = 0;
  for (int i = 0; i < code; i++) {
    if (i >= ABS_HAT0X && i <= ABS_HAT3Y) continue;
    if (pad_ord_test_bit(absb, i)) r++;
  }
  return r;
}

static void pad_ordinal_fix_apply(int index, const char *env_prefix) {
  char envname[64];
  snprintf(envname, sizeof(envname), "%s_ORDINAL_FIX", env_prefix);
  const char *env = getenv(envname);
  if (env && (!strcmp(env, "0") || !strcasecmp(env, "off"))) return;
  snprintf(envname, sizeof(envname), "%s_PAD_MAP", env_prefix);
  const char *usermap = getenv(envname);
  if (usermap && *usermap) return;

  SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
  int vid = guid.data[4] | (guid.data[5] << 8);
  int pid = guid.data[8] | (guid.data[9] << 8);
  if (!vid && !pid) return;

  unsigned long keyb[PAD_ORD_NBITS(KEY_MAX + 1)];
  unsigned long absb[PAD_ORD_NBITS(ABS_MAX + 1)];
  int found = 0;
  for (int i = 0; i < 32 && !found; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    struct input_id id;
    memset(&id, 0, sizeof(id));
    memset(keyb, 0, sizeof(keyb));
    memset(absb, 0, sizeof(absb));
    if (ioctl(fd, EVIOCGID, &id) == 0 &&
        id.vendor == vid && id.product == pid &&
        /* Pads gpio-keys (BUS_HOST) expõem códigos SEMÂNTICOS reais — a
         * tabela vendor densa 0x130..0x13c tem BTN_C/BTN_Z como X/R1 físicos
         * e disparava este fix por engano, atropelando o mapping correto da
         * CFW (caso de campo: família H700 no Knulli, menu sem controle).
         * O fix é só para HID de kernel antigo (USB/Bluetooth). */
        id.bustype != BUS_HOST &&
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0 &&
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb) >= 0) {
      if (pad_ord_test_bit(keyb, BTN_GAMEPAD) &&
          (pad_ord_test_bit(keyb, BTN_C) || pad_ord_test_bit(keyb, BTN_Z))) {
        found = 1;
      } else if (vid == 0x0810 && pid == 0x0001 &&
                 pad_ord_test_bit(keyb, BTN_TRIGGER) &&
                 pad_ord_test_bit(keyb, BTN_BASE6) &&
                 !pad_ord_test_bit(keyb, BTN_GAMEPAD)) {
        /* Proven GTA 3/VC exception.  This adapter predates the BTN_GAMEPAD
         * range and exposes twelve buttons as BTN_TRIGGER..BTN_BASE6. */
        found = 2;
      }
    }
    close(fd);
  }
  if (!found) return;

  char gs[64];
  SDL_JoystickGetGUIDString(guid, gs, sizeof(gs));
  const char *nm = SDL_JoystickNameForIndex(index);
  char name[64];
  snprintf(name, sizeof(name), "%s", nm && *nm ? nm : "pad");
  for (char *c = name; *c; c++)
    if (*c == ',' || *c == ':') *c = ' ';

  int sony = (vid == 0x054c);
  const char *btns = found == 2
      ? "a:b2,b:b1,x:b3,y:b0,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,"
        "righttrigger:b7,back:b8,start:b9,leftstick:b10,rightstick:b11,"
      : sony
      ? "x:b0,a:b1,b:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,"
        "righttrigger:b7,back:b8,start:b9,leftstick:b10,rightstick:b11,guide:b12,"
      : "a:b0,b:b1,x:b3,y:b4,leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,"
        "righttrigger:b9,back:b10,start:b11,guide:b12,leftstick:b13,rightstick:b14,";

  int lx = pad_ord_abs_rank(absb, ABS_X);
  int ly = pad_ord_abs_rank(absb, ABS_Y);
  int rx = pad_ord_abs_rank(absb, ABS_Z);
  int ry = pad_ord_abs_rank(absb, ABS_RZ);
  if (rx < 0 || ry < 0) {
    rx = pad_ord_abs_rank(absb, ABS_RX);
    ry = pad_ord_abs_rank(absb, ABS_RY);
  }

  char map[512];
  int off = snprintf(map, sizeof(map), "%s,%s,platform:Linux,%s", gs, name, btns);
  if (found == 2) {
    off += snprintf(map + off, sizeof(map) - off,
                    "leftx:a0,lefty:a1,rightx:a3,righty:a2,");
  } else {
    if (lx >= 0) off += snprintf(map + off, sizeof(map) - off, "leftx:a%d,", lx);
    if (ly >= 0) off += snprintf(map + off, sizeof(map) - off, "lefty:a%d,", ly);
    if (rx >= 0) off += snprintf(map + off, sizeof(map) - off, "rightx:a%d,", rx);
    if (ry >= 0) off += snprintf(map + off, sizeof(map) - off, "righty:a%d,", ry);
  }
  if (pad_ord_test_bit(absb, ABS_HAT0X) && pad_ord_test_bit(absb, ABS_HAT0Y))
    snprintf(map + off, sizeof(map) - off,
             "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");
  int mr = SDL_GameControllerAddMapping(map);
  fprintf(stderr, "[pad] ordinal fix (%s) vid=%04x pid=%04x r=%d: %s\n",
          found == 2 ? "0810:0001 legacy joystick" : (sony ? "sony" : "hid"),
          vid, pid, mr, map);
}

#endif /* PAD_ORDINAL_FIX_H */
