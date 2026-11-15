/* MXSHELL2: surfaces, the one way the shell and its apps draw a rounded
   shape: cards, notifications, pills, buttons, rings, bars. Every one has
   the same edge and the same light, as the game tiles have (tile.c):

   - The GS does not smooth a triangle's edges, so a surface gets a rim:
     from 1.5 pixels inside its outline (its own colour) to the outline (the
     edge's colour), then a pixel out to nothing (alpha 0). It blends with
     whatever is under it (the room, a tile, another card), so its corners
     look right over anything. Only that rim is translucent: the inside of
     an opaque surface is opaque, and 16-bit colour is dithered once there.
   - The lamp (lamp > 0): the edge takes the lamp's colour where it faces
     up, strongest straight up, so the light follows the top edge into the
     rounded corners and fades out round them.
   - Fading (ui_fade, or a: a card sliding in) makes the whole surface
     translucent, rim and all: its corners stay round. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <math.h>

#include "pn.h"

/* glass takes the colour under it: the room's, or the one set here */
static int under_set;
static u32 under_rgb;
void aa_under(u32 rgb) { under_set = 1, under_rgb = rgb; }
void aa_under_room(void) { under_set = 0; }

static void under_at(float x, float y, float rgb[3]) {
  if (under_set)
    rgb[0] = (float)(under_rgb >> 16 & 255), rgb[1] = (float)(under_rgb >> 8 & 255), rgb[2] = (float)(under_rgb & 255);
  else
    room_col(x, y, rgb);
}

static float snapx(float x) { return (float)(int)(x * px_x + 0.5f) / px_x; }
static float snapy(float y) { return (float)(int)(y * px_y + 0.5f) / px_y; }

float lamp_edge(float ny, float lamp) {
  float up = ny < 0 ? ny * ny : 0;
  return lamp * up * up;
}

#define CSEG 10 /* segments of a corner */
#define OUTLINE (4 * (CSEG + 1))

/* the outline of a rounded rectangle, clockwise from the top left corner's
   start, with the outward normal of each point */
static int outline(float x0, float y0, float x1, float y1, float r, float *px, float *py, float *nx, float *ny) {
  float ccx[4] = {x0 + r, x1 - r, x1 - r, x0 + r}, ccy[4] = {y0 + r, y0 + r, y1 - r, y1 - r};
  /* the directions, once (sinf and cosf for every point of every shape
     were a good part of a frame) */
  static float cs[OUTLINE], sn[OUTLINE];
  if (!cs[0])
    for (int c = 0, k = 0; c < 4; c++)
      for (int i = 0; i <= CSEG; i++, k++) {
        float a = 3.1415926f + 1.5707963f * (c + (float)i / CSEG);
        cs[k] = cosf(a), sn[k] = sinf(a);
      }
  int n = 0;
  for (int c = 0; c < 4; c++)
    for (int i = 0; i <= CSEG; i++) {
      px[n] = ccx[c] + cs[n] * r, py[n] = ccy[c] + sn[n] * r, nx[n] = cs[n], ny[n] = sn[n];
      n++;
    }
  return n;
}

/* a shape's part of ui_fade: the last of it, and fast. A light pill half
   way is a grey patch with no text to read on it, so text comes first and
   goes last */
static float shape_fade(void) {
  if (ui_fade >= 0.999f)
    return 1;
  float f = clampf((ui_fade - 0.6f) / 0.4f, 0, 1);
  return f * f;
}

static void rgb3(u32 c, float o[3]) {
  o[0] = (float)(c >> 16 & 255), o[1] = (float)(c >> 8 & 255), o[2] = (float)(c & 255);
}

/* a point's colour and opacity: the fill (top to bottom with grad, or the
   glass) at y, without the surface's own a */
static void fill_at(const struct surface *s, float x, float y, float y0, float y1, float o[4]) {
  if (s->glass >= 0) {
    under_at(x, y, o);
    o[0] += s->glass, o[1] += s->glass, o[2] += s->glass, o[3] = 1;
    return;
  }
  float f[3];
  rgb3(s->fill, f);
  if (!s->grad) {
    o[0] = f[0], o[1] = f[1], o[2] = f[2], o[3] = 1;
    return;
  }
  float b[3], t = y1 > y0 ? clampf((y - y0) / (y1 - y0), 0, 1) : 0;
  rgb3(s->fill_b, b);
  for (int j = 0; j < 3; j++)
    o[j] = f[j] + (b[j] - f[j]) * t;
  o[3] = s->fa + (s->fa_b - s->fa) * t;
}

