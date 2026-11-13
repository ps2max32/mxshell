/* MXSHELL2: notifications as cards top right. The newest is lit, older ones
   smaller and dimmer below it; a card goes after 4 s, an urgent one stays
   until a button. The shell's own sources only (pads, devices, games that
   ended badly) until the kernel has events (PLAN.md, O2, O3); a history of
   the last ones is kept. While a game runs nothing is drawn (the game owns
   the GS). */
#include <string.h>

#include "pn.h"

#define SHOW 4
#define HIST 16
#define LIFE (4 * 60) /* frames */

struct note {
  int kind, urgent;
  char title[40], text[56];
  u32 born;
  struct spring in; /* 0 .. 1: sliding in */
  int gone;
};
static struct note notes[HIST];
static int nnotes;

static void copy(char *d, u32 n, const char *s) {
  u32 i = 0;
  for (; s && s[i] && i + 1 < n; i++)
    d[i] = s[i];
  d[i] = 0;
}

void toast(int kind, const char *title, const char *text, int urgent) {
  if (nnotes == HIST) {
    memmove(&notes[0], &notes[1], sizeof notes[0] * (HIST - 1));
    nnotes--;
  }
  struct note *n = &notes[nnotes++];
  memset(n, 0, sizeof *n);
  n->kind = kind, n->urgent = urgent, n->born = ticks;
  copy(n->title, sizeof n->title, title);
  copy(n->text, sizeof n->text, text);
}

void toast_step(void) {
  for (int i = 0; i < nnotes; i++) {
    struct note *n = &notes[i];
    if (!n->urgent && ticks - n->born > LIFE)
      n->gone = 1;
    spring_step(&n->in, n->gone ? 0.0f : 1.0f, W_LAYER, frame_dt);
  }
}

int toast_dismiss(void) {
  for (int i = nnotes - 1; i >= 0; i--)
    if (notes[i].urgent && !notes[i].gone) {
      notes[i].gone = 1;
      return 1;
    }
  return 0;
}

static void icon(int kind, float cx, float cy, u32 c, u32 card) {
  aa_under(card);
  d_ring(cx, cy, 20, argb(C_WHITE, 0.12f));
  aa_under_room();
  switch (kind) {
  case N_PAD:
    d_rect(cx - 10, cy - 5, cx + 10, cy + 3, c, c);
    d_rect(cx - 10, cy + 3, cx - 5, cy + 8, c, c);
    d_rect(cx + 5, cy + 3, cx + 10, cy + 8, c, c);
    break;
  case N_DEVICE:
    d_rect(cx - 5, cy - 10, cx + 5, cy - 4, c, c);
    d_rect(cx - 7, cy - 4, cx + 7, cy + 10, c, c);
    break;
  case N_WARN:
    d_rect(cx - 1.5f, cy - 10, cx + 1.5f, cy + 3, c, c);
    d_rect(cx - 1.5f, cy + 6, cx + 1.5f, cy + 9, c, c);
    break;
  default:
    d_rect(cx - 3, cy - 3, cx + 3, cy + 3, c, c);
    break;
  }
}

void toast_draw(void) {
  float y = 88;
  int shown = 0;
  for (int i = nnotes - 1; i >= 0 && shown < SHOW; i--) {
    struct note *n = &notes[i];
    float a = clampf(n->in.x, 0, 1);
    if (a < 0.01f)
      continue;
    float s = shown == 0 ? 1.0f : 0.97f - 0.03f * shown;
    float dim = shown == 0 ? 1.0f : 0.75f - 0.2f * shown;
    float w = 360 * s, h = 68 * s;
    float x1 = SCR_W - MARGIN + (1 - a) * 60, x0 = x1 - w;
    float fade = a * dim;
    /* opaque once in (16-bit colour: pn.h); the older ones darker */
    u32 cardc = shown == 0 ? 0x161719u : 0x121315u;
    if (a > 0.98f)
      d_card(x0, y, x1, y + h, 8, cardc);
    else
      d_rect(x0, y, x1, y + h, argb(cardc, a), argb(cardc, a));
    if (shown == 0)
      d_rect(x0 + 8, y, x1 - 8, y + 1, argb(C_LAMP, 0.55f * fade), argb(C_LAMP, 0.55f * fade));
    u32 ic = n->kind == N_WARN ? C_WARN : shown == 0 ? C_LAMP : C_DIM;
    if (a > 0.98f)
      icon(n->kind, x0 + 36 * s, y + h * 0.5f, argb(ic, fade), shown == 0 ? 0x161719u : 0x121315u);
    d_text_fit(FONT_BUTTON, FONT_SMALL, x0 + 70 * s, y + 30 * s, w - 86 * s, n->title, argb(C_WHITE, fade));
    d_text_fit(FONT_SMALL, FONT_SMALL, x0 + 70 * s, y + 54 * s, w - 86 * s, n->text, argb(C_DIM, fade));
    y += h + 10;
    shown++;
  }
}
