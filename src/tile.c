/* MXSHELL2: the tiles, real geometry in a perspective, lit by the one lamp.

   A tile's face is a mesh of rows and columns inside its rounded outline,
   coloured per vertex (worked out on the EE for now; the plan moves it to
   VU1): a gradient from a lighter top to the deep bottom, and on the lit
   tile the lamp's highlight near its top, where the light comes from. A
   resting tile is the same gradient at half brightness.

   The perspective: at the shelf's depth D a unit is a unit of the 2D layer
   (square on the screen in every mode), so a face is laid out in units and
   only its turn makes it 3D. */
#include <GL/gl.h>
#include <math.h>

#include "mxgl.h"
#include "pn.h"

#define D 1400.0f    /* the eye's distance (the mockups' CSS perspective) */
#define NEAR 200.0f
#define THICK 9.0f   /* a tile's thickness */
#define RADIUS 6.0f  /* its corners */
#define COLS 6
#define MAX_ROWS 16

static float light_x;

void d_begin_3d(void) {
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  float s = NEAR / D;
  glFrustum(-SCR_W / 2 * s, SCR_W / 2 * s, -SCR_H / 2 * s, SCR_H / 2 * s, NEAR, 8000);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void light_at(float x, float y) {
  light_x = x;
  (void)y;
}

struct v3 {
  float x, y, z;
};

/* a point of the tile: (u, v) on its face (0..1 across, 0..1 down), d
   behind it; the tile turned about its vertical middle */
static struct v3 at(const struct tile_look *t, float u, float v, float d, float c, float s) {
  float cx = t->x + t->w * 0.5f - SCR_W / 2;
  float dx = (u - 0.5f) * t->w, dz = -d;
  struct v3 p = {cx + dx * c + dz * s, SCR_H / 2 - (t->y + v * t->h), -D - dx * s + dz * c};
  return p;
}

static float mixf(float a, float b, float k) { return a + (b - a) * k; }

/* the colour at (u, v) of the face: rgb 0..255 */
static void face_col(const struct tile_look *t, float u, float v, float out[3]) {
  u32 base = t->tex ? 0xD8D8D8u : t->base;
  float b[3] = {(float)(base >> 16 & 255), (float)(base >> 8 & 255), (float)(base & 255)};
  float deep[3] = {(float)(C_DEEP >> 16 & 255), (float)(C_DEEP >> 8 & 255), (float)(C_DEEP & 255)};
  float lamp[3] = {(float)(C_LAMP >> 16 & 255), (float)(C_LAMP >> 8 & 255), (float)(C_LAMP & 255)};
  /* along 165 degrees: down and a little to the right */
  float g = clampf((0.966f * v + 0.259f * u) / 1.1f, 0, 1);
  float l = t->lit;
  for (int i = 0; i < 3; i++) {
    /* lit: a lighter top, the colour at 55 %, deep at the end; resting:
       the colour to deep, at 68 % */
    float top = mixf(b[i], 255, 0.10f);
    float lit = g < 0.55f ? mixf(top, b[i], g / 0.55f) : mixf(b[i], deep[i], (g - 0.55f) / 0.45f);
    float rest = mixf(b[i], deep[i], g) * 0.68f;
    out[i] = mixf(rest, lit, l);
  }
  if (l > 0) {
    /* the lamp's highlight: an ellipse at the top, towards the light */
    float hx = clampf(0.35f + (light_x - (t->x + t->w * 0.5f)) / t->w * 0.4f, 0.1f, 0.9f);
    float du = (u - hx) / 1.2f, dv = v / 0.7f;
    float a = 0.30f * l * clampf(1 - sqrtf(du * du + dv * dv) / 0.6f, 0, 1);
    for (int i = 0; i < 3; i++)
      out[i] = mixf(out[i], lamp[i], a);
  }
}

/* a vertex, opaque: what shows through (a < 1) is mixed in here from the
   room's colour at its place on the screen, so 16-bit colour is dithered
   once (pn.h, the room) */
static int textured;
/* the tile's vertices, gathered and drawn as strips in one mxgl_MultiDraw
   (glBegin and a call a vertex were most of a tile's time): a record per
   vertex with its texture coordinates and colour in it, which libgl's
   fast path reads as it is */
struct tv {
  float x, y, z, s, t;
  u8 c[4];
};
#define TV_MAX 1024
#define PRIM_MAX 64
static struct tv tvs[TV_MAX];
static int ntv, nprim, prim_first[PRIM_MAX], prim_count[PRIM_MAX];
static unsigned prim_mode[PRIM_MAX];
static float cur_s, cur_t;
static void strip_begin(void) {
  if (nprim < PRIM_MAX)
    prim_mode[nprim] = GL_TRIANGLE_STRIP, prim_first[nprim] = ntv, prim_count[nprim] = 0;
}
static void strip_end(void) {
  if (nprim < PRIM_MAX)
    prim_count[nprim] = ntv - prim_first[nprim], nprim++;
}
/* the strips so far, with or without the texture bound */
static void flush(void) {
  if (nprim) {
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(struct tv), &tvs[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct tv), tvs[0].c);
    if (textured) {
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glTexCoordPointer(2, GL_FLOAT, sizeof(struct tv), &tvs[0].s);
    }
    mxgl_MultiDraw(prim_mode, prim_first, prim_count, nprim);
    if (textured)
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
  }
  ntv = nprim = 0;
}
static void tex(float s, float t) { cur_s = s, cur_t = t; }
static void vtx(struct v3 p, const float rgb[3], float a) {
  float c[3] = {rgb[0], rgb[1], rgb[2]};
  if (a < 0.999f && !textured) {
    float k = D / -p.z, room[3];
    room_col(SCR_W / 2 + p.x * k, SCR_H / 2 - p.y * k, room);
    for (int i = 0; i < 3; i++)
      c[i] = room[i] + (c[i] - room[i]) * a;
    a = 1;
  }
  int b = a >= 0.999f ? dither_bias : 0;
  if (ntv >= TV_MAX)
    return;
  struct tv *v = &tvs[ntv++];
  v->x = p.x, v->y = p.y, v->z = p.z, v->s = cur_s, v->t = cur_t;
  v->c[0] = (u8)clampf(c[0] + b, 0, 255), v->c[1] = (u8)clampf(c[1] + b, 0, 255), v->c[2] = (u8)clampf(c[2] + b, 0, 255);
  v->c[3] = (u8)clampf(a * 255, 0, 255);
}

