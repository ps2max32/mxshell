/* MXSHELL2: notifications as cards top right. The newest is lit, older ones
   smaller and dimmer below it; a card goes after 4 s, an urgent one stays
   until a button or until the kernel says it is over. They come from the
   kernel's ring (<mx/notify.h>: devices, the swap file, controllers,
   crashes, other programs; notes_poll) and from the shell itself (a game
   closed, a setting). A card with an action (the Storage page, ...) shows
   triangle, Open. While a game runs nothing is drawn (the game owns the
   GS): what came meanwhile is shown when the shell is in front again.

   Quiet at the start: what the kernel says about the devices, controllers
   and swap file that are there when the PS2 starts is not shown (during
   the boot screen and BOOT_QUIET after it), only what comes later. The
   swap file's state is not a card of the kernel's but the shell's own
   (ram_step): while there is no swap file a card stays, with the memory in
   use, so the user sees it run out. */
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/notify.h>
#include <mx/vm.h>
#include <string.h>

#include "pn.h"

#define SHOW 4
#define HIST 16
#define LIFE (4 * 60) /* frames */
#define BOOT_QUIET (5 * 60) /* frames after the boot screen */

struct note {
  int kind, urgent, action;
  u32 id; /* the kernel's, 0: the shell's own */
  char title[MX_NOTIFY_TITLE], text[MX_NOTIFY_TEXT];
  u32 born;
  struct spring in; /* 0 .. 1: sliding in */
  int gone;
  float bar; /* the memory in use, 0 .. 1; -1: no bar */
};
static struct note notes[HIST];
static int nnotes;
static struct note ram = {.kind = N_WARN, .urgent = 1, .action = MX_NOTIFY_ACTION_SWAP, .bar = -1};
static int ram_on;
static u32 quiet_until = ~0u; /* ticks: the start's news not shown before */

static void copy(char *d, u32 n, const char *s) {
  u32 i = 0;
  for (; s && s[i] && i + 1 < n; i++)
    d[i] = s[i];
  d[i] = 0;
}

static struct note *add(void) {
  if (nnotes == HIST) {
    memmove(&notes[0], &notes[1], sizeof notes[0] * (HIST - 1));
    nnotes--;
  }
  struct note *n = &notes[nnotes++];
  memset(n, 0, sizeof *n);
  n->born = ticks, n->bar = -1;
  return n;
}

void toast(int kind, const char *title, const char *text, int urgent) {
  struct note *n = add();
  n->kind = kind, n->urgent = urgent;
  copy(n->title, sizeof n->title, title);
  copy(n->text, sizeof n->text, text);
}

/* one of the kernel's: a new card, or the change of one shown (its urgency
   over: it goes like any other) */
static void from_kernel(const struct mx_notify *k, int show) {
  struct note *n = 0;
  for (int i = 0; i < nnotes; i++)
    if (notes[i].id == k->id)
      n = &notes[i];
  if (n && !k->urgent && n->urgent) {
    n->urgent = 0, n->born = ticks;
    return;
  }
  if (n || !show)
    return;
  /* the swap file in use, or none: the shell's own card (ram_step) */
  if (k->kind == MX_NOTIFY_SWAP && k->urgent) {
    struct mx_swap_info si;
    if (mx_swap_info(&si) == 0 && (si.state == MX_SWAP_ATTACHED || si.state == MX_SWAP_NONE))
      return;
  }
  /* the start's devices, controllers and swap file */
  if (ticks < quiet_until && !k->urgent &&
      (k->kind == MX_NOTIFY_DEVICE || k->kind == MX_NOTIFY_PAD || k->kind == MX_NOTIFY_SWAP))
    return;
  n = add();
  n->id = k->id, n->urgent = k->urgent, n->action = k->action;
  n->kind = k->kind == MX_NOTIFY_PAD ? N_PAD : k->kind == MX_NOTIFY_DEVICE ? N_DEVICE
          : k->kind == MX_NOTIFY_CRASH || k->urgent ? N_WARN : k->kind == MX_NOTIFY_SWAP ? N_DEVICE : N_INFO;
  copy(n->title, sizeof n->title, k->title);
  copy(n->text, sizeof n->text, k->text);
}

/* the kernel's ring, read when its counter moved: what is new or changed.
   The first time (the shell started), and after a game, only the last
   SHOW of them as cards, and every urgent one */
static u32 seen_seq;
void notes_poll(void) {
  u32 seq = mx_notify_seq();
  if (seq == seen_seq || (int)seq <= 0)
    return;
  struct mx_notify k[HIST];
  int n = mx_notify_read(seen_seq, k, HIST);
  seen_seq = seq;
  for (int i = 0; i < n; i++)
    from_kernel(&k[i], k[i].urgent || i >= n - SHOW);
}

void notes_boot_done(void) {
  if (quiet_until == ~0u)
    quiet_until = ticks + BOOT_QUIET;
}

