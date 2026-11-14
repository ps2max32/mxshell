/* MXSHELL2: the 2D layer on libgl. Pixel coordinates of the 640 x 448
   screen; shading is per vertex (the GS's Gouraud), light is added
   (GL_ONE) so it brightens what is under it as a lamp would. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <mx/debug.h>
#include <math.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

float frame_dt = 1.0f / 60.0f;

void spring_step(struct spring *s, float target, float w, float dt) {
  /* x'' = -w^2 (x - t) - 2 w x', in two half steps for stability */
  for (int i = 0; i < 2; i++) {
    float h = dt * 0.5f;
    float a = -w * w * (s->x - target) - 2.0f * w * s->v;
    s->v += a * h;
    s->x += s->v * h;
  }
}

/* 16-bit colour (1080i, or 16-bit SD) is dithered by the GS with offsets
   -4..3 and then cut to 5 bits: half a step darker on average. An opaque
   colour gets the half step added (dither_bias = 4), so it comes out as
   given. Translucent layers cannot be helped that way: the room (below)
   is drawn opaque for that. */
int dither_bias = 4;

static GLubyte bias(u32 v) { return (GLubyte)(v + dither_bias > 255 ? 255 : v + dither_bias); }

static void col(u32 c) {
  if (c >> 24 == 255)
    glColor4ub(bias(c >> 16 & 255), bias(c >> 8 & 255), bias(c & 255), 255);
  else
    glColor4ub((GLubyte)(c >> 16), (GLubyte)(c >> 8), (GLubyte)c, (GLubyte)(c >> 24));
}

/* ---- the room: the background, its lights and shadows, worked out per
   vertex on the EE and drawn opaque in one layer ---- */
#define MAX_LIGHTS 12
static struct {
  float cx, cy, rx, ry, a;
  u32 rgb;
} lights[MAX_LIGHTS];
static int nlights;
static float floor_y = -1, floor_k = 1;
float ui_fade = 1;

void room_begin(float fy) { nlights = 0, floor_y = fy, floor_k = 1; }
void room_floor(float k) { floor_k = k; }

static void add(float cx, float cy, float rx, float ry, float a, u32 rgb) {
  if (nlights < MAX_LIGHTS) {
    lights[nlights].cx = cx, lights[nlights].cy = cy, lights[nlights].rx = rx, lights[nlights].ry = ry;
    lights[nlights].a = a, lights[nlights++].rgb = rgb;
  }
}
void room_light(float cx, float cy, float rx, float ry, float a) { add(cx, cy, rx, ry, a, C_LAMP); }
void room_shadow(float cx, float cy, float rx, float ry, float a) { add(cx, cy, rx, ry, -a, 0); }

void room_col(float x, float y, float rgb[3]) {
  float b = (C_INK & 255);
  if (floor_y >= 0 && y >= floor_y) {
    /* the whole floor down, with a smooth start and end: no bend to show
       as a band (it is one 5-bit step, 16 to 8, dithered all the way) */
    float k = clampf((y - floor_y) / (SCR_H - floor_y), 0, 1);
    k = k * k * (3 - 2 * k);
    float f = (C_FLOOR & 255);
    b += (f - b) * (1 - k) * floor_k;
  }
  rgb[0] = rgb[1] = rgb[2] = b;
  for (int i = 0; i < nlights; i++) {
    float dx = (x - lights[i].cx) / lights[i].rx, dy = (y - lights[i].cy) / lights[i].ry;
    float r2 = dx * dx + dy * dy;
    if (r2 >= 1)
      continue;
    float f = (1 - r2) * (1 - r2) * lights[i].a;
    if (f < 0)
      for (int c = 0; c < 3; c++)
        rgb[c] *= 1 + f;
    else {
      u32 l = lights[i].rgb;
      rgb[0] += (l >> 16 & 255) * f, rgb[1] += (l >> 8 & 255) * f, rgb[2] += (l & 255) * f;
    }
  }
}

