#!/usr/bin/env python3
"""Generate src/cursor_arrow.h: a classic anti-aliased arrow cursor.

Outputs, from one master polygon:
  - summertime_arrow_tex[]: 64x64 premultiplied RGBA8 texture (white fill,
    black outline, soft drop shadow) drawn by the EGL overlay as a textured
    quad.  Hover recolor happens in the shader via a tint uniform.
  - summertime_fb1_arrow / summertime_fb1_arrow_hover: 32x32 RGB565 sprites
    for the Amlogic OSD2 hardware cursor (magenta 0xF81F = color key).

Pure Python (no PIL): polygon coverage + distance-to-edge supersampling.
Run from the port root: python3 tools/gen_cursor_arrow.py [--preview]
"""

import math
import os
import sys

# Master arrow polygon, y grows downward, tip at (0, 0), height 20 units.
POLY = [
    (0.0, 0.0),    # tip
    (0.0, 15.5),   # left edge
    (3.6, 12.6),   # notch left (tail starts)
    (6.2, 19.2),   # tail bottom-left
    (9.4, 17.9),   # tail bottom-right
    (6.8, 11.6),   # notch right
    (11.0, 11.6),  # wing
]
DESIGN_H = 20.0


def point_in_poly(px, py, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > py) != (yj > py):
            if px < (xj - xi) * (py - yi) / (yj - yi) + xi:
                inside = not inside
        j = i
    return inside


def dist_to_edges(px, py, poly):
    best = 1e9
    j = len(poly) - 1
    for i in range(len(poly)):
        x1, y1 = poly[j]
        x2, y2 = poly[i]
        dx, dy = x2 - x1, y2 - y1
        ln2 = dx * dx + dy * dy
        t = 0.0 if ln2 == 0 else max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / ln2))
        ex, ey = x1 + t * dx, y1 + t * dy
        d = math.hypot(px - ex, py - ey)
        if d < best:
            best = d
        j = i
    return best


def render_arrow(size, arrow_h_px, border_px, pad, ss, shadow=None):
    """Return size x size list of (fill_cov, border_cov, shadow_alpha) floats.

    fill_cov/border_cov partition arrow coverage; shadow_alpha is feathered.
    shadow = (offset_x, offset_y, strength, feather_px) or None.
    """
    scale = arrow_h_px / DESIGN_H
    poly = [(x * scale + pad, y * scale + pad) for x, y in POLY]
    out = []
    step = 1.0 / ss
    half = step / 2.0
    for y in range(size):
        row = []
        for x in range(size):
            cov = 0.0
            brd = 0.0
            sha = 0.0
            for sy in range(ss):
                py = y + half + sy * step
                for sx in range(ss):
                    px = x + half + sx * step
                    if point_in_poly(px, py, poly):
                        if dist_to_edges(px, py, poly) < border_px:
                            brd += 1.0
                        else:
                            cov += 1.0
                    if shadow is not None:
                        ox, oy, strength, feather = shadow
                        qx, qy = px - ox, py - oy
                        if point_in_poly(qx, qy, poly):
                            sha += strength
                        else:
                            d = dist_to_edges(qx, qy, poly)
                            if d < feather:
                                sha += strength * (1.0 - d / feather)
            n = float(ss * ss)
            row.append((cov / n, brd / n, sha / n))
        out.append(row)
    return out


def build_texture(size=64, arrow_h=52.0, border=2.4, pad=2.0):
    grid = render_arrow(size, arrow_h, border, pad, ss=6,
                        shadow=(2.2, 3.0, 0.38, 2.6))
    data = bytearray()
    for row in grid:
        for fill, brd, sha in row:
            arrow_a = min(1.0, fill + brd)
            # white fill + black border, premultiplied; shadow underneath
            a = arrow_a + sha * (1.0 - arrow_a)
            r = g = b = fill  # premultiplied white; border/shadow stay black
            data += bytes((int(round(r * 255)), int(round(g * 255)),
                           int(round(b * 255)), int(round(a * 255))))
    return data


def build_fb1(size=32, arrow_h=28.0, border=1.4, pad=1.0, hover=False):
    grid = render_arrow(size, arrow_h, border, pad, ss=6, shadow=None)
    key = 0xF81F
    fill_col = 0xF800 if hover else 0xFFFF  # red / white in RGB565
    line_col = 0x0000
    px = []
    for row in grid:
        for fill, brd, _ in row:
            if fill + brd < 0.5:
                px.append(key)
            elif brd > fill:
                px.append(line_col)
            else:
                px.append(fill_col)
    return px


def preview_tex(data, size):
    for y in range(size):
        line = []
        for x in range(size):
            r, g, b, a = data[(y * size + x) * 4:(y * size + x) * 4 + 4]
            if a < 30:
                line.append(" ")
            elif r > 140:
                line.append("#")   # white fill
            elif a > 140:
                line.append("@")   # border/strong shadow
            else:
                line.append(".")   # soft shadow
        print("".join(line).rstrip())


def preview_fb1(px, size):
    sym = {0xF81F: " ", 0x0000: "@"}
    for y in range(size):
        print("".join(sym.get(px[y * size + x], "#") for x in range(size)).rstrip())


def emit_c_bytes(data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("  " + ",".join(str(v) for v in data[i:i + per_line]) + ",")
    return "\n".join(lines)


def emit_c_u16(vals, per_line=12):
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append("  " + ",".join("0x%04x" % v for v in vals[i:i + per_line]) + ",")
    return "\n".join(lines)


def main():
    tex_size, arrow_h, pad = 64, 52.0, 2.0
    tex = build_texture(tex_size, arrow_h, border=2.4, pad=pad)
    fb1_n = build_fb1(hover=False)
    fb1_h = build_fb1(hover=True)

    if "--preview" in sys.argv:
        print("=== EGL texture %dx%d ===" % (tex_size, tex_size))
        preview_tex(tex, tex_size)
        print("=== fb1 32x32 ===")
        preview_fb1(fb1_n, 32)
        return

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       os.pardir, "src", "cursor_arrow.h")
    with open(out, "w") as f:
        f.write("/* Generated by tools/gen_cursor_arrow.py -- do not edit.\n"
                " * Classic arrow cursor: EGL RGBA texture (premultiplied) +\n"
                " * OSD2/fb1 RGB565 sprites (0xF81F magenta = color key). */\n"
                "#ifndef SUMMERTIME_CURSOR_ARROW_H\n"
                "#define SUMMERTIME_CURSOR_ARROW_H\n\n")
        f.write("#define SS_ARROW_TEX_SIZE %d\n" % tex_size)
        f.write("#define SS_ARROW_TIP_X %.1ff\n" % pad)
        f.write("#define SS_ARROW_TIP_Y %.1ff\n" % pad)
        f.write("#define SS_ARROW_DESIGN_H %.1ff\n\n" % arrow_h)
        f.write("static const unsigned char summertime_arrow_tex"
                "[SS_ARROW_TEX_SIZE * SS_ARROW_TEX_SIZE * 4] = {\n")
        f.write(emit_c_bytes(tex))
        f.write("\n};\n\n")
        f.write("static const unsigned short summertime_fb1_arrow[32 * 32] = {\n")
        f.write(emit_c_u16(fb1_n))
        f.write("\n};\n\n")
        f.write("static const unsigned short summertime_fb1_arrow_hover[32 * 32] = {\n")
        f.write(emit_c_u16(fb1_h))
        f.write("\n};\n\n#endif\n")
    print("wrote", os.path.normpath(out), len(tex), "tex bytes")


if __name__ == "__main__":
    main()
