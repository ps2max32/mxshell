/* MXSHELL2: Home, the shelf (the mockups' "Home A"). A row of tiles on a
   floor; the lit one has its caption under it, as a label in a museum. The
   light moves with the focus (a spring); the tiles beside it turn a little
   and rest in half-shadow. Layout in units (pn.h). */
#include <mx/debug.h>
#include <mx/pad.h>
#include <string.h>

#include "pn.h"

/* tiles as DVD cases, 1 : 1.41 */
#define BIG_W 236.0f
#define BIG_H 333.0f
#define SMALL_W 162.0f
#define SMALL_H 228.0f
#define GAP 28.0f

static int focus;
static struct spring fpos, shift;

int focus_game(void) { return focus < ngames ? focus : -1; }

/* the row: the games, then Settings */
static int ntiles(void) { return ngames + 1; }

void home_enter(void) {
  if (focus >= ntiles())
    focus = 0;
  fpos.x = (float)focus, fpos.v = 0;
}

static float lit_of(int i) {
  float d = fpos.x - (float)i;
  return clampf(1.0f - (d < 0 ? -d : d), 0, 1);
}

static void layout(int i, struct tile_look *t) {
  float x = MARGIN - shift.x;
  for (int k = 0; k < i; k++)
    x += SMALL_W + (BIG_W - SMALL_W) * lit_of(k) + GAP;
  float l = lit_of(i);
  memset(t, 0, sizeof *t);
  t->w = SMALL_W + (BIG_W - SMALL_W) * l;
  t->h = SMALL_H + (BIG_H - SMALL_H) * l;
  t->x = x, t->y = FLOOR_Y - t->h;
  t->turn = ((float)i < fpos.x ? 16.0f : -16.0f) * (1 - l);
  t->lit = l;
  /* the row fades out at the right edge */
  t->fade = clampf(1.0f - (x + t->w - (SCR_W - MARGIN * 0.5f)) / 120.0f, 0, 1);
  if (i < ngames) {
    t->base = games[i].base;
    t->tex = games[i].tex;
    t->tex_u = 1, t->tex_v = games[i].tex_v;
  } else
    t->base = 0x23262C;
}

void home_focus_rect(float r[4]) {
  struct tile_look t;
  layout(focus, &t);
  r[0] = t.x, r[1] = t.y, r[2] = t.w, r[3] = t.h;
}

u32 home_focus_colour(void) { return focus < ngames ? games[focus].base : 0x23262C; }

/* k: the tile on its way to the hub's slab (0 on the shelf), where its
   text has more room around it */
static void tile_text(int i, const struct tile_look *t, float k) {
  /* a cover speaks for itself: text only on a tile without one */
  if (t->fade < 0.05f || t->tex)
    return;
  float a = t->fade * (0.45f + 0.55f * t->lit);
  const char *name = i < ngames ? games[i].title : "Settings";
  int size = t->lit > 0.5f ? FONT_TITLE : FONT_TILE;
  float pad = 14 + 2 * t->lit + 4 * k;
  d_text_fit(size, FONT_BODY, t->x + pad, t->y + pad + (t->lit > 0.5f ? 26 : 20) + 4 * k, t->w - 2 * pad, name,
             argb(C_WHITE, a));
  const char *sub = i < ngames ? (games[i].type[0] ? games[i].type : "Game") : "System";
  d_label(t->x + 16 + 4 * k, t->y + t->h - 16 - 6 * k, sub, argb(C_WHITE, 0.7f * a));
}

static void caption(float x) {
  float y = 488;
  char meta[96];
  const char *kick, *title, *act;
  if (focus < ngames) {
    const struct game *g = &games[focus];
    char where[24];
    game_path(where, sizeof where, focus, "");
    char *colon = strchr(where, ':');
    if (colon)
      colon[1] = 0;
    kick = "Play";
    title = g->title;
    mx_snprintf(meta, sizeof meta, "%s%s%s%s%s", g->year, g->year[0] ? DOT : "", g->type,
                g->type[0] ? DOT : "", where);
    act = "Play";
  } else {
    kick = "System", title = "Settings", act = "Open";
    mx_snprintf(meta, sizeof meta, "Video" DOT "Network" DOT "Storage" DOT "Controllers");
  }
  if (x > SCR_W - MARGIN - 420)
    x = SCR_W - MARGIN - 420;
  d_label(x, y, kick, argb(C_LAMP, 1));
  d_text_fit(FONT_DISP, FONT_TITLE, x - 2, y + 54, SCR_W - MARGIN - x, title, argb(C_WHITE, 1));
  d_text(FONT_BODY, x, y + 86, meta, argb(C_DIM, 1));
  float bx = x;
  bx += d_button(bx, y + 104, SYM_CROSS, act, 1) + 12;
  if (focus < ngames)
    d_button(bx, y + 104, SYM_TRIANGLE, "Details", 0);
}

/* less of a tile than this is not drawn: a layer that faint only darkens
   16-bit colour */
