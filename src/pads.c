/* MXSHELL2: the controllers (kernel version 28, <mx/pad.h>), one page for
   Settings › Controllers and for the game menu: the pads there (cross: a
   pad's own page with its state, light bar colour, motors, disconnect and
   forget), then what is for all of them (pairing over Bluetooth, the light
   bars' brightness, turning idle pads off, the paired pads not there). */
#include <mx/debug.h>
#include <mx/pad.h>
#include <mx/setting.h>
#include <string.h>

#include "pn.h"

#define ROW 54.0f
static int item, active;
static int pr_n; /* the rows of the list */
static int pad_open = -1; /* the port whose own page is open, -1 the list */
static u32 rumble_until;

/* ---- Controllers (kernel version 28, <mx/pad.h>): the pads there, with
   their kind, link, battery and light; pairing over Bluetooth; the light
   bars' brightness and turning off; the paired pads not there ---- */
enum { PR_PAD, PR_PAIR, PR_BRIGHT, PR_IDLE, PR_PAIRED };
#define PR_MAX (MX_PAD_PORTS + 3 + 8)
static struct {
  u8 type, port;
  struct mx_pad_info in;
  struct mx_pad_paired pd;
} pr[PR_MAX];
static u32 pr_gen = ~0u;
static struct mx_pad_pairing pairing;

static void pr_read(void) {
  if (mx_pad_gen() == pr_gen && pr_n)
    return;
  pr_gen = mx_pad_gen();
  mx_pad_pair_state(&pairing);
  pr_n = 0;
  for (int p = 0; p < MX_PAD_PORTS; p++) {
    struct mx_pad_info in;
    if (mx_pad_info(p, &in) == 1)
      pr[pr_n].type = PR_PAD, pr[pr_n].port = (u8)p, pr[pr_n++].in = in;
  }
  pr[pr_n++].type = PR_PAIR;
  pr[pr_n++].type = PR_BRIGHT;
  pr[pr_n++].type = PR_IDLE;
  struct mx_pad_paired pd[8];
  int n = mx_pad_paired(pd, 8);
  for (int i = 0; i < n; i++)
    if (!pd[i].connected)
      pr[pr_n].type = PR_PAIRED, pr[pr_n++].pd = pd[i];
}

static const char *pad_kind(int k) {
  switch (k) {
  case MX_PAD_KIND_DS3: return "DualShock 3";
  case MX_PAD_KIND_DS4: return "DualShock 4";
  case MX_PAD_KIND_DS5: return "DualSense";
  case MX_PAD_KIND_PS2: return "Controller";
  default: return "Controller";
  }
}
static int has_light(int k) { return k == MX_PAD_KIND_DS4 || k == MX_PAD_KIND_DS5; }

/* the light bar's colours: the player's (the setting removed), then these */
static const struct {
  const char *name, *hex;
} colours[] = {{"Player", ""}, {"Amber", "e9a040"}, {"Red", "e02020"}, {"Green", "20c040"},
               {"Blue", "2040e0"}, {"Purple", "9030d0"}, {"White", "ffffff"}};
#define NCOLOURS (int)(sizeof colours / sizeof colours[0])
static int colour_of(u32 id) {
  char key[MX_SETTING_KEY];
  mx_snprintf(key, sizeof key, "pad.%08x.colour", id);
  const char *v = cfg_get(key, "");
  for (int i = 1; i < NCOLOURS; i++)
    if (!strcmp(v, colours[i].hex))
      return i;
  return 0;
}
static void colour_set(u32 id, int c) {
  char key[MX_SETTING_KEY];
  mx_snprintf(key, sizeof key, "pad.%08x.colour", id);
  cfg_set(key, colours[c].hex);
  cfg_save();
}

/* brightness and turning off: the choices and the settings' values */
static const char *const bright_l[] = {"Off", "Low", "Medium", "High"};
static const char *const bright_v[] = {"0", "25", "60", "100"};
static const char *const idle_l[] = {"Never", "10 m", "30 m", "60 m"};
static const char *const idle_v[] = {"0", "10", "30", "60"};
static int choice_of(const char *key, const char *def, const char *const *v, int n) {
  const char *c = cfg_get(key, def);
  for (int i = 0; i < n; i++)
    if (!strcmp(c, v[i]))
      return i;
  return 0;
}

