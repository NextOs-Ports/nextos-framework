#!/usr/bin/env python3
"""virtual_pad.py -- pad sintetico por uinput para provar SELECT+START sem maos.

Clona bus/vendor/product/version E o nome evdev do pad fisico do aparelho, para
o SDL calcular o MESMO GUID e aplicar o mapeamento que ja esta no
gamecontrollerdb. Sem isso o SDL enxerga um joystick sem mapa e o
SDL_GameController nunca aparece.

Uso: virtual_pad.py <atraso_s> <segurar_s>
"""
import ctypes, fcntl, os, struct, sys, time

UINPUT = "/dev/uinput"
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502

# Botoes do pad "Twin USB PS2 Adapter": o kernel expoe BTN_TRIGGER..BTN_DEAD
BTN_TRIGGER = 0x120
BUTTONS = list(range(BTN_TRIGGER, BTN_TRIGGER + 12))
ABS_AXES = [0x00, 0x01, 0x02, 0x05]  # X, Y, Z, RZ

NAME = b" USB Gamepad          "


def write_event(fd, etype, code, value):
    # struct input_event: timeval(2*8) + u16 + u16 + s32 (+pad)
    os.write(fd, struct.pack("@llHHi", 0, 0, etype, code, value))


def main():
    delay = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    hold = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
    fd = os.open(UINPUT, os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    for b in BUTTONS:
        fcntl.ioctl(fd, UI_SET_KEYBIT, b)
    for a in ABS_AXES:
        fcntl.ioctl(fd, UI_SET_ABSBIT, a)

    # struct uinput_user_dev: char name[80]; input_id id (4*u16);
    # ff_effects_max u32; absmax/absmin/absfuzz/absflat [64] s32 cada
    dev = bytearray(80 + 8 + 4 + 4 * 64 * 4)
    dev[0:len(NAME)] = NAME
    struct.pack_into("@HHHH", dev, 80, 0x0003, 0x0810, 0x0001, 0x0110)
    for a in ABS_AXES:
        struct.pack_into("@i", dev, 80 + 8 + 4 + 0 * 64 * 4 + a * 4, 255)  # absmax
        struct.pack_into("@i", dev, 80 + 8 + 4 + 1 * 64 * 4 + a * 4, 0)    # absmin
        struct.pack_into("@i", dev, 80 + 8 + 4 + 3 * 64 * 4 + a * 4, 15)   # absflat
    os.write(fd, bytes(dev))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    print("[vpad] criado; aguardando %.1fs" % delay, flush=True)

    # centralizar eixos para o SDL nao ler 0 como fundo de escala
    for a in ABS_AXES:
        write_event(fd, EV_ABS, a, 128)
    write_event(fd, EV_SYN, SYN_REPORT, 0)

    time.sleep(delay)
    # SELECT = botao 8 (b8 no evdev = BTN_TRIGGER+8), START = botao 9.
    select_code, start_code = BTN_TRIGGER + 8, BTN_TRIGGER + 9
    print("[vpad] SELECT+START", flush=True)
    write_event(fd, EV_KEY, select_code, 1)
    write_event(fd, EV_KEY, start_code, 1)
    write_event(fd, EV_SYN, SYN_REPORT, 0)
    time.sleep(hold)
    write_event(fd, EV_KEY, select_code, 0)
    write_event(fd, EV_KEY, start_code, 0)
    write_event(fd, EV_SYN, SYN_REPORT, 0)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
    print("[vpad] fim", flush=True)


main()
