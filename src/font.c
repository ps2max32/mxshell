/* MXSHELL2: text from the font atlas of the video mode (font/font_<mode>.z,
   made by tools/mk-font.py): 4-bit coverage with a palette of white at that
   alpha, one texture. The glyphs are made for the mode's pixels (NTSC and
   PAL with their pixels that are not square); each is placed on the pixel
   grid and drawn 1:1.

   Filtering is bilinear: beam racing draws each field half a line apart,
   and a nearest sample would take one row twice and skip the next (doubled
   lines); bilinear blends the two. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdlib.h>
#include <mx/debug.h>
#include <string.h>
#include <zlib.h>

#include "pn.h"

#define Z(n) extern const unsigned char font_##n##_z[], font_##n##_z_end[];
Z(hd)
Z(hd540)
Z(p720)
Z(p576)
Z(p480)
Z(ntsc)
Z(pal)
Z(p576w)
Z(p480w)
Z(ntscw)
Z(palw)
#undef Z
static const struct {
  const unsigned char *z, *z_end; /* zlib */
  const struct font *set;
  int w, h;
} atlases[] = {
#define A(S, n, N) [SHAPE_##S] = {font_##n##_z, font_##n##_z_end, fonts_##n, FONT_##N##_W, FONT_##N##_H}
    A(HD, hd, HD), A(HD540, hd540, HD540), A(P720, p720, P720), A(P576, p576, P576), A(P480, p480, P480),
    A(NTSC, ntsc, NTSC), A(PAL, pal, PAL), A(P576W, p576w, P576W), A(P480W, p480w, P480W),
    A(NTSCW, ntscw, NTSCW), A(PALW, palw, PALW),
#undef A
};
static GLuint font_tex;
static const struct font *fset = fonts_hd;
static int tex_w = FONT_HD_W, tex_h = FONT_HD_H;

void text_init(void) {
  fset = atlases[video_shape].set, tex_w = atlases[video_shape].w, tex_h = atlases[video_shape].h;
  u32 n = (u32)(tex_w * tex_h / 2);
  u8 *buf = malloc(64 + n);
  if (!buf)
    return;
  for (int i = 0; i < 16; i++)
    buf[4 * i] = buf[4 * i + 1] = buf[4 * i + 2] = 255, buf[4 * i + 3] = (u8)(i * 17);
  /* the mode's atlas, unpacked (zlib) */
  uLongf got = n;
  if (uncompress(buf + 64, &got, atlases[video_shape].z,
                 (uLong)(atlases[video_shape].z_end - atlases[video_shape].z)) != Z_OK || got != n) {
    mx_printf("mxshell2: the font of shape %d does not unpack\n", video_shape);
    free(buf);
    return;
  }
  /* the file has the left texel in the low bits; GL's 4-bit palette
     texture wants it in the high ones */
  for (u32 i = 0; i < n; i++)
    buf[64 + i] = (u8)(buf[64 + i] << 4 | buf[64 + i] >> 4);
  if (!font_tex)
    glGenTextures(1, &font_tex);
  glBindTexture(GL_TEXTURE_2D, font_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE4_RGBA8_OES, tex_w, tex_h, 0, (GLsizei)(64 + n), buf);
  free(buf);
}

/* from a line's middle to the baseline that centres the size's capitals
   and x height on it (Inter's capitals are 0.73 em, its x height 0.55: the
   middle of both at about 0.32 em), in units */
float text_mid(int size) { return 0.32f * fset[size].px / px_y; }

/* a size's line height in units (for wrapping) */
float text_line(int size) { return (fset[size].ascent + fset[size].descent) / px_y; }

static float run(int size, float x, float y, const char *s, u32 c, float track_units, int caps, int draw) {
  const struct font *f = &fset[size];
  const float ix = 1.0f / px_x, iy = 1.0f / px_y;
  if (draw) {
    /* only the glyphs' own pixels are written: the rest of their
       rectangles (alpha 0) would be blended and dithered again in 16-bit
       colour, which shows as a box of another pattern behind each letter */
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.0f);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glColor4ub((GLubyte)(c >> 16), (GLubyte)(c >> 8), (GLubyte)c, (GLubyte)(c >> 24));
    glBegin(GL_QUADS);
  }
  const float iw = 1.0f / tex_w, ih = 1.0f / tex_h;
  float track = (float)(int)(track_units * px_x + 0.5f);
  /* in pixels from here: the pen on the grid */
  float px = (float)(int)(x * px_x + 0.5f), py = (float)(int)(y * px_y + 0.5f), x0px = px;
  for (; *s; s++) {
    int ch = (unsigned char)*s;
    if (caps && ch >= 'a' && ch <= 'z')
      ch -= 32;
    if (ch < FONT_FIRST || ch > FONT_LAST)
      continue;
    const struct glyph *g = &f->g[ch - FONT_FIRST];
    if (draw && g->w > 1) {
      float gx = px + g->ox, gy = py + g->oy;
      float u0 = g->x * iw, v0 = g->y * ih, u1 = (g->x + g->w) * iw, v1 = (g->y + g->h) * ih;
      glTexCoord2f(u0, v0), glVertex2f(gx * ix, gy * iy);
      glTexCoord2f(u1, v0), glVertex2f((gx + g->w) * ix, gy * iy);
      glTexCoord2f(u1, v1), glVertex2f((gx + g->w) * ix, (gy + g->h) * iy);
      glTexCoord2f(u0, v1), glVertex2f(gx * ix, (gy + g->h) * iy);
    }
    px += g->adv + track;
  }
  if (draw) {
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
  }
  return x + (px - x0px) * ix;
}

float d_text(int size, float x, float y, const char *s, u32 c) { return run(size, x, y, s, c, 0, 0, 1); }
float d_text_w(int size, const char *s) { return run(size, 0, 0, s, 0, 0, 0, 0); }
/* the mono labels: capitals, a little apart */
float d_label(float x, float y, const char *s, u32 c) { return run(FONT_LABEL, x, y, s, c, 2, 1, 1); }
float d_label_w(const char *s) { return run(FONT_LABEL, 0, 0, s, 0, 2, 1, 0); }

/* text that fits in w: the size given, else smaller ones, else cut with "..." */
float d_text_fit(int size, int min_size, float x, float y, float w, const char *s, u32 c) {
  for (int sz = size; sz >= min_size; sz--)
    if (d_text_w(sz, s) <= w)
      return d_text(sz, x, y, s, c);
  char b[64];
  u32 n = (u32)strlen(s);
  if (n > sizeof b - 4)
    n = sizeof b - 4;
  memcpy(b, s, n);
  while (n > 0) {
    memcpy(b + n, "...", 4);
    if (d_text_w(min_size, b) <= w)
      break;
    n--;
  }
  return d_text(min_size, x, y, b, c);
}
