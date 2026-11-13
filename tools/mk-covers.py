#!/usr/bin/env python3
"""Covers for the games, made from each game's own data (its title or
console background picture), for your own USB stick.

  mk-covers.py <data dir> <out dir> [id ...]
  mk-covers.py --boxart <image dir> <out dir>

With --boxart the covers are made from box art you have (<image dir>/<id>.png
or .jpg, e.g. the game's box scanned or downloaded for your own use): as a
DVD case, 1 : 1.41, the picture filling it (its sides cut as needed).

<data dir> holds the games' data as the PS2 tests use it (id1, baseq2,
baseq3, data1, hl/valve, doom, duke3d, d3). For each game id it writes
<out dir>/<id>/cover.t8 (128 x 176, mxshell's format, apps/mxshell/tools/
mk-cover.py) and coverhd.t8 (256 x 361: 256 x 1.41, a DVD case, the same format, for MXSHELL2):
a CLUT of 256 RGBA entries in the GS's CSM1 order, alpha 0x80, then the
indices. The cover is the picture across the width, over a blurred,
darkened copy of itself that fills the rest.
"""
import io
import os
import struct
import sys
import zipfile
from PIL import Image, ImageFilter, ImageEnhance


def pak_files(path):
    d = open(path, "rb").read()
    ofs, ln = struct.unpack("<ii", d[4:12])
    out = {}
    for i in range(ln // 64):
        e = d[ofs + i * 64:ofs + i * 64 + 64]
        name = e[:56].split(b"\0")[0].decode()
        fo, fl = struct.unpack("<ii", e[56:64])
        out[name] = (d, fo, fl)
    return out


def pak_read(path, name):
    d, fo, fl = pak_files(path)[name]
    return d[fo:fo + fl]


def lmp(data, pal):
    w, h = struct.unpack("<ii", data[:8])
    im = Image.frombytes("P", (w, h), data[8:8 + w * h])
    im.putpalette(pal)
    return im.convert("RGB")


def quake(data):
    p = os.path.join(data, "id1/pak0.pak")
    return lmp(pak_read(p, "gfx/conback.lmp"), pak_read(p, "gfx/palette.lmp"))


def hexen2(data):
    p = os.path.join(data, "data1/pak0.pak")
    return lmp(pak_read(p, "gfx/menu/conback.lmp"), pak_read(p, "gfx/palette.lmp"))


def halflife(data):
    p = os.path.join(data, "hl/valve/pak0.pak")
    d = pak_read(p, "gfx/conback.lmp")
    w, h = struct.unpack("<ii", d[:8])
    n = w * h
    # a qpic of Half-Life: the pixels, then the count of colours (2 bytes) and the palette
    pal = d[8 + n + 2:8 + n + 2 + 768]
    if len(pal) < 768:
        pal = pak_read(p, "gfx/palette.lmp")
    return lmp(d, pal)


def quake2(data):
    p = os.path.join(data, "baseq2/pak0.pak")
    return Image.open(io.BytesIO(pak_read(p, "pics/conback.pcx"))).convert("RGB")


def quake3(data):
    z = zipfile.ZipFile(os.path.join(data, "baseq3/pak0.pk3"))
    return Image.open(io.BytesIO(z.read("levelshots/Q3DM17.jpg"))).convert("RGB")


def doom(data):
    d = open(os.path.join(data, "doom/doom2.wad"), "rb").read()
    n, ofs = struct.unpack("<ii", d[4:12])
    lumps = {}
    for i in range(n):
        fp, sz, name = struct.unpack("<ii8s", d[ofs + i * 16:ofs + i * 16 + 16])
        lumps.setdefault(name.split(b"\0")[0].decode(), (fp, sz))
    fp, sz = lumps["PLAYPAL"]
    pal = d[fp:fp + 768]
    fp, sz = lumps["TITLEPIC"]
    p = d[fp:fp + sz]
    w, h = struct.unpack("<hh", p[:4])
    im = Image.new("P", (w, h), 0)
    px = im.load()
    for x in range(w):
        o = struct.unpack("<i", p[8 + 4 * x:12 + 4 * x])[0]
        while p[o] != 0xFF:
            top, ln = p[o], p[o + 1]
            for y in range(ln):
                if top + y < h:
                    px[x, top + y] = p[o + 3 + y]
            o += ln + 4
    im.putpalette(pal)
    return im.convert("RGB")


def duke3d(data):
    d = open(os.path.join(data, "duke3d/duke3d.grp"), "rb").read()
    n = struct.unpack("<i", d[12:16])[0]
    files, at = {}, 16 + 16 * n
    for i in range(n):
        name, sz = struct.unpack("<12si", d[16 + 16 * i:32 + 16 * i])
        files[name.split(b"\0")[0].decode().upper()] = d[at:at + sz]
        at += sz
    pal = bytes(min(255, c * 4) for c in files["PALETTE.DAT"][:768])
    def tile(num):
        for k in range(20):
            a = files.get("TILES%03d.ART" % k)
            if not a:
                continue
            _, _, t0, t1 = struct.unpack("<4i", a[:16])
            if not t0 <= num <= t1:
                continue
            cnt = t1 - t0 + 1
            xs = struct.unpack("<%dh" % cnt, a[16:16 + 2 * cnt])
            ys = struct.unpack("<%dh" % cnt, a[16 + 2 * cnt:16 + 4 * cnt])
            o = 16 + 8 * cnt + sum(xs[i] * ys[i] for i in range(num - t0))
            i = num - t0
            # columns, one after the other
            im = Image.frombytes("P", (ys[i], xs[i]), a[o:o + xs[i] * ys[i]]).transpose(Image.TRANSPOSE)
            im = Image.frombytes("P", im.size, im.tobytes())  # a plain copy: this PIL's resize fails on the view
            im.putpalette(pal)
            return im
        raise KeyError(num)

    # Duke with his guns, the title picture (tile 2493)
    im = tile(2493)
    im = Image.frombytes("RGB", im.size, im.convert("RGB").tobytes())
    return im


GAMES = {"quake1": quake, "quake2": quake2, "quake3": quake3, "hexen2": hexen2, "halflife": halflife,
         "doom": doom, "duke3d": duke3d}


def cover(im, w, h):
    """the picture across the width, over a blurred and darkened copy that fills the cover"""
    s = max(w / im.width, h / im.height)
    bg = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x, y = (bg.width - w) // 2, (bg.height - h) // 2
    bg = bg.crop((x, y, x + w, y + h)).filter(ImageFilter.GaussianBlur(w / 18))
    bg = ImageEnhance.Brightness(bg).enhance(0.38)
    fg = im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)
    out = bg.copy()
    out.paste(fg, (0, round(h * 0.40 - fg.height / 2)))
    # the foot darker, for the label on it
    shade = Image.linear_gradient("L").resize((w, h))
    dark = Image.new("RGB", (w, h), (8, 8, 8))
    mask = shade.point(lambda v: max(0, v - 150) * 2)
    out.paste(dark, (0, 0), mask)
    return out


