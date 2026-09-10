#!/usr/bin/env python3
"""vpad.py -- pad sintetico por uinput, CLONANDO o pad fisico do aparelho.

Por que clonar: o SDL calcula o GUID do joystick a partir de bus/vendor/
product/version e do nome. Um pad inventado nao casa com nenhuma linha do
gamecontrollerdb, o SDL_GameController nunca aparece e o teste "prova" um
caminho que o jogador nunca vai usar. Aqui a identidade e as capacidades sao
lidas do pad real em tempo de execucao -- nada de VID/PID cravado, nada de
nome de CFW.

Uso:
    vpad.py <script>

<script> e' uma lista separada por virgula de passos:
    wait:<s>            espera
    tap:<BTN>[:<s>]     press+release (padrao 0.30s, ~18 quadros a 60 fps)
    down:<BTN>          press
    up:<BTN>            release
    hold:<BTN>:<s>      press, espera, release
    combo:<A>+<B>:<s>   press dos dois, espera, release dos dois
    axis:<ABS>:<v>      valor absoluto cru
    stick:<ABS>:<-1..1>[:<s>] eixo normalizado, volta ao centro depois

O botao e' o nome evdev (BTN_SOUTH, BTN_TRIGGER_HAPPY1, ...). Nada de mapear
por posicao: quem decide o significado e' o mapeamento do CFW/SDL.
"""
import fcntl, os, struct, sys, time
import evdev
from evdev import ecodes as e

# ATENCAO: nao usar evdev.UInput aqui.
#
# O evdev novo cria o dispositivo com o ioctl UI_DEV_SETUP, que so existe a
# partir do kernel 4.5. O aparelho .137 roda 4.4.189 e devolve EINVAL para
# QUALQUER UInput, ate o vazio. O caminho abaixo e' o legado (escrever a
# struct uinput_user_dev e chamar UI_DEV_CREATE), que funciona nos dois.
# O evdev continua sendo usado, mas so para LER o pad fisico.

UINPUT = "/dev/uinput"
UI_SET_EVBIT  = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
ABS_CNT = 64


class SyntheticPad(object):
    """Pad padrao para aparelho SEM controle fisico.

    O .105 e' uma caixa Amlogic: so tem gpio_keypad, cec_input e IR -- nenhum
    gamepad. Sem um controle estavel o MonoGame fica em
    "Found new controller"/"Detected controller disconnect" em laco e o jogo
    nao aceita nada no titulo. A identidade abaixo e' a do xpad padrao, que o
    SDL ja mapeia de fabrica no Linux; nao ha VID/PID de CFW nem de aparelho
    aqui, e' o pad generico de referencia.
    """
    name = "Microsoft X-Box 360 pad"

    class _Info(object):
        bustype, vendor, product, version = 0x0003, 0x045E, 0x028E, 0x0114
    info = _Info()

    def capabilities(self):
        from evdev import AbsInfo
        stick = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128,
                        resolution=0)
        trig = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
        hat = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)
        return {
            e.EV_KEY: [e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST,
                       e.BTN_TL, e.BTN_TR, e.BTN_SELECT, e.BTN_START,
                       e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
            e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick),
                       (e.ABS_RX, stick), (e.ABS_RY, stick),
                       (e.ABS_Z, trig), (e.ABS_RZ, trig),
                       (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
        }


def find_pad():
    """O pad e' o dispositivo que declara os botoes de gamepad."""
    best = None
    for path in evdev.list_devices():
        try:
            d = evdev.InputDevice(path)
        except OSError:
            continue
        keys = set(d.capabilities().get(e.EV_KEY, []))
        if e.BTN_SOUTH in keys or e.BTN_GAMEPAD in keys:
            score = len(keys) + (10 if e.EV_ABS in d.capabilities() else 0)
            if not best or score > best[0]:
                best = (score, d)
    if not best:
        print("[vpad] nenhum pad fisico: criando o pad padrao sintetico",
              flush=True)
        return SyntheticPad()
    return best[1]


def create_clone(phys):
    caps = phys.capabilities()
    keys = sorted(caps.get(e.EV_KEY, []))
    absx = caps.get(e.EV_ABS, [])

    fd = os.open(UINPUT, os.O_WRONLY | os.O_NONBLOCK)
    for ev in (e.EV_KEY, e.EV_ABS, e.EV_SYN):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for k in keys:
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    for code, _ in absx:
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)

    # struct uinput_user_dev:
    #   char name[80]; struct input_id id (4 x u16); u32 ff_effects_max;
    #   s32 absmax[64]; absmin[64]; absfuzz[64]; absflat[64]
    dev = bytearray(80 + 8 + 4 + 4 * ABS_CNT * 4)
    nm = phys.name.encode()[:79]
    dev[0:len(nm)] = nm
    i = phys.info
    struct.pack_into("@HHHH", dev, 80, i.bustype, i.vendor, i.product, i.version)
    base = 80 + 8 + 4
    for code, ai in absx:
        struct.pack_into("@i", dev, base + 0 * ABS_CNT * 4 + code * 4, ai.max)
        struct.pack_into("@i", dev, base + 1 * ABS_CNT * 4 + code * 4, ai.min)
        struct.pack_into("@i", dev, base + 2 * ABS_CNT * 4 + code * 4, ai.fuzz)
        struct.pack_into("@i", dev, base + 3 * ABS_CNT * 4 + code * 4, ai.flat)
    os.write(fd, bytes(dev))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd, {c: a for c, a in absx}


