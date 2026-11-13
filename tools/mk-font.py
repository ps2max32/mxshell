#!/usr/bin/env python3
"""MXSHELL2's font atlases: font/font_<mode>.z and font/font.h, from the
fonts in font/src (Inter Tight and JetBrains Mono, both SIL Open Font
License).

  mk-font.py <font dir> <out dir>

One atlas per video mode, its glyphs made for that mode's pixels and drawn
1:1 there:
  hd    1920 x 1080 (1080i): square pixels
  ntsc  640 x 448 on a 4:3 screen: a pixel 1.07 times as tall as wide... so
        the glyphs are made 1.07 times as wide as at square pixels
  pal   640 x 512 on a 4:3 screen: a pixel 0.94 times as tall as wide: the
        glyphs 0.94 times as wide
Each .z is (zlib) W x H texels of 4 bits (two a byte, the left one in the low
bits): the coverage 0..15; the shell gives it a palette of white at that
alpha. font.h has where each glyph is, and its metrics, in that mode's
pixels.

The sizes (pixel heights; SD is relatively larger: smaller is not legible
on a TV):
  FONT_LABEL  JetBrains Mono 500: capitals with tracking (labels)
  FONT_SMALL  Inter Tight 400
  FONT_BODY   Inter Tight 400
  FONT_BUTTON Inter Tight 500
  FONT_TILE   Inter Tight 300: a resting tile's name, settings rows
  FONT_TITLE  Inter Tight 300: the lit tile's name, section titles
  FONT_DISP   Inter Tight 300: the caption's title, a page's title
Characters 1-4 of BODY and BUTTON are the pad's cross, circle, square and
triangle as outlines; 5 is the middle dot.
"""
import os
import sys
import zlib
from PIL import Image, ImageDraw, ImageFont

FACES = [("LABEL", "mono", 500), ("SMALL", "sans", 400), ("BODY", "sans", 400), ("BUTTON", "sans", 500),
         ("TILE", "sans", 300), ("TITLE", "sans", 300), ("DISP", "sans", 300)]
# the sizes in layout units (the mockups' CSS px), and the least pixel
# heights that stay legible on a TV
UNITS = [11.3, 12.7, 14.7, 15.3, 19.3, 26, 46]
LEAST = [10, 12, 14, 14, 17, 22, 36]
# an atlas per shape of pixel: pixels a unit across and down
#   hd      1080i, 1080i50 (1920 x 1080, 16:9)
#   hd540   1080p30 (a 1920 x 540 buffer, each line shown twice)
#   p720    720p, 720p50 (1280 x 720)
#   p576    576p (640 x 576, 4:3)
#   p480    480p (640 x 480, 4:3)
#   ntsc    640 x 448, 4:3
#   pal     640 x 512, 4:3
#   ...w    the same at 16:9 (anamorphic: 1280 units across 640 pixels)
SHAPES = {"hd": (1.5, 1.5), "hd540": (1.5, 0.75), "p720": (1.0, 1.0), "p576": (640 / 960, 576 / 720),
          "p480": (640 / 960, 480 / 720), "ntsc": (640 / 960, 448 / 720), "pal": (640 / 960, 512 / 720),
          "p576w": (640 / 1280, 576 / 720), "p480w": (640 / 1280, 480 / 720),
          "ntscw": (640 / 1280, 448 / 720), "palw": (640 / 1280, 512 / 720)}
MODES = {}
for name, (sx, sy) in SHAPES.items():
    MODES[name] = (1024 if sy > 1 else 512, sx / sy, [max(round(u * sy), m) for u, m in zip(UNITS, LEAST)])
SYMS = {1: "cross", 2: "circle", 3: "square", 4: "triangle"}


def face(path, weight, px):
    f = ImageFont.truetype(path, px)
    f.set_variation_by_axes([weight])
    return f


def glyph(font, ch, wide):
    """the glyph made at square pixels, then wide times as wide"""
    x0, y0, x1, y1 = font.getbbox(ch)
    w, h = max(x1 - x0, 1), max(y1 - y0, 1)
    k = 4
    big = Image.new("L", (w * k, h * k), 0)
    f2 = ImageFont.truetype(font.path, font.size * k)
    f2.set_variation_by_axes([font._weight])
    bx0, by0, _, _ = f2.getbbox(ch)
    ImageDraw.Draw(big).text((-bx0, -by0), ch, 255, font=f2)
    # not narrower than two texels (unless it was one): a stem a texel wide
    # (an l or an i, PAL at 16:9) falls between two texels when drawn
    # filtered, at half its strength, and the alpha test behind the text
    # leaves it out
    nw = max(min(w, 2), round(w * wide))
    im = big.resize((nw, h), Image.LANCZOS)
    return im, round(x0 * wide), y0, round(font.getlength(ch) * wide)


