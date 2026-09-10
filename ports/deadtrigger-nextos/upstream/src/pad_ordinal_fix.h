/*
 * pad_ordinal_fix.h — RECEITA NextOS "ordinal pad fix" (padroniza controles)
 *
 * Kernel antigo (3.14, Amlogic-old) sem driver HID especifico: hid-generic
 * mapeia botao HID n -> BTN_GAMEPAD+n-1 pela ORDEM do report. Assinatura no
 * evdev = BTN_C/BTN_Z presentes no bitmap EV_KEY (layout semantico moderno
 * nunca tem esses codigos). Mapeamentos vindos do gamecontrollerdb ou do
 * SDL_GAMECONTROLLERCONFIG do launcher foram autorados em kernel moderno ou
 * por LABEL fisico (pads com etiqueta estilo Nintendo) -> A/B e X/Y saem
 * trocados. Aqui detectamos a assinatura e registramos um mapping SDL pela
 * ordem FISICA da classe do pad (layout POSICIONAL Xbox, padrao dos ports):
 *   Sony (054c): Square,Cross,Circle,Triangle,L1,R1,L2,R2,Create,Options,L3,R3,PS
 *   demais (usage HID padrao): A,B,C,X,Y,Z,L1,R1,L2,R2,Select,Start,Mode,L3,R3
 * Eixos: indice SDL = rank do codigo ABS presente no evdev (hats excluidos).
 * Refs: deadcells/src/input.c + tabela DualSense confirmada no device.
 *
 * USO (apos os includes de SDL2):
 *   #include "pad_ordinal_fix.h"
 *   ...
 *   pad_ordinal_fix_apply(index, "GTACTW");   // ANTES de SDL_IsGameController()
 *   if (!SDL_IsGameController(index)) ...
 *
 * Env: <PREFIX>_ORDINAL_FIX=0|off desliga; <PREFIX>_PAD_MAP (mapping manual)
 * tem prioridade e suprime o fix.
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
        if (i >= ABS_HAT0X && i <= ABS_HAT3Y) continue;   /* hats nao viram eixo no SDL */
        if (pad_ord_test_bit(absb, i)) r++;
    }
    return r;
}

static int pad_ord_button_is(SDL_GameController *controller,
                             SDL_GameControllerButton button,
                             int raw_button) {
    SDL_GameControllerButtonBind binding =
        SDL_GameControllerGetBindForButton(controller, button);
    return binding.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
           binding.value.button == raw_button;
}

static int pad_ord_axis_is_button(SDL_GameController *controller,
                                  SDL_GameControllerAxis axis,
                                  int raw_button) {
    SDL_GameControllerButtonBind binding =
        SDL_GameControllerGetBindForAxis(controller, axis);
    return binding.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
           binding.value.button == raw_button;
}

/* The canonical PortMaster handheld topology is already positional and must
 * win over the old hid-generic ordinal heuristic. Match the whole capability
 * shape, never controller identity, so a partial or unrelated map still falls
 * through to the proven old-kernel repair below. */
static int pad_ord_has_complete_portmaster_layout(int index) {
    SDL_GameController *controller;
    SDL_Joystick *joystick;
    int complete;

    if (!SDL_IsGameController(index))
        return 0;
    controller = SDL_GameControllerOpen(index);
    if (!controller)
        return 0;
    joystick = SDL_GameControllerGetJoystick(controller);
    complete = joystick && SDL_JoystickNumButtons(joystick) >= 12 &&
        pad_ord_button_is(controller, SDL_CONTROLLER_BUTTON_A, 1) &&
        pad_ord_button_is(controller, SDL_CONTROLLER_BUTTON_B, 0) &&
        pad_ord_button_is(controller, SDL_CONTROLLER_BUTTON_X, 3) &&
        pad_ord_button_is(controller, SDL_CONTROLLER_BUTTON_Y, 2) &&
        pad_ord_button_is(
            controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 4) &&
        pad_ord_button_is(
            controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 5) &&
        pad_ord_button_is(controller, SDL_CONTROLLER_BUTTON_START, 6) &&
        pad_ord_button_is(controller, SDL_CONTROLLER_BUTTON_BACK, 7) &&
        pad_ord_button_is(
            controller, SDL_CONTROLLER_BUTTON_LEFTSTICK, 8) &&
        pad_ord_button_is(
            controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK, 9) &&
        pad_ord_axis_is_button(
            controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 10) &&
        pad_ord_axis_is_button(
            controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 11);
    SDL_GameControllerClose(controller);
    return complete;
}

