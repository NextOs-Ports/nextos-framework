#!/usr/bin/env python3
"""Pad virtual por uinput — FERRAMENTA DE TESTE, nao entra no pacote publico.

Cria um gamepad evdev real (o SDL o enxerga como GameController de verdade) e
executa uma sequencia de botoes. Serve para provar o caminho de controle no
aparelho sem a mao no console, e para disparar SELECT+START de verdade.

Uso: python3 virtual-pad.py "a:0.2,wait:1,dpad_right:0.15,select+start:0.6"
"""
import ctypes, fcntl, os, struct, sys, time

UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
BUS_USB = 0x03

# A ordem de declaracao vira o INDICE de botao no SDL, e o mapping Xbox 360 do
# gamecontrollerdb casa por indice: a,b,x,y,l,r,back,start,guide,l3,r3 e o
# direcional pelo HAT — declarar dpad como BTN_DPAD_* faria o SDL entrega-lo
# como l3/r3 (medido no R36S).
BTN = {
    "a": 0x130, "b": 0x131, "x": 0x133, "y": 0x134,
    "l": 0x136, "r": 0x137,
    "select": 0x13a, "start": 0x13b, "mode": 0x13c,
    "l3": 0x13d, "r3": 0x13e,
}
ABS_X, ABS_Y, ABS_RX, ABS_RY = 0x00, 0x01, 0x03, 0x04
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11
HAT = {
    "dpad_up": (ABS_HAT0Y, -1), "dpad_down": (ABS_HAT0Y, 1),
    "dpad_left": (ABS_HAT0X, -1), "dpad_right": (ABS_HAT0X, 1),
}


def _create():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    for code in BTN.values():
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for axis in (ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y):
        fcntl.ioctl(fd, UI_SET_ABSBIT, axis)

    # struct uinput_user_dev: char name[80]; input_id(4H); ff_effects_max;
    # then absmax/absmin/absfuzz/absflat[64] each __s32
    name = b"NextOS Virtual Pad".ljust(80, b"\0")
    dev = name + struct.pack("HHHH", BUS_USB, 0x045e, 0x028e, 0x0110)
    dev += struct.pack("i", 0)
    absmax = [0] * 64
    absmin = [0] * 64
    for axis in (ABS_X, ABS_Y, ABS_RX, ABS_RY):
        absmax[axis], absmin[axis] = 32767, -32768
    for axis in (ABS_HAT0X, ABS_HAT0Y):
        absmax[axis], absmin[axis] = 1, -1
    dev += struct.pack("64i", *absmax) + struct.pack("64i", *absmin)
    dev += struct.pack("64i", *([0] * 64)) + struct.pack("64i", *([0] * 64))
    os.write(fd, dev)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.5)  # deixa o SDL/udev enxergar o dispositivo
    return fd


def _run(fd, script):
    def emit(etype, code, value):
        os.write(fd, struct.pack("llHHi", 0, 0, etype, code, value))

    def sync():
        emit(EV_SYN, 0, 0)

    for step in script.split(","):
        step = step.strip()
        if not step:
            continue
        name_part, _, hold = step.partition(":")
        hold = float(hold) if hold else 0.15
        if name_part == "wait":
            time.sleep(hold)
            continue
        parts = name_part.split("+")
        for k in parts:
            if k in HAT:
                emit(EV_ABS, HAT[k][0], HAT[k][1])
            else:
                emit(EV_KEY, BTN[k], 1)
        sync()
        time.sleep(hold)
        for k in parts:
            if k in HAT:
                emit(EV_ABS, HAT[k][0], 0)
            else:
                emit(EV_KEY, BTN[k], 0)
        sync()
        time.sleep(0.12)
        print("pad: %s" % name_part, flush=True)


def daemon(fifo):
    """Mantem UM pad virtual vivo e executa comandos lidos do FIFO.

    Evita o churn de hotplug de criar/destruir o dispositivo a cada passo."""
    fd = _create()
    try:
        while True:
            with open(fifo, "r") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    if line == "quit":
                        return
                    _run(fd, line)
                    print("pad: %s" % line, flush=True)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


def main(script):
    fd = _create()
    _run(fd, script)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--daemon":
        daemon(sys.argv[2])
    else:
        main(sys.argv[1] if len(sys.argv) > 1 else "a:0.2")