void room_vertex(float x, float y, const float rgb[3]) {
  glColor4ub(bias((u32)clampf(rgb[0], 0, 250)), bias((u32)clampf(rgb[1], 0, 250)), bias((u32)clampf(rgb[2], 0, 250)), 255);
  glVertex2f(x, y);
}

/* the room's lights on VU1 (room.vsm), with room=vu (tests): measured on
   the PS2 in 1080i, Home's busiest frames, it costs the GS passes 7 ms each
   instead of 2 (beam racing plays the frame's stream once a strip, three
   times a field, so VU1's work counts three times, the EE's once) and
   about 90 passes a second came too late. So the EE works the lights out
   (room_col), and this waits for a cheaper program (pipelined, fewer
   points) */
extern const struct mxgl_vu1_binary room_vu1;
int room_on_vu;
static GLuint room_prog;
static GLint u_lamp, u_lights, u_shadows;
static int room_vu_on(void) {
  static int tried;
  if (!tried && room_on_vu) {
    tried = 1;
    GLuint sh = glCreateShader(GL_VERTEX_SHADER), prog = glCreateProgram();
    glShaderBinary(1, &sh, GL_PS2_VU1_BINARY, &room_vu1, sizeof room_vu1);
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    u_lamp = glGetUniformLocation(prog, "lamp");
    u_lights = glGetUniformLocation(prog, "lights");
    u_shadows = glGetUniformLocation(prog, "shadows");
    if (ok && u_lamp >= 0 && u_lights >= 0 && u_shadows >= 0)
      room_prog = prog;
    else
      mx_printf("mxshell: room.vsm does not link: the room on the EE\n");
  }
  return room_prog && room_on_vu;
}

static void room_vu_draw(const unsigned *modes, const int *firsts, const int *counts, int n, const void *pos,
                         const void *rgba) {
  /* the lights and the shadows in groups of four, a lane each: cx, cy,
     1/rx, 1/ry, strength (an unused lane: strength 0) */
  float lg[5][4] = {{0}}, sg[3][5][4] = {{{0}}};
  int nl = 0, ns = 0;
  for (int i = 0; i < nlights; i++) {
    float *q[5];
    if (lights[i].a >= 0 && nl < 4)
      for (int k = 0; k < 5; k++)
        q[k] = &lg[k][nl];
    else if (lights[i].a < 0 && ns < 12)
      for (int k = 0; k < 5; k++)
        q[k] = &sg[ns / 4][k][ns % 4];
    else
      continue;
    *q[0] = lights[i].cx, *q[1] = lights[i].cy, *q[2] = 1 / lights[i].rx, *q[3] = 1 / lights[i].ry;
    *q[4] = lights[i].a >= 0 ? lights[i].a : -lights[i].a;
    lights[i].a >= 0 ? nl++ : ns++;
  }
  float lamp[4] = {(float)(C_LAMP >> 16 & 255), (float)(C_LAMP >> 8 & 255), (float)(C_LAMP & 255), (float)dither_bias};
  glUseProgram(room_prog);
  glUniform4fv(u_lamp, 1, lamp);
  glUniform4fv(u_lights, 5, &lg[0][0]);
  glUniform4fv(u_shadows, 15, &sg[0][0][0]);
  glDisable(GL_BLEND);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, pos);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);
  mxgl_MultiDraw(modes, firsts, counts, n);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glEnable(GL_BLEND);
  glUseProgram(0);
}

