#!/usr/bin/env python3
"""Converte os shot_*.raw do CHRONO_SHOTS (glReadPixels RGBA, bottom-up) em PNG.

FERRAMENTA DE TESTE — nao entra no pacote publico.
Uso: python3 shot2png.py LARGURA ALTURA entrada.raw saida.png
"""
import sys
from PIL import Image

w, h, src, dst = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
data = open(src, "rb").read()
need = w * h * 4
if len(data) < need:
    raise SystemExit("raw curto: %d < %d" % (len(data), need))
img = Image.frombytes("RGBA", (w, h), data[:need])
# glReadPixels devolve de baixo para cima.
img = img.transpose(Image.FLIP_TOP_BOTTOM).convert("RGB")
img.save(dst)
nonblack = sum(1 for p in img.getdata() if p != (0, 0, 0))
print("%s -> %s  %dx%d  pixels nao-pretos=%d (%.1f%%)"
      % (src, dst, w, h, nonblack, 100.0 * nonblack / (w * h)))