def emit(fd, etype, code, value):
    os.write(fd, struct.pack("@llHHi", 0, 0, etype, code, value))


def main():
    script = sys.argv[1] if len(sys.argv) > 1 else "wait:5"
    phys = find_pad()
    i = phys.info
    print("[vpad] clonando %r bus=%04x vid=%04x pid=%04x ver=%04x"
          % (phys.name, i.bustype, i.vendor, i.product, i.version), flush=True)

    fd, absinfo = create_clone(phys)
    time.sleep(1.0)   # deixa o SDL enumerar antes do primeiro evento

    # centraliza os eixos: o SDL leria 0 como fundo de escala num pad -1800..1800
    for code, ai in absinfo.items():
        emit(fd, e.EV_ABS, code, (ai.max + ai.min) // 2)
    emit(fd, e.EV_SYN, e.SYN_REPORT, 0)

    def key(name, value):
        code = getattr(e, name, None)
        if code is None:
            print("[vpad] botao desconhecido: %s" % name, flush=True); return
        emit(fd, e.EV_KEY, code, value)
        emit(fd, e.EV_SYN, e.SYN_REPORT, 0)

    def axis(name, raw):
        code = getattr(e, name, None)
        if code is None:
            print("[vpad] eixo desconhecido: %s" % name, flush=True); return
        emit(fd, e.EV_ABS, code, int(raw))
        emit(fd, e.EV_SYN, e.SYN_REPORT, 0)

    for step in script.split(","):
        step = step.strip()
        if not step:
            continue
        parts = step.split(":")
        op = parts[0]
        if op == "wait":
            time.sleep(float(parts[1]))
        elif op == "waitfile":
            # espera por MARCADOR (o roteiro de entrada avisa quando terminou),
            # com teto: nunca esperar sem produtor vivo
            target = parts[1]
            limit = float(parts[2]) if len(parts) > 2 else 900.0
            end = time.time() + limit
            print("[vpad] esperando %s" % target, flush=True)
            while not os.path.exists(target) and time.time() < end:
                time.sleep(1)
            print("[vpad] %s" % ("marcador visto" if os.path.exists(target)
                                 else "teto atingido"), flush=True)
        elif op == "tap":
            d = float(parts[2]) if len(parts) > 2 else 0.30
            print("[vpad] tap %s" % parts[1], flush=True)
            key(parts[1], 1); time.sleep(d); key(parts[1], 0); time.sleep(0.15)
        elif op == "down":
            print("[vpad] down %s" % parts[1], flush=True); key(parts[1], 1)
        elif op == "up":
            print("[vpad] up %s" % parts[1], flush=True); key(parts[1], 0)
        elif op == "hold":
            print("[vpad] hold %s %ss" % (parts[1], parts[2]), flush=True)
            key(parts[1], 1); time.sleep(float(parts[2])); key(parts[1], 0)
            time.sleep(0.15)
        elif op == "combo":
            names = parts[1].split("+")
            d = float(parts[2]) if len(parts) > 2 else 0.30
            print("[vpad] combo %s" % "+".join(names), flush=True)
            for n in names: key(n, 1)
            time.sleep(d)
            for n in names: key(n, 0)
        elif op == "axis":
            axis(parts[1], parts[2])
        elif op == "stick":
            name, v = parts[1], float(parts[2])
            d = float(parts[3]) if len(parts) > 3 else 0.6
            code = getattr(e, name, None)
            ai = absinfo.get(code)
            if ai is None:
                print("[vpad] sem AbsInfo p/ %s" % name, flush=True); continue
            mid = (ai.max + ai.min) / 2.0
            span = (ai.max - ai.min) / 2.0
            print("[vpad] stick %s %+.1f" % (name, v), flush=True)
            axis(name, mid + v * span); time.sleep(d)
            axis(name, mid); time.sleep(0.2)
        else:
            print("[vpad] passo ignorado: %s" % step, flush=True)

    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
    print("[vpad] fim", flush=True)


main()