void room_draw(void) {
  /* rows: even, and close together around the floor, where the shadows
     and the pool are small. The lights are curves: between two rows (and
     along each triangle's diagonal) the GS draws them as straight lines,
     and a bend there shows as a band, so the grid is fine (64 x ~44) */
  float ys[64];
  int ny = 0;
  float top_end = floor_y >= 0 ? floor_y - 48 : SCR_H;
  int nt = floor_y >= 0 ? 24 : 40;
  for (int i = 0; i <= nt; i++)
    ys[ny++] = top_end * i / nt;
  if (floor_y >= 0) {
    static const float near[] = {-36, -26, -18, -11, -5, 0, 1, 5, 10, 16, 23, 31, 40, 50, 62, 76, 92};
    for (u32 i = 0; i < sizeof near / sizeof near[0]; i++)
      ys[ny++] = floor_y + near[i];
    for (int i = 1; i <= 10; i++)
      ys[ny++] = floor_y + 92 + (SCR_H - floor_y - 92) * i / 10;
  }
  /* each point worked out once (a row is in two strips), a row only
     against the lights that reach it, and the whole grid handed to libgl
     as arrays: at 9 tiles' shadows this was most of Home's frame */
  enum { NX = 64, PTS = NX + 1, ROWS = 64 + 2 };
  static float pos[ROWS * PTS][2];
  static u8 rgba[ROWS * PTS][4];
  /* the strips one after the other (a row in two of them), drawn with
     mxgl_MultiDraw: whole strips, without libgl's work for each triangle
     of an index list (that was 5 ms a frame) */
  static float spos[(ROWS - 1) * 2 * PTS][3]; /* x, y, 0 */
  static u8 srgba[(ROWS - 1) * 2 * PTS][4];
  static unsigned modes[ROWS];
  static int firsts[ROWS], counts[ROWS];
  int line = -1;
  float base[64];
  for (int j = 0; j < ny; j++) {
    float y = ys[j];
    if (floor_y >= 0 && y == floor_y)
      line = j;
    base[j] = (C_INK & 255);
    if (floor_y >= 0 && y >= floor_y) {
      float k = clampf((y - floor_y) / (SCR_H - floor_y), 0, 1);
      k = k * k * (3 - 2 * k);
      base[j] += ((float)(C_FLOOR & 255) - base[j]) * (1 - k) * floor_k;
    }
  }
  if (room_vu_on()) {
    /* VU1 adds the lights (room.vsm): the grid with each row's own colour */
    int n = 0;
    for (int j = 0; j + 1 < ny; j++) {
      modes[j] = GL_TRIANGLE_STRIP, firsts[j] = n, counts[j] = 2 * PTS;
      for (int i = 0; i < PTS; i++)
        for (int q = 0; q < 2; q++) {
          u8 b = (u8)(base[j + q] + (j == line ? 10 * floor_k : 0) + 0.5f);
          spos[n][0] = SCR_W * i / NX, spos[n][1] = ys[j + q], spos[n][2] = 0;
          srgba[n][0] = srgba[n][1] = srgba[n][2] = b, srgba[n][3] = 255;
          n++;
        }
    }
    room_vu_draw(modes, firsts, counts, ny - 1, spos, srgba);
    return;
  }
  for (int j = 0; j < ny; j++) {
    float y = ys[j];
    /* the lights this row is in, with their part of r^2 */
    int act[MAX_LIGHTS], na = 0;
    float dy2[MAX_LIGHTS];
    for (int l = 0; l < nlights; l++) {
      float dy = (y - lights[l].cy) / lights[l].ry;
      if (dy * dy < 1)
        dy2[na] = dy * dy, act[na++] = l;
    }
    for (int i = 0; i < PTS; i++) {
      float x = SCR_W * i / NX, rgb[3] = {base[j], base[j], base[j]};
      for (int m = 0; m < na; m++) {
        int l = act[m];
        float dx = (x - lights[l].cx) / lights[l].rx, r2 = dx * dx + dy2[m];
        if (r2 >= 1)
          continue;
        float f = (1 - r2) * (1 - r2) * lights[l].a;
        if (f < 0)
          rgb[0] *= 1 + f, rgb[1] *= 1 + f, rgb[2] *= 1 + f;
        else {
          u32 c = lights[l].rgb;
          rgb[0] += (c >> 16 & 255) * f, rgb[1] += (c >> 8 & 255) * f, rgb[2] += (c & 255) * f;
        }
      }
      int v = j * PTS + i;
      pos[v][0] = x, pos[v][1] = y;
      for (int c = 0; c < 3; c++)
        rgba[v][c] = bias((u32)clampf(rgb[c], 0, 250));
      rgba[v][3] = 255;
    }
  }
  /* the floor's edge, a faint line: the strip below it, its own two rows */
  int edge = (int)(10 * floor_k + 0.5f);
  if (line >= 0 && line + 1 < ny)
    for (int q = 0; q < 2; q++)
      for (int i = 0; i < PTS; i++) {
        int from = (line + q) * PTS + i, to = (ny + q) * PTS + i;
        pos[to][0] = pos[from][0], pos[to][1] = pos[from][1];
        for (int c = 0; c < 3; c++)
          rgba[to][c] = (u8)(rgba[from][c] + edge > 255 ? 255 : rgba[from][c] + edge);
        rgba[to][3] = 255;
      }
  int n = 0;
  for (int j = 0; j + 1 < ny; j++) {
    int a = j == line ? ny : j;
    modes[j] = GL_TRIANGLE_STRIP, firsts[j] = n, counts[j] = 2 * PTS;
    for (int i = 0; i < PTS; i++)
      for (int q = 0; q < 2; q++) {
        int v = (a + q) * PTS + i;
        spos[n][0] = pos[v][0], spos[n][1] = pos[v][1], spos[n][2] = 0;
        memcpy(srgba[n], rgba[v], 4);
        n++;
      }
  }
  glDisable(GL_BLEND);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, spos);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, srgba);
  mxgl_MultiDraw(modes, firsts, counts, ny - 1);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glEnable(GL_BLEND);
}

