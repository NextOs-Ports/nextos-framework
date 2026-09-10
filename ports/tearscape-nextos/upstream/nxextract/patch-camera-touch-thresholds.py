#!/usr/bin/env python3
"""Neutraliza o desvio nao-touch do Camera::TrackTarget no Tearscape.dll.

No celular do dono (_isTouchMode=true) os limiares de transicao de sala sao
proporcionais ao viewport e provadamente estaveis: a descida dispara no fundo
real da tela e o salto de 8/9 da altura devolve o personagem exatamente na
borda superior permitida (vpSize.Y/9). No modo nao-touch o limiar de descida
vira `vpSize.Y - worldSize.Y` misturando pixels de tela com unidades de mundo;
a conta so e estavel com viewport de pelo menos 1296 px de altura. Em qualquer
handheld (viewport 640x360 apos o override) o limiar cai para cerca de 216 px:
a descida dispara com o personagem a ~61% da sala, o salto o deixa com Y de
tela negativo e
a transicao de subida dispara de volta — sobe/desce infinito sempre que o
personagem para no terco de baixo da sala.

O patch troca 6 bytes de IL dentro de TrackTarget:

    ldarg.0; ldfld bool Camera::_isTouchMode   ->   ldc.i4.1; nop x5

fazendo o brtrue.s seguinte tomar sempre o caminho touch (limiares
proporcionais). Nada mais muda: a grade de salas (GetViewRectForPosition),
o TrackVisitedRoom e a UI de toque continuam decididos pelo _isTouchMode real.

A ancora e por capacidade, nao por versao: a sequencia
`ldc.r4 9; div; stloc.3; ldarg.0; ldfld <tok>; brtrue.s +0x17` existe
exatamente uma vez nos dois builds documentados do jogo. Se um build futuro
nao tiver a assinatura, o hook nao altera nada e registra o recibo —
o jogo continua instalavel, apenas sem a correcao da camera.
"""

import os
import re
import stat
import sys

SIGNATURE = re.compile(
    rb"\x22\x00\x00\x10\x41\x5B\x0D\x02\x7B....\x2D\x17", re.S
)
PATCHED = re.compile(
    rb"\x22\x00\x00\x10\x41\x5B\x0D\x17\x00\x00\x00\x00\x00\x2D\x17", re.S
)
PREFIX = 7          # bytes ate o ldarg.0 dentro da assinatura
REPLACEMENT = b"\x17\x00\x00\x00\x00\x00"  # ldc.i4.1 + nop x5
MIN_SIZE = 1_653_555  # piso do validate do extractor para o Tearscape.dll


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-camera-touch-thresholds.py TEARSCAPE_DLL")
    path = os.path.abspath(sys.argv[1])
    info = os.lstat(path)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise SystemExit("Tearscape.dll is linked or not a private file")
    with open(path, "rb") as stream:
        data = bytearray(stream.read())
    if len(data) < MIN_SIZE:
        raise SystemExit("Tearscape.dll is smaller than the recipe floor")
    if PATCHED.search(data):
        print("nx/camera: touch thresholds already applied (idempotent)")
        return 0
    matches = list(SIGNATURE.finditer(data))
    if len(matches) != 1:
        print(
            "nx/camera: SKIPPED — TrackTarget signature found %d times "
            "(expected 1); assembly layout not recognized, DLL untouched"
            % len(matches)
        )
        return 0
    start = matches[0].start() + PREFIX
    data[start : start + len(REPLACEMENT)] = REPLACEMENT
    if not PATCHED.search(data) or SIGNATURE.search(data):
        raise SystemExit("camera patch result failed its self-check")
    temporary = path + ".camera.nxpart"
    try:
        with open(temporary, "xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, stat.S_IMODE(info.st_mode))
        os.replace(temporary, path)
        directory = os.open(os.path.dirname(path), os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    print(
        "nx/camera: TrackTarget non-touch thresholds neutralized at 0x%x "
        "(proportional room transitions on every panel)" % start
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