static void battery(float x, float mid, int level, int charging) {
  /* five steps of two levels, amber when low */
  float w = 30, h = 12;
  surface_draw(&(struct surface){.x0 = x, .y0 = mid - h / 2, .x1 = x + w, .y1 = mid + h / 2, .r = 3, .glass = -1,
                                 .line = argb(C_WHITE, 0.45f), .a = 1, .hollow = 1});
  d_rect(x + w + 1, mid - 3, x + w + 3, mid + 3, argb(C_WHITE, 0.45f), argb(C_WHITE, 0.45f));
  int steps = (level + 1) / 2;
  u32 c = charging ? C_LAMP : level <= 2 ? C_WARN : C_WHITE;
  for (int i = 0; i < steps && i < 5; i++)
    d_rect(x + 3 + i * 5, mid - h / 2 + 3, x + 7 + i * 5, mid + h / 2 - 3, argb(c, 0.9f), argb(c, 0.9f));
}

/* the paired entry of a pad there (its id), or -1 */
static int paired_of(u32 id) {
  struct mx_pad_paired pd[8];
  int n = mx_pad_paired(pd, 8);
  for (int i = 0; i < n && id; i++)
    if (pd[i].id == id)
      return i;
  return -1;
}

/* the light bar as it is, bright enough to see its colour */
static u32 light_rgb(const u8 *c) {
  u32 m = c[0] > c[1] ? c[0] : c[1];
  m = m > c[2] ? m : c[2];
  if (!m)
    return 0;
  return (u32)(c[0] * 220 / m) << 16 | (u32)(c[1] * 220 / m) << 8 | (u32)(c[2] * 220 / m);
}

static const char *const link_name[] = {"Controller port", "USB", "Bluetooth"};

/* ---- one pad's own page: its state, its light, the motors, disconnect, forget ---- */
enum { PD_STATE, PD_COLOUR, PD_MOTORS, PD_DISCONNECT, PD_FORGET };
static struct mx_pad_info pad_in;
static int pd_rows[5], pd_n;

static void pd_build(void) {
  pd_n = 0;
  if (pad_open < 0 || mx_pad_info(pad_open, &pad_in) != 1) {
    pad_open = -1;
    return;
  }
  pd_rows[pd_n++] = PD_STATE;
  if (has_light(pad_in.kind) && pad_in.id)
    pd_rows[pd_n++] = PD_COLOUR;
  pd_rows[pd_n++] = PD_MOTORS;
  if (pad_in.link == MX_PAD_LINK_BT)
    pd_rows[pd_n++] = PD_DISCONNECT;
  if (paired_of(pad_in.id) >= 0)
    pd_rows[pd_n++] = PD_FORGET;
}

static void page_pad(float x0, float x1, float y) {
  pd_build();
  if (pad_open < 0)
    return;
  char l[48], v[64];
  mx_snprintf(l, sizeof l, "Player %d" DOT "%s", pad_open + 1, pad_kind(pad_in.kind));
  d_label(x0, y - 44, l, argb(C_LAMP, 1));
  for (int i = 0; i < pd_n; i++) {
    float yy = y + ROW * i, mid = yy - text_mid(FONT_BODY);
    int chosen = active && item == i;
    switch (pd_rows[i]) {
    case PD_STATE: {
      int n = mx_snprintf(v, sizeof v, "%s", link_name[pad_in.link < 3 ? pad_in.link : 0]);
      if (pad_in.link == MX_PAD_LINK_PORT)
        mx_snprintf(v + n, sizeof v - (u32)n, " %d", pad_in.hw + 1);
      else if (pad_in.battery != 255)
        mx_snprintf(v + n, sizeof v - (u32)n, DOT "battery %d %%%s", pad_in.battery * 10,
                    pad_in.flags & MX_PAD_INFO_CHARGING ? ", charging" : "");
      row_line(x0, x1, yy, "Connected", v, chosen, 1);
      break;
    }
    case PD_COLOUR: {
      int c = colour_of(pad_in.id);
      row_line(x0, x1, yy, "Light bar", 0, chosen, 1);
      float tw = d_text_w(FONT_BODY, colours[c].name);
      d_text(FONT_BODY, x1 - tw, yy, colours[c].name, argb(chosen ? C_WHITE : C_DIM, 1));
      u32 rgb = light_rgb(pad_in.rgb);
      if (rgb)
        d_pill(x1 - tw - 34, mid - 8, x1 - tw - 14, mid + 8, rgb);
      break;
    }
    case PD_MOTORS:
      row_line(x0, x1, yy, "Motors", ticks < rumble_until ? "Rumbling" : SYM_CROSS "  Try them", chosen, 1);
      break;
    case PD_DISCONNECT:
      row_line(x0, x1, yy, "Disconnect", SYM_CROSS "  It turns off", chosen, 1);
      break;
    case PD_FORGET:
      row_line(x0, x1, yy, "Forget", SYM_CROSS "  Pair it again to use it", chosen, 1);
      break;
    }
  }
}

/* ---- the Controllers page: the pads there (cross: a pad's own page),
   then what is for all of them ---- */