void surface_draw(const struct surface *s) {
  float a = clampf(s->a, 0, 1) * shape_fade();
  if (a < 0.004f)
    return;
  float x0 = snapx(s->x0), x1 = snapx(s->x1), y0 = snapy(s->y0), y1 = snapy(s->y1), r = s->r;
  if (x1 - x0 < 1 / px_x || y1 - y0 < 1 / px_y)
    return;
  if (r > (y1 - y0) * 0.5f)
    r = (y1 - y0) * 0.5f;
  if (r > (x1 - x0) * 0.5f)
    r = (x1 - x0) * 0.5f;
  float px[OUTLINE], py[OUTLINE], nx[OUTLINE], ny[OUTLINE];
  int n = outline(x0, y0, x1, y1, r, px, py, nx, ny);
  float la = (float)(s->line >> 24) / 255.0f, line[3], lamp[3];
  rgb3(s->line, line), rgb3(C_LAMP, lamp);
  /* the inside ring (1.5 pixels in), the edge (the outline), the rim's
     outer side (a pixel out): x, y, rgb, alpha */
  static float in[OUTLINE][6], ed[OUTLINE][6];
  /* the inside ring 1.5 pixels in, but not past the middle of a thin
     shape (a bar): there it would fold over and show as a brighter dot */
  float ix = fminf(1.5f / px_x, r * 0.5f), iy = fminf(1.5f / px_y, r * 0.5f), ox = 1.0f / px_x, oy = 1.0f / px_y;
  for (int k = 0; k < n; k++) {
    in[k][0] = px[k] - nx[k] * ix, in[k][1] = py[k] - ny[k] * iy;
    ed[k][0] = px[k], ed[k][1] = py[k];
    float *f = in[k] + 2, *e = ed[k] + 2;
    fill_at(s, in[k][0], in[k][1], y0, y1, f);
    /* the edge: the line over the fill (a ring: the line alone), then the
       lamp over that, which also makes a translucent surface's edge
       opaque where it is lit */
    float kl = lamp_edge(ny[k], s->lamp), ea = s->hollow ? la : f[3] + (1 - f[3]) * la;
    for (int j = 0; j < 3; j++) {
      e[j] = s->hollow ? line[j] : f[j] + (line[j] - f[j]) * la;
      e[j] += (lamp[j] - e[j]) * kl;
    }
    e[3] = (ea + (1 - ea) * kl) * a;
    f[3] = s->hollow ? 0 : f[3] * a;
    if (s->hollow)
      f[0] = e[0], f[1] = e[1], f[2] = e[2];
  }
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  if (!s->hollow) {
    float c[4], cxm = (x0 + x1) * 0.5f, cym = (y0 + y1) * 0.5f;
    fill_at(s, cxm, cym, y0, y1, c);
    glBegin(GL_TRIANGLE_FAN);
    d_vertex(cxm, cym, c, c[3] * a);
    for (int i = 0; i <= n; i++)
      d_vertex(in[i % n][0], in[i % n][1], in[i % n] + 2, in[i % n][5]);
    glEnd();
  }
  glBegin(GL_TRIANGLE_STRIP);
  for (int i = 0; i <= n; i++) {
    int k = i % n;
    d_vertex(in[k][0], in[k][1], in[k] + 2, in[k][5]);
    d_vertex(ed[k][0], ed[k][1], ed[k] + 2, ed[k][5]);
  }
  glEnd();
  glBegin(GL_TRIANGLE_STRIP);
  for (int i = 0; i <= n; i++) {
    int k = i % n;
    d_vertex(ed[k][0], ed[k][1], ed[k] + 2, ed[k][5]);
    d_vertex(px[k] + nx[k] * ox, py[k] + ny[k] * oy, ed[k] + 2, 0);
  }
  glEnd();
}

/* the shapes as before, on surfaces */
void d_pill(float x0, float y0, float x1, float y1, u32 c) {
  surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .r = (y1 - y0) * 0.5f, .fill = c, .glass = -1, .a = 1});
}
void d_pill_glass(float x0, float y0, float x1, float y1, float lighter, u32 line) {
  surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .r = (y1 - y0) * 0.5f, .glass = lighter, .line = line, .a = 1});
}
void d_card(float x0, float y0, float x1, float y1, float r, u32 c) {
  surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .r = r, .fill = c, .glass = -1, .a = 1});
}
void d_card_lit(float x0, float y0, float x1, float y1, float r, u32 c, float a, float lamp) {
  surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .r = r, .fill = c, .glass = -1, .lamp = lamp, .a = a});
}
/* a translucent white highlight (a chosen row), a_top to a_bottom, with
   the lamp along its top */
void d_highlight(float x0, float y0, float x1, float y1, float r, float a_top, float a_bottom, float lamp) {
  surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .r = r, .fill = C_WHITE, .glass = -1,
                                 .lamp = lamp, .a = 1, .grad = 1, .fill_b = C_WHITE, .fa = a_top, .fa_b = a_bottom});
}
/* a bar: its track, and the part k (0..1) in c */
void d_bar(float x0, float y0, float x1, float y1, float k, u32 c, float a) {
  float r = (y1 - y0) * 0.5f;
  surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .r = r, .fill = C_WHITE, .glass = -1,
                                 .a = a, .grad = 1, .fill_b = C_WHITE, .fa = 0.14f, .fa_b = 0.14f});
  float w = (x1 - x0) * clampf(k, 0, 1);
  if (w > 0)
    surface_draw(&(struct surface){.x0 = x0, .y0 = y0, .x1 = x0 + (w > 2 * r ? w : 2 * r), .y1 = y1, .r = r,
                                   .fill = c, .glass = -1, .a = a * (w > 2 * r ? 1 : w / (2 * r))});
}
void d_ring(float cx, float cy, float r, u32 line) {
  surface_draw(&(struct surface){.x0 = cx - r, .y0 = cy - r, .x1 = cx + r, .y1 = cy + r, .r = r, .glass = -1, .line = line, .a = 1, .hollow = 1});
}
