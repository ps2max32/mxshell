/* MXSHELL2: dithering into 16 bits (1080i, 720p, 1080p30, 1080p25, 16-bit SD) and a
   test picture to compare on a television.

   The GS adds an offset from a 4 x 4 matrix (DIMX, -4..3) to each colour
   and keeps the top 5 bits. Only Bayer's order is used:
     Off     no offsets: the plain 5-bit steps
     On      Bayer, the same every field
     Noise   Bayer moved about, a field at a time (mxgl_dither: with beam
             racing every field is drawn anew): all 16 places, each dx and
             each dy 0..3 used equally (as much across as down). On an
             interlaced screen a line is in every other field only, so the
             places alternate between two halves of 8, each of which gives
             every pixel every offset once: in its own field too a pixel
             averages to the exact shade (with the 8 places of before a
             line got only the low or only the high half). The order
             (found by tools: the running means over 2, 4 and 8 fields
             kept closest to exact, both for progressive and for each
             field) moves the error to the highest temporal frequencies:
             it looks like fine grain, not a pattern. */
#include <GL/gl.h>
#include <mx/pad.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

enum { D_OFF, D_ON, D_NOISE, NDITHER };
static const char *const names[NDITHER] = {"Off", "On", "Noise"};
static const char *const keys[NDITHER] = {"off", "on", "noise"};

static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
/* the places of Noise, (dx, dy) in their order: the even ones are one
   half, the odd ones the other */
#define NPLACES 16
static const int places[NPLACES][2] = {{0, 0}, {2, 2}, {2, 1}, {0, 1}, {0, 2}, {1, 3}, {1, 0}, {3, 0},
                                       {3, 3}, {1, 1}, {1, 2}, {3, 2}, {3, 1}, {2, 0}, {2, 3}, {0, 3}};

/* DIMX of Bayer moved by (dx, dy): row r, column c in bits 16 r + 4 c,
   three bits signed, offsets -4..3 */
static u64 dimx(int dx, int dy) {
  u64 v = 0;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      v |= (u64)((bayer[(r + dy) & 3][(c + dx) & 3] / 2 - 4) & 7) << (16 * r + 4 * c);
  return v;
}

int dither_choice(void) {
  const char *v = cfg_get("video.dither", "noise");
  for (int i = 0; i < NDITHER; i++)
    if (!strcmp(v, keys[i]))
      return i;
  return D_NOISE;
}

const char *dither_name(int d) { return names[d]; }

void dither_apply(void) {
  unsigned long long m[NPLACES];
  int n = 0;
  switch (dither_choice()) {
  case D_OFF:
    m[n++] = 0;
    break;
  case D_ON:
    m[n++] = dimx(0, 0);
    break;
  default:
    for (; n < NPLACES; n++)
      m[n] = dimx(places[n][0], places[n][1]);
    break;
  }
  mxgl_dither(m, n);
}

void dither_next(int step) {
  int d = (dither_choice() + NDITHER + step) % NDITHER;
  cfg_set("video.dither", keys[d]);
  cfg_save();
  dither_apply();
}
