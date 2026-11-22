#!/usr/bin/env python3
"""A cover for the shell: any image to cover.t8, which game.ini names (cover=).

  mk-cover.py <image> <cover.t8> [W H]

The file is what the GS takes: a CLUT of 256 entries (RGBA, alpha 0x80, in the
order of CSM1: bits 3 and 4 of the index swapped), then W x H indices: 128 x
176 (cover=), or 256 x 361 for coverhd.t8 (cover_hd=). Run by hand when a
picture changes; the .t8 files are kept with the picture.
"""
import sys
from PIL import Image

W, H = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) == 5 else (128, 176)
im = Image.open(sys.argv[1]).convert("RGB")
scale = max(W / im.width, H / im.height)
im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
x, y = (im.width - W) // 2, (im.height - H) // 2
im = im.crop((x, y, x + W, y + H)).quantize(256, dither=Image.Dither.FLOYDSTEINBERG)
pal = im.getpalette() + [0] * 768
clut = bytearray(1024)
for i in range(256):
    at = (i & ~0x18) | (i & 8) << 1 | (i & 0x10) >> 1
    clut[at * 4:at * 4 + 4] = bytes(pal[i * 3:i * 3 + 3]) + b"\x80"
open(sys.argv[2], "wb").write(bytes(clut) + im.tobytes())