#define FADE_MIN 0.06f

static float mixf(float a, float b, float k) { return a + (b - a) * k; }

void home_draw(void) { home_scene(0, 0); }

/* k > 0: the game's hub coming in (hub.c draws its details over this). The
   lit tile goes to the slab's place and turn; the other tiles, the floor,
   the shelf's lights and Home's text fade, the hub's light comes up. All
   in the one opaque room, as ever (pn.h) */
void home_scene(float k, const struct tile_look *slab) {
  float out = clampf(1 - 2.2f * k, 0, 1); /* the shelf's own things */
  int hub = k > 0.995f;
  spring_step(&fpos, (float)focus, W_FOCUS, frame_dt);
  int visible = (int)((SCR_W - 2 * MARGIN - BIG_W) / (SMALL_W + GAP)) + 1;
  int over = focus - (visible - 2);
  spring_step(&shift, over > 0 ? (float)over * (SMALL_W + GAP) : 0.0f, W_FOCUS, frame_dt);

  int n = ntiles();
  struct tile_look t;
  /* where the light is: the middle of the lit tiles, weighted */
  float lxp = 0, wsum = 0, fx = MARGIN;
  for (int i = 0; i < n; i++) {
    layout(i, &t);
    lxp += (t.x + t.w * 0.5f) * t.lit, wsum += t.lit;
    if (t.lit > 0.5f)
      fx = t.x;
  }
  lxp = wsum > 0 ? lxp / wsum : MARGIN + BIG_W / 2;
  /* the lit tile, on the shelf or on its way */
  struct tile_look ft;
  layout(focus, &ft);
  if (slab) {
    ft.x = mixf(ft.x, slab->x, k), ft.y = mixf(ft.y, slab->y, k);
    ft.w = mixf(ft.w, slab->w, k), ft.h = mixf(ft.h, slab->h, k);
    ft.turn = mixf(ft.turn, slab->turn, k), ft.lit = mixf(ft.lit, 1, k), ft.fade = mixf(ft.fade, 1, k);
  }

  /* the room: the floor, the cone of light from above, its pool on the
     floor, the tiles' shadows */
  room_begin(hub ? -1 : FLOOR_Y);
  room_floor(1 - k);
  if (!hub) {
    room_light(lxp, 0, 340, FLOOR_Y + 50, 0.28f * (1 - k));
    room_light(lxp, FLOOR_Y + 2, 300, 44, 0.34f * (1 - k));
  }
  if (slab)
    room_light(slab->x + slab->w * 0.4f, 220, 420, 360, 0.08f * k);
  for (int i = 0; i < n && !hub; i++) {
    layout(i, &t);
    t.fade *= out;
    if (t.fade > FADE_MIN)
      room_shadow(t.x + t.w * 0.5f, FLOOR_Y + 4, t.w * 0.62f, 20 + 12 * t.lit, (0.35f + 0.3f * t.lit) * t.fade);
  }
  d_begin_2d();
  room_draw();

  /* the tiles, their reflections first */
  d_begin_3d();
  light_at(slab ? mixf(lxp, slab->x, k) : lxp, 0);
  for (int i = 0; i < n && !hub; i++) {
    layout(i, &t);
    if (i == focus)
      t = ft; /* its reflection goes with it, and fades */
    t.fade *= out;
    if (t.fade > FADE_MIN)
      d_tile_reflection(&t);
  }
  for (int i = 0; i < n && !hub; i++) {
    layout(i, &t);
    t.fade *= out;
    if (t.fade > FADE_MIN && i != focus)
      d_tile(&t);
  }
  d_tile(&ft);
  d_begin_2d();
  for (int i = 0; i < n && !hub; i++) {
    layout(i, &t);
    t.fade *= out;
    if (i != focus)
      tile_text(i, &t, 0);
  }
  tile_text(focus, &ft, k);
  if (k > 0.4f)
    return;
  /* Home's own text: gone before the hub's comes */
  ui_fade = clampf(1 - 2.5f * k, 0, 1);
  caption(fx);

  static const char *const hints_play[] = {SYM_CROSS, "Play", SYM_TRIANGLE, "Details", "R1", "Settings", 0};
  static const char *const hints_sys[] = {SYM_CROSS, "Open", 0};
  hints_draw(focus < ngames ? hints_play : hints_sys);
  ui_fade = 1;
  if (!slab)
    status_draw();
}

void home_input(void) {
  int n = ntiles();
  if (pad_pressed & MX_PAD_LEFT && focus > 0)
    focus--;
  if (pad_pressed & MX_PAD_RIGHT && focus + 1 < n)
    focus++;
  if (pad_pressed & MX_PAD_CROSS) {
    if (focus < ngames)
      screen = SCR_LAUNCH;
    else
      settings_enter();
  }
  if (pad_pressed & MX_PAD_TRIANGLE && focus < ngames)
    screen = SCR_HUB;
  if (pad_pressed & MX_PAD_R1)
    settings_enter();
}
