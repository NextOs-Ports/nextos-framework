"""Grab the framebuffer half the panel is actually showing.

/dev/fb0 here is 1280x1440: two 1280x720 buffers, and `pan` says which one is
on screen.  Reading from offset zero gives whichever buffer the game is
currently drawing into -- the one nobody is looking at -- which is how a
capture invents both "black screen" and "nothing moved".
"""
import os, sys

W, H, BPP = 1280, 720, 4
FRAME = W * H * BPP
out = sys.argv[1]

yoffset = 0
try:
    with open("/sys/class/graphics/fb0/pan") as f:
        yoffset = int(f.read().strip().split(",")[1])
except Exception:
    pass
start = yoffset * W * BPP

buf = bytearray()
fd = os.open("/dev/fb0", os.O_RDONLY)
while len(buf) < FRAME:
    os.lseek(fd, start + len(buf), 0)
    chunk = os.read(fd, min(1 << 20, FRAME - len(buf)))
    if not chunk:
        break
    buf += chunk
os.close(fd)
open(out, "wb").write(bytes(buf))
print("pan yoffset=%d, %d bytes" % (yoffset, len(buf)))