/* the rows of the rounded outline: v of each, and the inset of the face's
   sides there (as a fraction of the width) */
static int rows(const struct tile_look *t, float *vs, float *ins) {
  static const float arc[4] = {0, 0.134f, 0.5f, 1}; /* along the radius: 1 - cos of 90, 60, 30, 0 degrees */
  int n = 0;
  const float r = RADIUS;
  for (int i = 0; i < 4; i++) {
    float y = arc[i] * r, dy = r - y;
    vs[n] = y / t->h, ins[n++] = (r - sqrtf(r * r - dy * dy)) / t->w;
  }
  for (int i = 1; i < 6; i++)
    vs[n] = (r + (t->h - 2 * r) * i / 6) / t->h, ins[n++] = 0;
  for (int i = 3; i >= 0; i--) {
    float y = arc[i] * r, dy = r - y;
    vs[n] = 1 - y / t->h, ins[n++] = (r - sqrtf(r * r - dy * dy)) / t->w;
  }
  return n;
}

void d_tile(const struct tile_look *t) {
  float c = cosf(t->turn * 0.0174533f), s = sinf(t->turn * 0.0174533f);
  float a = t->fade;

  /* the edge that shows when it is turned */
  if (t->turn > 0.5f || t->turn < -0.5f) {
    float su = t->turn > 0 ? 0 : 1;
    float e0[3], e1[3];
    face_col(t, su, 0, e0), face_col(t, su, 1, e1);
    for (int i = 0; i < 3; i++)
      e0[i] *= 0.55f, e1[i] *= 0.4f;
    strip_begin();
    vtx(at(t, su, RADIUS / t->h, 0, c, s), e0, a), vtx(at(t, su, RADIUS / t->h, THICK, c, s), e0, a);
    vtx(at(t, su, 1 - RADIUS / t->h, 0, c, s), e1, a), vtx(at(t, su, 1 - RADIUS / t->h, THICK, c, s), e1, a);
    strip_end();
    flush();
  }

  float vs[MAX_ROWS], ins[MAX_ROWS];
  int n = rows(t, vs, ins);
  if (t->tex) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)t->tex);
    textured = 1;
  }
  for (int r = 0; r + 1 < n; r++) {
    strip_begin();
    for (int k = 0; k <= COLS; k++)
      for (int q = 0; q < 2; q++) {
        float v = vs[r + q], in = ins[r + q];
        float u = in + (1 - 2 * in) * k / COLS;
        float rgb[3];
        face_col(t, u, v, rgb);
        tex(u * t->tex_u, v * t->tex_v);
        vtx(at(t, u, v, 0, c, s), rgb, a);
      }
    strip_end();
  }
  flush();
  if (t->tex)
    glDisable(GL_TEXTURE_2D);
  textured = 0;

  /* the edge: around the outline, from 1.5 pixels inside (the face's
     colour) to the outline itself, then a pixel out to the room's colour
     (pn.h, draw.c: a rim instead of blending). On the lit tile the
     outline takes the lamp's colour where it faces up: a line along the
     top that follows the rounded corners and fades out round them. */
  {
    float ou[2 * MAX_ROWS], ov[2 * MAX_ROWS];
    int m = 0;
    for (int r = 0; r < n; r++)
      ou[m] = ins[r], ov[m++] = vs[r];
    for (int r = n - 1; r >= 0; r--)
      ou[m] = 1 - ins[r], ov[m++] = vs[r];
    float pu = 1.0f / (px_x * t->w), pv = 1.0f / (px_y * t->h);
    float lamp[3] = {(float)(C_LAMP >> 16 & 255), (float)(C_LAMP >> 8 & 255), (float)(C_LAMP & 255)};
    float nus[2 * MAX_ROWS], nvs[2 * MAX_ROWS];
    for (int k = 0; k < m; k++) {
      int a0 = (k + m - 1) % m, a1 = (k + 1) % m;
      float du = (ou[a1] - ou[a0]) * t->w, dv = (ov[a1] - ov[a0]) * t->h;
      float len = sqrtf(du * du + dv * dv);
      nus[k] = len > 0 ? -dv / len : 0, nvs[k] = len > 0 ? du / len : -1;
    }
    /* the two points where the left and the right side meet at the top
       share a place: straight up there */
    nus[0] = 0, nvs[0] = -1, nus[m - 1] = 0, nvs[m - 1] = -1;
    for (int ring = 0; ring < 2; ring++) {
      strip_begin();
      for (int i = 0; i <= m; i++) {
        int k = i % m;
        float rgb[3], edge[3];
        face_col(t, ou[k], ov[k], rgb);
        if (t->tex)
          for (int j = 0; j < 3; j++)
            rgb[j] *= 0.5f; /* a cover's edge: its average, roughly */
        float k_lamp = lamp_edge(nvs[k], 0.6f * t->lit);
        for (int j = 0; j < 3; j++)
          edge[j] = rgb[j] + (lamp[j] - rgb[j]) * k_lamp;
        if (ring == 0) {
          vtx(at(t, ou[k] - nus[k] * pu * 1.5f, ov[k] - nvs[k] * pv * 1.5f, 0, c, s), rgb, a);
          vtx(at(t, ou[k], ov[k], 0, c, s), edge, a);
        } else {
          vtx(at(t, ou[k], ov[k], 0, c, s), edge, a);
          vtx(at(t, ou[k] + nus[k] * pu, ov[k] + nvs[k] * pv, 0, c, s), edge, 0);
        }
      }
      strip_end();
    }
    flush();
  }
}