void d_begin_2d(void) {
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, SCR_W, SCR_H, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void d_rect(float x0, float y0, float x1, float y1, u32 top, u32 bottom) {
  glBegin(GL_QUADS);
  col(top), glVertex2f(x0, y0), glVertex2f(x1, y0);
  col(bottom), glVertex2f(x1, y1), glVertex2f(x0, y1);
  glEnd();
}

void d_rect_h(float x0, float y0, float x1, float y1, u32 left, u32 right) {
  glBegin(GL_QUADS);
  col(left), glVertex2f(x0, y0);
  col(right), glVertex2f(x1, y0), glVertex2f(x1, y1);
  col(left), glVertex2f(x0, y1);
  glEnd();
}

#define SEG 32
#define RINGS 7

/* an ellipse whose alpha falls off smoothly, (1 - r^2)^2, from a in the
   middle to 0 at its edge: rings of the falloff, so no edge shows */
static void soft_ellipse(float cx, float cy, float rx, float ry, u32 rgb, float a) {
  static float cs[SEG + 1], sn[SEG + 1];
  if (!cs[0])
    for (int i = 0; i <= SEG; i++)
      cs[i] = cosf(6.2831853f * i / SEG), sn[i] = sinf(6.2831853f * i / SEG);
  for (int r = 0; r < RINGS; r++) {
    float r0 = (float)r / RINGS, r1 = (float)(r + 1) / RINGS;
    float f0 = (1 - r0 * r0) * (1 - r0 * r0), f1 = (1 - r1 * r1) * (1 - r1 * r1);
    u32 c0 = argb(rgb, a * f0), c1 = argb(rgb, a * f1);
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= SEG; i++) {
      col(c0), glVertex2f(cx + cs[i] * rx * r0, cy + sn[i] * ry * r0);
      col(c1), glVertex2f(cx + cs[i] * rx * r1, cy + sn[i] * ry * r1);
    }
    glEnd();
  }
}