static void pad_ordinal_fix_apply(int index, const char *env_prefix) {
    char envname[64];
    snprintf(envname, sizeof(envname), "%s_ORDINAL_FIX", env_prefix);
    const char *env = getenv(envname);
    if (env && (!strcmp(env, "0") || !strcasecmp(env, "off"))) return;
    snprintf(envname, sizeof(envname), "%s_PAD_MAP", env_prefix);
    const char *usermap = getenv(envname);
    if (usermap && *usermap) return;   /* mapping manual tem prioridade */
    if (pad_ord_has_complete_portmaster_layout(index)) {
        fprintf(stderr,
                "[pad] ordinal fix ignorado: mapping PortMaster completo "
                "confirmado por capacidades\n");
        return;
    }

    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    int vid = guid.data[4] | (guid.data[5] << 8);
    int pid = guid.data[8] | (guid.data[9] << 8);
    if (!vid && !pid) return;

    unsigned long keyb[PAD_ORD_NBITS(KEY_MAX + 1)], absb[PAD_ORD_NBITS(ABS_MAX + 1)];
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
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0 &&
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb) >= 0 &&
            pad_ord_test_bit(keyb, BTN_GAMEPAD) &&
            (pad_ord_test_bit(keyb, BTN_C) || pad_ord_test_bit(keyb, BTN_Z)))
            found = 1;
        close(fd);
    }
    if (!found) return;   /* layout semantico (kernel/driver ok) -> nao mexe */

    char gs[64];
    SDL_JoystickGetGUIDString(guid, gs, sizeof(gs));
    const char *nm = SDL_JoystickNameForIndex(index);
    char name[64];
    snprintf(name, sizeof(name), "%s", nm && *nm ? nm : "pad");
    for (char *c = name; *c; c++)
        if (*c == ',' || *c == ':') *c = ' ';

    int sony = (vid == 0x054c);
    const char *btns = sony
        ? "x:b0,a:b1,b:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,"
          "righttrigger:b7,back:b8,start:b9,leftstick:b10,rightstick:b11,guide:b12,"
        : "a:b0,b:b1,x:b3,y:b4,leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,"
          "righttrigger:b9,back:b10,start:b11,guide:b12,leftstick:b13,rightstick:b14,";

    int lx = pad_ord_abs_rank(absb, ABS_X), ly = pad_ord_abs_rank(absb, ABS_Y);
    int rx = pad_ord_abs_rank(absb, ABS_Z), ry = pad_ord_abs_rank(absb, ABS_RZ);
    if (rx < 0 || ry < 0) { rx = pad_ord_abs_rank(absb, ABS_RX); ry = pad_ord_abs_rank(absb, ABS_RY); }

    char map[512];
    int off = snprintf(map, sizeof(map), "%s,%s,platform:Linux,%s", gs, name, btns);
    if (lx >= 0) off += snprintf(map + off, sizeof(map) - off, "leftx:a%d,", lx);
    if (ly >= 0) off += snprintf(map + off, sizeof(map) - off, "lefty:a%d,", ly);
    if (rx >= 0) off += snprintf(map + off, sizeof(map) - off, "rightx:a%d,", rx);
    if (ry >= 0) off += snprintf(map + off, sizeof(map) - off, "righty:a%d,", ry);
    if (pad_ord_test_bit(absb, ABS_HAT0X) && pad_ord_test_bit(absb, ABS_HAT0Y))
        snprintf(map + off, sizeof(map) - off,
                 "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");
    int mr = SDL_GameControllerAddMapping(map);
    fprintf(stderr, "[pad] ordinal fix (%s) vid=%04x pid=%04x r=%d: %s\n",
            sony ? "sony" : "hid", vid, pid, mr, map);
}

#endif /* PAD_ORDINAL_FIX_H */
