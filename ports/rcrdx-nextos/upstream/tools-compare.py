#!/usr/bin/env python3
"""Compara quadros capturados do device e diz o que MUDOU entre eles.

Existe porque "a tela mudou" não prova nada sozinho: o cenário deste jogo tem
folhagem animada e o brilho médio quase não se mexe. O que separa "o personagem
andou" de "o vento balançou a árvore" é ONDE a diferença está e o quanto ela
pesa — por isso a comparação sai por FAIXA da tela, e não num número só.
"""
import os
import struct
import sys
import zlib

W, H = 1280, 720


def carrega(fn):
    d = open(fn, "rb").read()
    if len(d) < W * H * 4:
        return None
    return d[: W * H * 4]


def png(path, f):
    rgb = bytearray(W * H * 3)
    for i in range(W * H):
        b, g, r, _ = f[i * 4 : i * 4 + 4]
        rgb[i * 3] = r
        rgb[i * 3 + 1] = g
        rgb[i * 3 + 2] = b
    raw = b"".join(b"\x00" + bytes(rgb[y * W * 3 : (y + 1) * W * 3]) for y in range(H))

    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + ch(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
        + ch(b"IDAT", zlib.compress(bytes(raw), 6))
        + ch(b"IEND", b"")
    )


def brilho(f):
    tot = n = 0
    for i in range(0, W * H, 41):
        j = i * 4
        tot += f[j] + f[j + 1] + f[j + 2]
        n += 1
    return tot / n / 3


def diferenca_por_faixa(a, b, faixas=6):
    """Fração de pixels que mudaram de forma relevante, por faixa horizontal."""
    out = []
    alt = H // faixas
    for k in range(faixas):
        mudou = n = 0
        for y in range(k * alt, (k + 1) * alt, 3):
            for x in range(0, W, 3):
                j = (y * W + x) * 4
                d = abs(a[j] - b[j]) + abs(a[j + 1] - b[j + 1]) + abs(a[j + 2] - b[j + 2])
                if d > 60:  # acima do ruído de compressão/animação sutil
                    mudou += 1
                n += 1
        out.append(round(100.0 * mudou / n, 1))
    return out


def main():
    nomes = sys.argv[1:]
    quadros = {}
    for nome in nomes:
        fn = f"shots/fb_{nome}.raw"
        if not os.path.exists(fn):
            print(f"{nome}: AUSENTE")
            continue
        f = carrega(fn)
        if f is None:
            print(f"{nome}: incompleto ({os.path.getsize(fn)} bytes)")
            continue
        quadros[nome] = f
        png(f"shots/cmp_{nome}.png", f)
        print(f"{nome}: ok  brilho={brilho(f):.1f}")

    ordem = [n for n in nomes if n in quadros]
    for i in range(len(ordem) - 1):
        a, b = ordem[i], ordem[i + 1]
        faixas = diferenca_por_faixa(quadros[a], quadros[b])
        pico = max(faixas)
        print(f"\n{a} -> {b}")
        print(f"  mudanca por faixa (%): {faixas}")
        print(f"  pico={pico}%  ", end="")
        if pico >= 8:
            print("MUDANCA FORTE (cena diferente / personagem se deslocou)")
        elif pico >= 2:
            print("mudanca moderada")
        else:
            print("praticamente igual")


if __name__ == "__main__":
    main()