void d_glow(float cx, float cy, float rx, float ry, u32 c) {
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  soft_ellipse(cx, cy, rx, ry, c & 0xFFFFFFu, (float)(c >> 24) / 255.0f);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void d_cone(float cx, float y0, float y1, float rx, u32 c) {
  /* two halves, each bright along the middle and dark at its outer edge;
     the light fades towards the floor */
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  u32 none = c & 0xFFFFFFu, low = none | (((c >> 24) / 5) << 24);
  float top = rx * 0.18f;
  glBegin(GL_QUADS);
  for (int side = -1; side <= 1; side += 2) {
    col(none), glVertex2f(cx + side * top, y0);
    col(c), glVertex2f(cx, y0);
    col(low), glVertex2f(cx, y1);
    col(none), glVertex2f(cx + side * rx, y1);
  }
  glEnd();
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}


void d_shadow(float cx, float cy, float rx, float ry, float a) { soft_ellipse(cx, cy, rx, ry, 0x000000, a); }

/* ---- smooth edges ----
   The GS does not smooth a triangle's edges. The colour under a shape is
   known (the room, or a colour given), so a shape gets a rim of a pixel
   whose colour goes from its own to what is under it, per vertex: the GS
   interpolates it, and the edge is smooth without blending (opaque, so
   16-bit colour is dithered once). Shapes are rounded rectangles: pills,
   rings, cards. */
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

#define CSEG 10 /* segments of a corner */
#define OUTLINE (4 * (CSEG + 1))
/* the outline of a rounded rectangle, clockwise from the top left corner's
   start, with the outward normal of each point */
static int outline(float x0, float y0, float x1, float y1, float r, float *px, float *py, float *nx, float *ny) {
  static const float cx_sign[4] = {1, -1, -1, 1}, cy_sign[4] = {1, 1, -1, -1};
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
  (void)cx_sign, (void)cy_sign;
  return n;
}

/* a shape's part of ui_fade: the last of it, and fast. A light pill half
   way is a grey patch with no text to read on it, so text comes first and
   goes last */
static float shape_fade(void) {
  float f = clampf((ui_fade - 0.6f) / 0.4f, 0, 1);
  return f * f;
}
static void put(float x, float y, const float rgb[3]) {
  if (ui_fade < 0.999f) {
    /* fading: towards what is under it, still opaque */
    float u[3], f = shape_fade();
    under_at(x, y, u);
    for (int i = 0; i < 3; i++)
      u[i] += (rgb[i] - u[i]) * f;
    room_vertex(x, y, u);
  } else
    room_vertex(x, y, rgb);
}

/* fill: the shape's colour, or (glass >= 0) what is under it lighter by
   glass; line: an outline of a pixel at that colour and alpha (0: none).
   Rings from the inside out: the fill a pixel (half a pixel without a
   line) inside the edge, the line on the edge, what is under it a pixel
   (half a pixel) outside: the GS's interpolation smooths the edge. */
static void rrect(float x0, float y0, float x1, float y1, float r, u32 fill, float glass, u32 line) {
  /* faded out: nothing (drawn in the room's colour it would cover what
     is over the room there, a tile) */
  if (ui_fade < 0.999f && shape_fade() < 0.004f)
    return;
  x0 = snapx(x0), x1 = snapx(x1), y0 = snapy(y0), y1 = snapy(y1);
  if (r > (y1 - y0) * 0.5f)
    r = (y1 - y0) * 0.5f;
  if (r > (x1 - x0) * 0.5f)
    r = (x1 - x0) * 0.5f;
  float px[OUTLINE], py[OUTLINE], nx[OUTLINE], ny[OUTLINE];
  int n = outline(x0, y0, x1, y1, r, px, py, nx, ny);
  float la = (float)(line >> 24) / 255.0f;
  float lrgb[3] = {(float)(line >> 16 & 255), (float)(line >> 8 & 255), (float)(line & 255)};
  float d = la > 0 ? 1.0f : 0.5f, ex = d / px_x, ey = d / px_y;
  static float fin[OUTLINE][5], fout[OUTLINE][5], fmid[OUTLINE][5]; /* x, y, rgb */
  for (int k = 0; k < n; k++) {
    float xi = px[k] - nx[k] * ex, yi = py[k] - ny[k] * ey, xo = px[k] + nx[k] * ex, yo = py[k] + ny[k] * ey;
    fin[k][0] = xi, fin[k][1] = yi, fout[k][0] = xo, fout[k][1] = yo, fmid[k][0] = px[k], fmid[k][1] = py[k];
    if (glass >= 0) {
      under_at(xi, yi, fin[k] + 2);
      fin[k][2] += glass, fin[k][3] += glass, fin[k][4] += glass;
    } else
      fin[k][2] = (float)(fill >> 16 & 255), fin[k][3] = (float)(fill >> 8 & 255), fin[k][4] = (float)(fill & 255);
    under_at(xo, yo, fout[k] + 2);
    for (int j = 0; j < 3; j++)
      fmid[k][2 + j] = fin[k][2 + j] + (lrgb[j] - fin[k][2 + j]) * la;
  }
  glDisable(GL_BLEND);
  float c[3], cxm = (x0 + x1) * 0.5f, cym = (y0 + y1) * 0.5f;
  if (glass >= 0) {
    under_at(cxm, cym, c);
    c[0] += glass, c[1] += glass, c[2] += glass;
  } else
    c[0] = (float)(fill >> 16 & 255), c[1] = (float)(fill >> 8 & 255), c[2] = (float)(fill & 255);
  glBegin(GL_TRIANGLE_FAN);
  put(cxm, cym, c);
  for (int i = 0; i <= n; i++)
    put(fin[i % n][0], fin[i % n][1], fin[i % n] + 2);
  glEnd();
  float(*from)[5] = fin;
  if (la > 0) {
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= n; i++)
      put(fin[i % n][0], fin[i % n][1], fin[i % n] + 2), put(fmid[i % n][0], fmid[i % n][1], fmid[i % n] + 2);
    glEnd();
    from = fmid;
  }
  glBegin(GL_TRIANGLE_STRIP);
  for (int i = 0; i <= n; i++)
    put(from[i % n][0], from[i % n][1], from[i % n] + 2), put(fout[i % n][0], fout[i % n][1], fout[i % n] + 2);
  glEnd();
  glEnable(GL_BLEND);
}

