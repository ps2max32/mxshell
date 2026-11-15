#!/usr/bin/env python3
"""The covers of Media's three tiles (Photos, Videos, Music), drawn here:
graphite, a lamp-coloured mark in a soft light, the kind's name.

  mk-media-covers.py <font.ttf> <out dir>

Writes <out dir>/<kind>/cover.t8 (128 x 176) and coverhd.t8 (256 x 361),
mxshell's format (tools/mk-cover.py): a CLUT of 256 RGBA entries in the GS's
CSM1 order, alpha 0x80, then the indices.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

INK, SLAB, LAMP, WHITE = (8, 8, 8), (28, 31, 36), (233, 201, 143), (236, 234, 230)


def t8(im, path):
    im = im.convert("RGB").quantize(256, dither=Image.Dither.FLOYDSTEINBERG)
    pal = im.getpalette() + [0] * 768
    clut = bytearray(1024)
    for i in range(256):
        at = (i & ~0x18) | (i & 8) << 1 | (i & 0x10) >> 1
        clut[at * 4:at * 4 + 4] = bytes(pal[i * 3:i * 3 + 3]) + b"\x80"
    open(path, "wb").write(bytes(clut) + im.tobytes())


def mark(d, kind, cx, cy, s, c):
    """The kind's mark, s its size, centred on (cx, cy)."""
    w = max(2, round(s * 0.06))
    if kind == "photos":  # a frame, a hill and a sun
        x0, y0, x1, y1 = cx - s * 0.5, cy - s * 0.38, cx + s * 0.5, cy + s * 0.38
        d.rounded_rectangle((x0, y0, x1, y1), radius=s * 0.06, outline=c, width=w)
        d.polygon([(x0 + w, y1 - w), (cx - s * 0.12, cy - s * 0.06), (cx + s * 0.1, cy + s * 0.14),
                   (cx + s * 0.24, cy + s * 0.02), (x1 - w, y1 - w)], fill=c)
        r = s * 0.08
        d.ellipse((cx + s * 0.2 - r, cy - s * 0.2 - r, cx + s * 0.2 + r, cy - s * 0.2 + r), fill=c)
    elif kind == "videos":  # a play triangle in a ring
        r = s * 0.48
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=c, width=w)
        t = s * 0.24
        d.polygon([(cx - t * 0.7, cy - t), (cx - t * 0.7, cy + t), (cx + t * 1.05, cy)], fill=c)
    else:  # an eighth note
        hx, hy = cx - s * 0.12, cy + s * 0.3
        d.ellipse((hx - s * 0.17, hy - s * 0.12, hx + s * 0.17, hy + s * 0.12), fill=c)
        sx = hx + s * 0.13
        d.rectangle((sx, cy - s * 0.45, sx + w * 1.4, hy), fill=c)
        d.polygon([(sx, cy - s * 0.45), (sx + s * 0.3, cy - s * 0.18), (sx + s * 0.3, cy - s * 0.06),
                   (sx + w, cy - s * 0.28)], fill=c)


def cover(kind, W, H, font_path):
    k = 4  # drawn larger, then down: smooth edges
    im = Image.new("RGB", (W * k, H * k), INK)
    d = ImageDraw.Draw(im)
    for y in range(H * k):  # graphite, darker towards the floor
        f = y / (H * k)
        c = tuple(round(SLAB[i] * (1 - f) + INK[i] * f) for i in range(3))
        d.line([(0, y), (W * k, y)], fill=c)
    cx, cy = W * k / 2, H * k * 0.42
    glow = Image.new("L", im.size, 0)
    gd = ImageDraw.Draw(glow)
    r = W * k * 0.42
    gd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(W * k * 0.18))
    im = Image.composite(Image.new("RGB", im.size, LAMP), im, glow.point(lambda v: v // 3))
    d = ImageDraw.Draw(im)
    mark(d, kind, cx, cy, W * k * 0.5, LAMP)
    font = ImageFont.truetype(font_path, round(H * k * 0.075))
    try:
        font.set_variation_by_name("Medium")
    except Exception:
        pass
    name = kind.capitalize()
    tw = d.textlength(name, font=font)
    d.text(((W * k - tw) / 2, H * k * 0.76), name, fill=WHITE, font=font)
    return im.resize((W, H), Image.LANCZOS)


def main():
    font, out = sys.argv[1], sys.argv[2]
    for kind in ("photos", "videos", "music"):
        os.makedirs(os.path.join(out, kind), exist_ok=True)
        t8(cover(kind, 128, 176, font), os.path.join(out, kind, "cover.t8"))
        t8(cover(kind, 256, 361, font), os.path.join(out, kind, "coverhd.t8"))


main()