def t8(im, path):
    q = im.quantize(256, dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette() + [0] * 768
    clut = bytearray(1024)
    for i in range(256):
        at = (i & ~0x18) | (i & 8) << 1 | (i & 0x10) >> 1
        clut[at * 4:at * 4 + 4] = bytes(pal[i * 3:i * 3 + 3]) + b"\x80"
    open(path, "wb").write(bytes(clut) + q.tobytes())


DVD = 1.41  # a DVD case: height / width


def boxart(src, out):
    for f in sorted(os.listdir(src)):
        gid, ext = os.path.splitext(f)
        if ext.lower() not in (".png", ".jpg", ".jpeg"):
            continue
        im = Image.open(os.path.join(src, f)).convert("RGB")
        im = Image.frombytes("RGB", im.size, im.tobytes())
        os.makedirs(os.path.join(out, gid), exist_ok=True)
        hd = case(im, 256, round(256 * DVD))
        t8(hd, os.path.join(out, gid, "coverhd.t8"))
        hd.save(os.path.join(out, gid, "preview.png"))
        t8(case(im, 128, 176), os.path.join(out, gid, "cover.t8"))
        print(f"{gid}: {im.width} x {im.height}")


def case(im, w, h):
    """the box art filling the whole case: scaled until it covers it, the
    middle kept (a big box is wider than a DVD case: its sides are cut)"""
    s = max(w / im.width, h / im.height)
    big = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x, y = (big.width - w) // 2, (big.height - h) // 2
    return Image.frombytes("RGB", (w, h), big.crop((x, y, x + w, y + h)).tobytes())


def main():
    if sys.argv[1] == "--boxart":
        boxart(sys.argv[2], sys.argv[3])
        return
    data, out = sys.argv[1:3]
    ids = sys.argv[3:] or list(GAMES)
    for gid in ids:
        try:
            im = GAMES[gid](data)
        except Exception as e:  # missing data: no cover, the shell draws the title
            print(f"{gid}: no picture ({e})")
            continue
        os.makedirs(os.path.join(out, gid), exist_ok=True)
        t8(cover(im, 128, 176), os.path.join(out, gid, "cover.t8"))
        t8(cover(im, 256, 352), os.path.join(out, gid, "coverhd.t8"))
        cover(im, 256, 352).save(os.path.join(out, gid, "preview.png"))
        print(f"{gid}: {im.width} x {im.height}")


main()