void d_pill(float x0, float y0, float x1, float y1, u32 c) { rrect(x0, y0, x1, y1, (y1 - y0) * 0.5f, c, -1, 0); }
void d_pill_glass(float x0, float y0, float x1, float y1, float lighter, u32 line) {
  rrect(x0, y0, x1, y1, (y1 - y0) * 0.5f, 0, lighter, line);
}
void d_card(float x0, float y0, float x1, float y1, float r, u32 c) { rrect(x0, y0, x1, y1, r, c, -1, 0); }
void d_ring(float cx, float cy, float r, u32 line) { rrect(cx - r, cy - r, cx + r, cy + r, r, 0, 0, line); }

/* a button as in the mockups: a pill, a pad symbol and its label, both
   centred on the pill's middle by their x height */
float d_button(float x, float y, const char *sym, const char *label, int primary) {
  const float h = 44;
  float sw = d_text_w(FONT_BUTTON, sym), lw = d_text_w(FONT_BUTTON, label);
  float pl = 18, gap = 9, pr = 22;
  float w = pl + sw + gap + lw + pr;
  if (primary)
    d_pill(x, y, x + w, y + h, C_WHITE);
  else
    d_pill_glass(x, y, x + w, y + h, 12, argb(C_WHITE, 0.16f));
  u32 c = primary ? argb(C_INK, 1) : argb(C_WHITE, 1);
  float base = y + h * 0.5f + text_mid(FONT_BUTTON);
  d_text(FONT_BUTTON, x + pl, base, sym, c);
  d_text(FONT_BUTTON, x + pl + sw + gap, base, label, c);
  return w;
}