/* the reflection's look: -1 the shell's; tests: refl=a|b|c, three to
   compare, in the colour of the face above its deep foot (the foot's
   near black makes the shell's look more a shadow than a reflection) */
int refl_style = -1;

void d_tile_reflection(const struct tile_look *t) {
  float c = cosf(t->turn * 0.0174533f), s = sinf(t->turn * 0.0174533f);
  float top, part, power;
  int rows = 4;
  switch (refl_style) {
  default: /* the shell's */
    top = 0.10f + 0.12f * t->lit, part = 0.32f, power = 2;
    break;
  case 0: /* a: clear, short */
    top = 0.28f + 0.14f * t->lit, part = 0.32f, power = 2;
    break;
  case 1: /* b: a long gloss, fading slowly */
    top = 0.20f + 0.12f * t->lit, part = 0.55f, power = 1, rows = 6;
    break;
  case 2: /* c: the lit one mirrored, the others faint */
    top = 0.10f + 0.36f * t->lit, part = 0.40f, power = 1.5f;
    break;
  }
  top *= t->fade;
  if (t->tex) {
    /* a cover: blended, the texture cannot be mixed in per vertex */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)t->tex);
    textured = 1;
  }
  float fy = SCR_H / 2 - (t->y + t->h);
  for (int r = 0; r < rows; r++) {
    strip_begin();
    for (int k = 0; k <= COLS; k++)
      for (int q = 0; q < 2; q++) {
        float f = (float)(r + q) / rows, v = 1 - f * part, u = (float)k / COLS;
        float rgb[3];
        face_col(t, u, refl_style < 0 ? v : 0.35f, rgb);
        struct v3 p = at(t, u, v, 0, c, s);
        p.y = 2 * fy - p.y;
        tex(u * t->tex_u, v * t->tex_v);
        vtx(p, rgb, top * powf(1 - f, power));
      }
    strip_end();
  }
  flush();
  if (t->tex)
    glDisable(GL_TEXTURE_2D);
  textured = 0;
}