/* the swap file's card: there while MXOS has none in use, with the memory
   in use (the kernel's VM: every program's) */
void ram_step(void) {
  if (ticks % 30 == 0) {
    struct mx_swap_info si;
    struct mx_vm_stats vs;
    ram_on = 0;
    if (mx_swap_info(&si) == 0 && si.state != MX_SWAP_ATTACHED && si.state != MX_SWAP_CREATING) {
      mx_vm_stats(&vs);
      ram_on = vs.frames > 0;
    }
    if (ram_on) {
      u32 used = vs.frames - vs.free_frames;
      char a[16], b[16];
      pdev_bytes(a, sizeof a, (u64)used * MX_VM_PAGE);
      pdev_bytes(b, sizeof b, (u64)vs.frames * MX_VM_PAGE);
      ram.bar = (float)used / (float)vs.frames;
      copy(ram.title, sizeof ram.title,
           si.state == MX_SWAP_WAITING ? "Swap file not there"
           : si.state == MX_SWAP_FAILED ? "Swap file failed" : "No swap file");
      mx_snprintf(ram.text, sizeof ram.text, "%s of %s memory in use. Programs end when it runs out.", a, b);
    }
  }
  spring_step(&ram.in, ram_on && quiet_until != ~0u ? 1.0f : 0.0f, W_LAYER, frame_dt);
}

/* the lit card: the newest note there, else the swap file's */
static struct note *lit(void) {
  for (int i = nnotes - 1; i >= 0; i--)
    if (!notes[i].gone && notes[i].in.x > 0.5f)
      return &notes[i];
  return ram_on && ram.in.x > 0.5f ? &ram : 0;
}

/* the action of the lit card (MX_NOTIFY_ACTION_*), 0 none */
int toast_action(void) {
  struct note *n = lit();
  return n ? n->action : 0;
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

/* the lit card taken away (its action was chosen); the swap file's stays */
void toast_done(void) {
  struct note *n = lit();
  if (n && n != &ram)
    n->gone = 1;
}

/* text in at most two lines of w: where the first ends (a space), or its length */
static int split(const char *s, float w) {
  char b[MX_NOTIFY_TEXT];
  int len = (int)strlen(s), best = len;
  if (d_text_w(FONT_SMALL, s) <= w)
    return len;
  for (int i = 1; i < len; i++)
    if (s[i] == ' ') {
      memcpy(b, s, (u32)i), b[i] = 0;
      if (d_text_w(FONT_SMALL, b) > w)
        break;
      best = i;
    }
  return best;
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
  /* the newest first, the swap file's card under them */
  for (int i = nnotes - 1; i >= -1 && shown < SHOW; i--) {
    struct note *n = i >= 0 ? &notes[i] : &ram;
    float a = clampf(n->in.x, 0, 1);
    if (a < 0.01f)
      continue;
    float s = shown == 0 ? 1.0f : 0.97f - 0.03f * shown;
    float dim = shown == 0 ? 1.0f : 0.75f - 0.2f * shown;
    float w = 400 * s, tw = w - 86 * s;
    int cut = split(n->text, tw), two = n->text[cut] != 0;
    float tb = (two ? 90 : 68) * s; /* the text's bottom */
    float h = tb + (n->bar >= 0 ? 14 * s : 0) + (n->action && shown == 0 ? 24 * s : 0);
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
    char l1[MX_NOTIFY_TEXT];
    memcpy(l1, n->text, (u32)cut), l1[cut] = 0;
    d_text_fit(FONT_SMALL, FONT_SMALL, x0 + 70 * s, y + 54 * s, tw, l1, argb(C_DIM, fade));
    if (two)
      d_text_fit(FONT_SMALL, FONT_SMALL, x0 + 70 * s, y + 76 * s, tw, n->text + cut + 1, argb(C_DIM, fade));
    if (n->bar >= 0) {
      /* the memory in use: amber from 85 % */
      float bx0 = x0 + 70 * s, bx1 = x1 - 16 * s, by = y + tb - 4 * s, bw = (bx1 - bx0) * clampf(n->bar, 0, 1);
      u32 bc = n->bar >= 0.85f ? C_WARN : C_LAMP;
      d_rect(bx0, by, bx1, by + 4 * s, argb(C_WHITE, 0.12f * fade), argb(C_WHITE, 0.12f * fade));
      d_rect(bx0, by, bx0 + bw, by + 4 * s, argb(bc, fade), argb(bc, fade));
    }
    if (n->action && shown == 0) {
      float ay = y + h - 14 * s;
      float ax = d_text(FONT_BODY, x0 + 70 * s, ay, SYM_TRIANGLE, argb(C_LAMP, fade));
      d_text(FONT_SMALL, ax + 8, ay, "Open", argb(C_LAMP, fade));
    }
    y += h + 10;
    shown++;
  }
}