#define GROUP_GAP 44.0f
static float pr_y(int i, float y) {
  return y + ROW * i + (pr[i].type != PR_PAD ? GROUP_GAP : 0);
}

void pads_draw(float x0, float x1, float y, int act, const char *crumb) {
  active = act;
  if (ticks >= rumble_until && rumble_until) {
    for (int i = 0; i < MX_PAD_PORTS; i++)
      mx_pad_rumble(i, 0, 0);
    rumble_until = 0;
  }
  if (pad_open < 0)
    d_label(x0, y - 40, crumb ? crumb : "Connected", argb(C_LAMP, 1));
  if (pad_open >= 0) {
    page_pad(x0, x1, y);
    if (pad_open >= 0)
      return;
    item = 0; /* it went: the list again */
  }
  pr_read();
  mx_pad_pair_state(&pairing);
  int npads = 0;
  for (int i = 0; i < pr_n; i++) {
    float yy = pr_y(i, y), mid = yy - text_mid(FONT_BODY);
    int chosen = active && item == i;
    char l[48], v[64];
    if (pr[i].type != PR_PAD && (i == 0 || pr[i - 1].type == PR_PAD)) {
      if (!npads)
        row_note(x0, y - 10, "No controller: plug one in, or pair one over Bluetooth.");
      d_label(x0, yy - 50, "All controllers", argb(C_LAMP, 1));
    }
    switch (pr[i].type) {
    case PR_PAD: {
      const struct mx_pad_info *in = &pr[i].in;
      npads++;
      mx_snprintf(l, sizeof l, "P%d" DOT "%s", pr[i].port + 1, pad_kind(in->kind));
      if (in->link == MX_PAD_LINK_PORT)
        mx_snprintf(v, sizeof v, "Port %d", in->hw + 1);
      else
        mx_snprintf(v, sizeof v, "%s%s", link_name[in->link < 3 ? in->link : 0],
                    in->flags & MX_PAD_INFO_CHARGING ? DOT "charging" : "");
      row_line(x0, x1, yy, l, 0, chosen, 1);
      /* at the right: the light's colour, the battery, the link */
      float xr = x1;
      if (in->battery != 255) {
        xr -= 34;
        battery(xr, mid, in->battery, in->flags & MX_PAD_INFO_CHARGING);
        xr -= 14;
      }
      u32 rgb = has_light(in->kind) ? light_rgb(in->rgb) : 0;
      if (rgb) {
        d_pill(xr - 14, mid - 7, xr, mid + 7, rgb);
        xr -= 26;
      }
      float vw = d_text_w(FONT_SMALL, v);
      d_text(FONT_SMALL, xr - vw, yy, v, argb(chosen ? C_WHITE : C_DIM, 1));
      break;
    }
    case PR_PAIR: {
      static const char *const st[] = {SYM_CROSS "  Pair a controller", "Looking: hold Share or Create with PS",
                                       "Pairing", "Paired", "None found", "Did not pair",
                                       "No Bluetooth adapter"};
      const char *val = st[pairing.state < 7 ? pairing.state : 0];
      if (pairing.state == MX_PAD_PAIR_DONE || pairing.state == MX_PAD_PAIR_CONNECTING) {
        mx_snprintf(v, sizeof v, "%s %s", pairing.state == MX_PAD_PAIR_DONE ? "Paired:" : "Pairing", pad_kind(pairing.kind));
        val = v;
      }
      row_line(x0, x1, yy, "Bluetooth", val, chosen, pairing.state != MX_PAD_PAIR_NO_DONGLE);
      break;
    }
    case PR_BRIGHT:
      row_line(x0, x1, yy, "Light bars", 0, chosen, 1);
      row_seg(x1, yy, bright_l, 4, choice_of("pad.brightness", "25", bright_v, 4), chosen);
      break;
    case PR_IDLE:
      row_line(x0, x1, yy, "Idle, off after", 0, chosen, 1);
      row_seg(x1, yy, idle_l, 4, choice_of("pad.idle_off", "0", idle_v, 4), chosen);
      break;
    case PR_PAIRED: {
      const u8 *a = pr[i].pd.addr;
      mx_snprintf(l, sizeof l, "%s" DOT "paired", pad_kind(pr[i].pd.kind));
      mx_snprintf(v, sizeof v, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
      row_line(x0, x1, yy, l, v, chosen, 0);
      break;
    }
    }
  }
}

/* the Controllers page's hints, for the row chosen */
const char *const *pads_hints(const char *back) {
  static const char *h[12];
  int n = 0;
  if (pad_open >= 0) {
    if (item < pd_n)
      switch (pd_rows[item]) {
      case PD_COLOUR: h[n++] = "L R", h[n++] = "Colour"; break;
      case PD_MOTORS: h[n++] = SYM_CROSS, h[n++] = "Try"; break;
      case PD_DISCONNECT: h[n++] = SYM_CROSS, h[n++] = "Disconnect"; break;
      case PD_FORGET: h[n++] = SYM_CROSS, h[n++] = "Forget"; break;
      }
  } else if (item < pr_n)
    switch (pr[item].type) {
    case PR_PAD: h[n++] = SYM_CROSS, h[n++] = "Its settings"; break;
    case PR_PAIR:
      h[n++] = SYM_CROSS;
      h[n++] = pairing.state == MX_PAD_PAIR_SEARCHING || pairing.state == MX_PAD_PAIR_CONNECTING ? "Stop" : "Pair";
      break;
    case PR_BRIGHT:
    case PR_IDLE: h[n++] = "L R", h[n++] = "Change"; break;
    case PR_PAIRED: h[n++] = SYM_SQUARE, h[n++] = "Forget"; break;
    }
  h[n++] = SYM_CIRCLE, h[n++] = pad_open >= 0 ? "Back" : back, h[n] = 0;
  return h;
}

static int pads_items(void) { return pad_open >= 0 ? pd_n : pr_n; }

void pads_enter(void) { pad_open = -1, item = 0, pr_gen = ~0u; }

/* a pad's page: its buttons */
static void pad_input(u32 p) {
  if (item >= pd_n)
    return;
  int d = p & MX_PAD_RIGHT ? 1 : p & MX_PAD_LEFT ? -1 : 0, x = p & MX_PAD_CROSS;
  switch (pd_rows[item]) {
  case PD_COLOUR:
    if (d || x)
      colour_set(pad_in.id, (colour_of(pad_in.id) + (d ? d : 1) + NCOLOURS) % NCOLOURS);
    break;
  case PD_MOTORS:
    if (x && mx_pad_rumble(pad_open, 1, 160) >= 0)
      rumble_until = ticks + 30;
    break;
  case PD_DISCONNECT:
    if (x && mx_pad_disconnect(pad_open) == 0) {
      toast(N_PAD, "Disconnected", pad_kind(pad_in.kind), 0);
      pad_open = -1, item = 0;
    }
    break;
  case PD_FORGET:
    if (x && mx_pad_forget(pad_in.id) == 0) {
      toast(N_PAD, "Forgotten", pad_kind(pad_in.kind), 0);
      pad_open = -1, item = 0;
    }
    break;
  }
}

/* the Controllers page's buttons: 0 when circle leaves it */
int pads_input(u32 p) {
  pr_read();
  if (pad_open >= 0)
    pd_build();
  if (p & MX_PAD_UP && item > 0)
    item--;
  if (p & MX_PAD_DOWN && item + 1 < pads_items())
    item++;
  if (pad_open >= 0) {
    if (p & MX_PAD_CIRCLE) {
      /* back to the list, on that pad's row */
      for (int i = 0; i < pr_n; i++)
        if (pr[i].type == PR_PAD && pr[i].port == pad_open)
          item = i;
      pad_open = -1;
    } else
      pad_input(p);
    return 1;
  }
  if (p & MX_PAD_CIRCLE)
    return 0;
  if (item >= pr_n)
    return 1;
  int d = p & MX_PAD_RIGHT ? 1 : p & MX_PAD_LEFT ? -1 : 0;
  switch (pr[item].type) {
  case PR_PAD:
    if (p & MX_PAD_CROSS)
      pad_open = pr[item].port, item = 0;
    break;
  case PR_PAIR:
    if (p & MX_PAD_CROSS) {
      int busy = pairing.state == MX_PAD_PAIR_SEARCHING || pairing.state == MX_PAD_PAIR_CONNECTING;
      mx_pad_pair(busy ? 0 : 60);
    }
    break;
  case PR_BRIGHT:
  case PR_IDLE: {
    int b = pr[item].type == PR_BRIGHT;
    const char *key = b ? "pad.brightness" : "pad.idle_off";
    const char *const *v = b ? bright_v : idle_v;
    int now = choice_of(key, b ? "25" : "0", v, 4), c = now + d + (p & MX_PAD_CROSS ? 1 : 0);
    if (c != now) {
      cfg_set(key, v[(c + 4) % 4]);
      cfg_save();
    }
    break;
  }
  case PR_PAIRED:
    if (p & MX_PAD_SQUARE && mx_pad_forget(pr[item].pd.id) == 0)
      toast(N_PAD, "Forgotten", pad_kind(pr[item].pd.kind), 0);
    break;
  }
  return 1;
}