def symbol(kind, px, wide):
    """A pad symbol, a thin outline drawn at 8x and scaled down."""
    s = round(px * 0.62)
    k = 8
    big = Image.new("L", (s * k, s * k), 0)
    d = ImageDraw.Draw(big)
    lw = max(k, round(px * 0.075 * k))
    m, e = lw, s * k - lw
    if kind == "cross":
        d.line((m, m, e, e), 255, lw)
        d.line((e, m, m, e), 255, lw)
    elif kind == "circle":
        d.ellipse((m, m, e, e), outline=255, width=lw)
    elif kind == "square":
        d.rectangle((m, m, e, e), outline=255, width=lw)
    else:
        top, base = m + lw // 2, e - lw
        d.line((s * k // 2, top, e, base, m, base, s * k // 2, top), 255, lw, joint="curve")
    return big.resize((max(1, round(s * wide)), s), Image.LANCZOS)


def build(mode, paths, out, hfile):
    W, wide, pxs = MODES[mode]
    sizes = []
    for (name, fam, weight), px in zip(FACES, pxs):
        font = face(paths[fam], weight, px)
        font._weight = weight
        ascent, descent = font.getmetrics()
        gl = {c: glyph(font, chr(c), wide) for c in range(32, 127)}
        gl[5] = glyph(font, "·", wide)
        if name in ("BODY", "BUTTON"):
            for c, kind in SYMS.items():
                im = symbol(kind, px, wide)
                gl[c] = (im, 1, ascent - round(px * 0.36) - im.height // 2, im.width + 2)
        sizes.append((name, px, ascent, descent, gl))

    items = [(si, c) for si, s in enumerate(sizes) for c in s[4]]
    items.sort(key=lambda it: -sizes[it[0]][4][it[1]][0].height)
    x = y = row = 0
    place = {}
    for si, c in items:
        im = sizes[si][4][c][0]
        if x + im.width + 2 > W:
            x, y, row = 0, y + row + 2, 0
        place[(si, c)] = (x, y)
        x += im.width + 2
        row = max(row, im.height)
    h = 8
    while h < y + row + 2:
        h *= 2
    atlas = Image.new("L", (W, h), 0)
    for (si, c), (px, py) in place.items():
        atlas.paste(sizes[si][4][c][0], (px, py))
    q = bytes((v * 15 + 127) // 255 for v in atlas.tobytes())
    packed = bytes(q[i] | q[i + 1] << 4 for i in range(0, len(q), 2))
    # zlib: the atlases are mostly empty (1.3 MB in all, 0.33 MB packed); the
    # shell unpacks the one of its mode
    open(os.path.join(out, f"font_{mode}.z"), "wb").write(zlib.compress(packed, 9))

    M = mode.upper()
    hfile.write(f"#define FONT_{M}_W {W}\n#define FONT_{M}_H {h}\n")
    hfile.write(f"static const struct font fonts_{mode}[FONT_SIZES] = {{\n")
    for si, (name, px, ascent, descent, gl) in enumerate(sizes):
        hfile.write(f"  /* FONT_{name} */ {{{px}, {ascent}, {descent}, {{\n")
        for c in range(1, 127):
            if c in gl:
                im, ox, oy, adv = gl[c]
                gx, gy = place[(si, c)]
                hfile.write(f"    {{{gx}, {gy}, {im.width}, {im.height}, {ox}, {oy - ascent}, {adv}}},\n")
            else:
                hfile.write("    {0, 0, 0, 0, 0, 0, 0},\n")
        hfile.write("  }},\n")
    hfile.write("};\n\n")
    print(f"font_{mode}.z: {W} x {h}, 4 bits")


def main():
    src, out = sys.argv[1:3]
    paths = {"sans": os.path.join(src, "InterTight-Variable.ttf"),
             "mono": os.path.join(src, "JetBrainsMono-Variable.ttf")}
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "font.h"), "w") as f:
        f.write("/* made by tools/mk-font.py: the glyphs of font_<mode>.z (Inter Tight, JetBrains Mono: SIL OFL) */\n")
        f.write("#ifndef FONT_H\n#define FONT_H\n\n")
        f.write("#define FONT_FIRST 1\n#define FONT_LAST 126\n\n")
        f.write("struct glyph { unsigned short x, y; unsigned char w, h; signed char ox, oy; unsigned char adv; };\n")
        f.write("struct font { unsigned char px, ascent, descent; struct glyph g[FONT_LAST - FONT_FIRST + 1]; };\n\n")
        f.write("enum { " + ", ".join(f"FONT_{n}" for n, _, _ in FACES) + ", FONT_SIZES };\n\n")
        for mode in MODES:
            build(mode, paths, out, f)
        f.write("#endif\n")


main()
