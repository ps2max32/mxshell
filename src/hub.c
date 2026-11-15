/* MXSHELL2: a game's hub. Its tile as a slab turned towards the light, its
   details from game.ini, where it is and how large, and its saves where
   game.ini names them (saves=<directory on the same device>). */
#include <mx/debug.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <string.h>

#include "pn.h"

static int hub_game = -1;
static u32 size_kb;
static int nsaves;
static char save_name[3][40];
static struct spring in;
static int leaving;
/* tests: hubk=<percent>, the way in held there */
float hub_pin = -1;
#define W_HUB 11.0f /* the tile's way to its place: ~360 ms */

/* the size of a directory, its files and those one level down */
static u32 dir_kb(const char *path, int depth) {
  int d = mx_fio_dopen(path);
  if (d < 0)
    return 0;
  char name[64], sub[128];
  u32 size, kb = 0;
  while (mx_fio_dread(d, name, sizeof name, &size) > 0) {
    if (name[0] == '.')
      continue;
    kb += (size + 1023) / 1024;
    if (depth > 0 && size == 0) {
      mx_snprintf(sub, sizeof sub, "%s/%s", path, name);
      kb += dir_kb(sub, depth - 1);
    }
  }
  mx_fio_dclose(d);
  return kb;
}

static void enter(int g) {
  hub_game = g;
  in.x = 0, in.v = 0, leaving = 0;
  char path[112];
  game_path(path, sizeof path, g, "");
  path[strlen(path) - 1] = 0;
  size_kb = dir_kb(path, 2);
  nsaves = 0;
  if (games[g].saves[0]) {
    game_path(path, sizeof path, g, games[g].saves);
    int d = mx_fio_dopen(path);
    if (d >= 0) {
      char name[64];
      u32 sz;
      while (mx_fio_dread(d, name, sizeof name, &sz) > 0)
        if (name[0] != '.' && nsaves < 3)
          strncpy(save_name[nsaves], name, 39), save_name[nsaves++][39] = 0;
      mx_fio_dclose(d);
    }
  }
}

/* text wrapped at width w, at most lines lines; returns the y after it */
static float para(float x, float y, float w, const char *s, int lines) {
  char line[128];
  float lh = text_line(FONT_BODY) + 4;
  while (*s && lines-- > 0) {
    u32 n = 0, cut = 0;
    while (s[n] && n < sizeof line - 1) {
      line[n] = s[n], line[n + 1] = 0;
      if (s[n] == ' ')
        cut = n;
      if (d_text_w(FONT_BODY, line) > w)
        break;
      n++;
    }
    if (s[n] && cut)
      n = cut;
    line[n] = 0;
    d_text(FONT_BODY, x, y, line, argb(C_DIM, 1));
    y += lh;
    s += n;
    while (*s == ' ')
      s++;
  }
  return y;
}

static void fact(float x, float y, float w, const char *label, const char *value) {
  d_label(x, y, label, argb(C_FAINT, 1));
  d_text_fit(FONT_BODY, FONT_SMALL, x, y + 28, w, value, argb(C_WHITE, 1));
}

void hub_draw(void) {
  int g = focus_game();
  if (g < 0) {
    screen = SCR_HOME;
    return;
  }
  if (g != hub_game)
    enter(g);
  spring_step(&in, leaving ? 0 : 1, W_HUB, frame_dt);
  if (hub_pin >= 0)
    in.x = hub_pin;
  if (leaving && in.x < 0.004f) {
    /* the tile is back on the shelf */
    screen = SCR_HOME, hub_game = -1;
    home_draw();
    return;
  }
  const struct game *gm = &games[g];
  float way = in.x;
  /* the details: once Home's text is gone */
  float k = clampf((way - 0.45f) / 0.55f, 0, 1);
  int wide = SCR_W > 1100;

  /* the slab, turned towards the light */
  struct tile_look t;
  memset(&t, 0, sizeof t);
  t.w = wide ? 262 : 206, t.h = wide ? 370 : 290; /* a DVD case */
  t.x = wide ? 120 : MARGIN, t.y = 150;
  t.turn = 24;

  /* the shelf going, the game's tile on its way here (home.c) */
  home_scene(way, &t);
  ui_fade = k;

  /* the details */
  float x = (wide ? 500 : t.x + t.w + 64) + (1 - k) * 20, x1 = SCR_W - MARGIN, y = 120;
  d_label(x, y, g == run_game ? "Paused" : gm->type[0] ? gm->type : "Game", argb(C_LAMP, 1));
  d_text_fit(FONT_DISP, FONT_TITLE, x - 2, y + 58, x1 - x, gm->title, argb(C_WHITE, 1));
  y += 96;
  if (gm->desc[0])
    y = para(x, y, x1 - x, gm->desc, 3);
  y += 14;
  float bx = x;
  int paused = g == run_game;
  bx += d_button(bx, y, SYM_CROSS, paused ? "Resume" : "Play", 1) + 12;
  if (paused)
    bx += d_button(bx, y, SYM_SQUARE, "Close", 0) + 12;
  y += 82;
  d_rect(x, y - 18, x1, y - 17, argb(C_WHITE, 0.08f), argb(C_WHITE, 0.08f));
  char v[64];
  float col = (x1 - x) / 3;
  fact(x, y + 8, col - 16, "Year", gm->year[0] ? gm->year : "-");
  if (size_kb >= 1024)
    mx_snprintf(v, sizeof v, "%u.%u MB", size_kb / 1024, (size_kb % 1024) * 10 / 1024);
  else
    mx_snprintf(v, sizeof v, "%u KB", size_kb);
  fact(x + col, y + 8, col - 16, "Size", v);
  game_path(v, sizeof v, g, "");
  v[strlen(v) - 1] = 0;
  fact(x + 2 * col, y + 8, col, "Where", v);
  y += 80;
  if (nsaves) {
    d_label(x, y, "Saves", argb(C_FAINT, 1));
    for (int i = 0; i < nsaves; i++)
      d_text(FONT_BODY, x, y + 30 + 28 * i, save_name[i], argb(C_WHITE, 1));
  } else
    fact(x, y, x1 - x, "Saves", gm->saves[0] ? "None yet" : "Kept by the game itself");

  static const char *const hints[] = {SYM_CROSS, "Play", SYM_CIRCLE, "Back", 0};
  static const char *const hints_paused[] = {SYM_CROSS, "Resume", SYM_SQUARE, "Close", SYM_CIRCLE, "Back", 0};
  hints_draw(paused ? hints_paused : hints);
  ui_fade = 1;
  status_draw();
}

void hub_input(void) {
  if (leaving)
    return;
  if (pad_pressed & MX_PAD_CIRCLE)
    leaving = 1;
  if (pad_pressed & MX_PAD_CROSS)
    screen = SCR_LAUNCH, hub_game = -1;
  else if (pad_pressed & MX_PAD_SQUARE && hub_game == run_game) {
    char m[48];
    strncpy(m, games[hub_game].title, sizeof m - 1), m[sizeof m - 1] = 0;
    game_close();
    toast(N_INFO, "Closed", m, 0);
  }
}
