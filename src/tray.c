/* MXSHELL2: the tray at the top right, on every screen of the shell. The
   players at the right: a ring per pad there in its light bar's colour
   (white for a pad without one), its player number in it and its battery
   as a bar under it (the lamp's colour while charging, orange when low,
   none for a controller port's pad). Left of them the devices: memory
   cards, USB drives, the hard disk, an SD card in MX4SIO; the one with the
   swap file in the lamp's colour. */
#include <mx/dev.h>
#include <mx/pad.h>
#include <string.h>

#include "pn.h"

static struct mx_pad_info pads[MX_PAD_PORTS];
static int pad_there[MX_PAD_PORTS], npads_known = -1;
static u32 pads_seen = ~0u;
static struct pdev devs[PDEV_MAX];
static int ndevs;
static u32 devs_seen = ~0u;

static void read_pads(void) {
  u32 g = mx_pad_gen();
  if (g == pads_seen && npads_known >= 0 && g)
    return;
  pads_seen = g, npads_known = 0;
  for (int p = 0; p < MX_PAD_PORTS; p++) {
    int r = mx_pad_info(p, &pads[p]);
    if (r < 0 && p < 2) { /* no kernel 28 (the standalone build): the two ports */
      memset(&pads[p], 0, sizeof pads[p]);
      pads[p].battery = 255;
      r = pad_on && mx_pad_connected(p);
    }
    pad_there[p] = r == 1;
  }
}

static u32 ring_rgb(const struct mx_pad_info *in) {
  if (in->kind != MX_PAD_KIND_DS4 && in->kind != MX_PAD_KIND_DS5)
    return 0;
  const u8 *c = in->rgb;
  u32 m = c[0] > c[1] ? c[0] : c[1];
  m = m > c[2] ? m : c[2];
  if (!m)
    return 0;
  return (u32)(c[0] * 220 / m) << 16 | (u32)(c[1] * 220 / m) << 8 | (u32)(c[2] * 220 / m);
}

/* a device's glyph, its middle at cx, its foot at y */
static void glyph(int kind, float cx, float y, u32 c) {
  struct surface s = {.glass = -1, .line = c, .a = 1, .hollow = 1, .r = 2};
  switch (kind) {
  case MX_DEV_USB:
    s.x0 = cx - 5, s.x1 = cx + 5, s.y0 = y - 13, s.y1 = y;
    surface_draw(&s);
    s.x0 = cx - 3, s.x1 = cx + 3, s.y0 = y - 18, s.y1 = y - 13, s.r = 1;
    surface_draw(&s);
    break;
  case MX_DEV_HDD:
    s.x0 = cx - 9, s.x1 = cx + 9, s.y0 = y - 13, s.y1 = y;
    surface_draw(&s);
    d_rect(cx + 4, y - 5, cx + 6, y - 3, c, c);
    break;
  case MX_DEV_MC:
    s.x0 = cx - 7, s.x1 = cx + 7, s.y0 = y - 17, s.y1 = y;
    surface_draw(&s);
    for (int i = -1; i <= 1; i++)
      d_rect(cx + i * 3.5f - 0.6f, y - 16, cx + i * 3.5f + 0.6f, y - 12, c, c);
    break;
  default: /* an SD card */
    s.x0 = cx - 6, s.x1 = cx + 6, s.y0 = y - 16, s.y1 = y;
    surface_draw(&s);
    d_rect(cx - 3, y - 12, cx + 3, y - 11, c, c);
    break;
  }
}

static const char *dev_short(const struct pdev *d) {
  switch (d->kind) {
  case MX_DEV_MC: return d->root[2] == '1' ? "MC2" : "MC1";
  case MX_DEV_USB: return "USB";
  case MX_DEV_HDD: return "HDD";
  case MX_DEV_MX4SIO: return "SD";
  default: return 0;
  }
}

void tray_draw(void) {
  read_pads();
  if (pdevs_gen() != devs_seen) {
    devs_seen = pdevs_gen();
    ndevs = pdevs_read(devs, PDEV_MAX, 0);
  }
  float x = SCR_W - MARGIN, cy = TOP - 6;
  /* the players, right to left */
  int any = 0;
  for (int p = MX_PAD_PORTS - 1; p >= 0; p--) {
    if (!pad_there[p])
      continue;
    const struct mx_pad_info *in = &pads[p];
    float cx = x - 15;
    u32 rgb = ring_rgb(in);
    d_ring(cx, cy, 15, rgb ? argb(rgb, 1) : argb(C_WHITE, 0.45f));
    char s[4] = {'P', (char)('1' + p), 0};
    float w = d_label_w(s) - 2;
    d_label(cx - w / 2, cy + text_mid(FONT_LABEL) * 1.15f, s, argb(C_WHITE, 1));
    if (in->battery != 255) {
      int charging = in->flags & MX_PAD_INFO_CHARGING;
      u32 bc = charging ? C_LAMP : in->battery <= 2 ? C_WARN : C_WHITE;
      d_bar(cx - 10, cy + 20, cx + 10, cy + 23, (float)in->battery / 10.0f, bc, 1);
    }
    x -= 42, any = 1;
  }
  if (!any) { /* no pad: a faint P1 ring, where one goes */
    d_ring(x - 15, cy, 15, argb(C_WHITE, 0.12f));
    float w = d_label_w("P1") - 2;
    d_label(x - 15 - w / 2, cy + text_mid(FONT_LABEL) * 1.15f, "P1", argb(C_FAINT, 1));
    x -= 42;
  }
  /* the devices, right to left, after a thin line */
  int shown = 0;
  for (int i = ndevs - 1; i >= 0; i--) {
    const char *n = dev_short(&devs[i]);
    if (!n)
      continue;
    if (!shown)
      d_rect(x - 2, cy - 15, x - 1, cy + 17, argb(C_WHITE, 0.12f), argb(C_WHITE, 0.12f)), x -= 16;
    int swap = devs[i].flags & MX_DEV_SWAP;
    u32 c = swap ? C_LAMP : C_DIM;
    float lw = d_label_w(n), w = lw > 20 ? lw : 20, cx = x - w / 2;
    glyph(devs[i].kind, cx, cy + 4, argb(c, swap ? 1 : 0.8f));
    d_label(cx - lw / 2, cy + 22, n, argb(swap ? C_LAMP : C_FAINT, 1));
    x -= w + 14, shown++;
  }
  /* the clock, when the time is known */
  char t[16];
  if (clock_text(t, sizeof t) == 0) {
    d_rect(x - 2, cy - 15, x - 1, cy + 17, argb(C_WHITE, 0.12f), argb(C_WHITE, 0.12f)), x -= 16;
    d_text(FONT_BODY, x - d_text_w(FONT_BODY, t), TOP, t, argb(C_DIM, 1));
  }
}
