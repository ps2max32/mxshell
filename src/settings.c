/* MXSHELL2: Settings, with what the OS offers today (PLAN.md, phase A):
     Video        the shell's mode: the console's own, NTSC or PAL (libgl's modes)
     Network      IPCONFIG.DAT on the card: the kernel reads it at its start
     Storage      the devices (the kernel's list), ejecting one, the swap file
     Controllers  which ports have a pad; the motors tried
     System       the shell, the kernel, the services the kernel starts
   The rest (battery, light colour, network status, time) comes with the
   OS's parts (O1-O8). */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/setting.h>
#include <mx/video.h>
#include <math.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* only libmxk has the kernel's table (the standalone build: none) */
#pragma weak mx__kernel

static const char *const page_name[NPAGES] = {"Video", "Network", "Storage", "Controllers", "System"};
static int page, in_page, item, edit;
/* Storage: the kernel's devices, the swap file's device chosen (-1 none) */
static struct pdev st_dev[PDEV_MAX];
static int st_n, st_pick;
static struct spring sel;
static u32 rumble_until;

/* ---- Network: IPCONFIG.DAT, "ip mask gw" ---- */
#define IPCONF "mc0:/MXOS/IPCONFIG.DAT"
static u8 ip[3][4];
static int ip_loaded, ip_dirty;

static void ip_load(void) {
  static const u8 def[3][4] = {{192, 168, 1, 10}, {255, 255, 255, 0}, {192, 168, 1, 1}};
  memcpy(ip, def, sizeof ip);
  char buf[96];
  int fd = mx_fio_open(IPCONF, MX_FIO_RDONLY);
  if (fd >= 0) {
    int n = mx_fio_read(fd, buf, sizeof buf - 1);
    mx_fio_close(fd);
    buf[n > 0 ? n : 0] = 0;
    const char *s = buf;
    for (int a = 0; a < 3; a++)
      for (int b = 0; b < 4; b++) {
        while (*s && (*s < '0' || *s > '9'))
          s++;
        u32 v = 0;
        while (*s >= '0' && *s <= '9')
          v = v * 10 + (u32)(*s++ - '0');
        if (*s || v)
          ip[a][b] = (u8)v;
      }
  }
  ip_loaded = 1;
}

static void ip_save(void) {
  char buf[64];
  int n = mx_snprintf(buf, sizeof buf, "%u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u\n", ip[0][0], ip[0][1], ip[0][2],
                      ip[0][3], ip[1][0], ip[1][1], ip[1][2], ip[1][3], ip[2][0], ip[2][1], ip[2][2], ip[2][3]);
  int fd = mx_fio_open(IPCONF, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    toast(N_WARN, "Not saved", "The memory card could not be written", 1);
    return;
  }
  mx_fio_write(fd, buf, (u32)n);
  mx_fio_close(fd);
  ip_dirty = 0;
  toast(N_INFO, "Network address saved", "Used from the next start", 0);
}

static int vrows(int *r);
static int pr_n;

/* ---- the items of each page ---- */
static int nitems(void) {
  switch (page) {
  case P_VIDEO: {
    int r[8];
    return vrows(r);
  }
  case P_NETWORK: return 4;
  case P_STORAGE: return st_n + 2;
  case P_PADS: return pr_n;
  default: return 0;
  }
}

/* the Video page: a family of modes, and under it what varies in it (the
   rate, or the standard); the rows that only some families have */
enum { FAM_1080, FAM_720, FAM_EDTV, FAM_SD, NFAMS };
static const struct {
  const char *name, *size;
  int n, mode[4];
  const char *sub[4];
} fams[NFAMS] = {
    {"1080", "1920 x 1080", 4, {VM_1080I, VM_1080I50, VM_1080P30, VM_1080P25}, {"60i", "50i", "30p", "25p"}},
    {"720p", "1280 x 720", 2, {VM_720P, VM_720P50}, {"60p", "50p"}},
    {"480p/576p", "640 x 480 / 576", 2, {VM_480P, VM_576P}, {"NTSC  60p", "PAL  50p"}},
    {"NTSC/PAL", "640 x 448 / 512", 2, {VM_NTSC, VM_PAL}, {"NTSC  60i", "PAL  50i"}},
};
static int vfam, vsub; /* the one being chosen (not yet in use) */

static void fam_of(int m, int *f, int *sub) {
  for (int i = 0; i < NFAMS; i++)
    for (int j = 0; j < fams[i].n; j++)
      if (fams[i].mode[j] == m) {
        *f = i, *sub = j;
        return;
      }
  *f = FAM_SD, *sub = 0;
}

/* the rows of the Video page */
enum { VR_FAMILY, VR_RATE, VR_COLOUR, VR_DITHER, VR_DTEST, VR_TV, VR_FLICKER };
static int vrows(int *r) {
  int n = 0;
  r[n++] = VR_FAMILY, r[n++] = VR_RATE, r[n++] = VR_COLOUR, r[n++] = VR_DITHER, r[n++] = VR_DTEST;
  if (vfam == FAM_EDTV || vfam == FAM_SD)
    r[n++] = VR_TV;
  if (vfam == FAM_SD)
    r[n++] = VR_FLICKER;
  return n;
}

/* a new mode is kept only when it is confirmed within 15 s: a television
   that cannot show it gets the old one back */
static int confirm_from = -1;
static u32 confirm_until;

static void video_set(int c) {
  int old = video_choice();
  if (c == old)
    return;
  cfg_set("video.mode", vmodes[c].key);
  video_apply();
  if (video_mode != c) {
    /* libgl could not set it and fell back: back to the old one */
    char m[48];
    mx_snprintf(m, sizeof m, "%s does not run here", vmodes[c].name);
    cfg_set("video.mode", vmodes[old].key);
    video_apply();
    toast(N_WARN, "Mode not available", m, 1);
    return;
  }
  confirm_from = old;
  confirm_until = ticks + (u32)(15.0f / frame_dt);
}

static void video_revert(void) {
  cfg_set("video.mode", vmodes[confirm_from].key);
  confirm_from = -1;
  video_apply();
}

/* tests: in the page, row n chosen */
void settings_pick(int n) { in_page = 1, item = n; }

void settings_enter(void) {
  screen = SCR_SETTINGS;
  fam_of(video_choice(), &vfam, &vsub);
  in_page = 0, item = 0, edit = 0;
  if (!ip_loaded)
    ip_load();
}

/* the rows of a page, as in the mockups: label left, value right, a line
   under each; the chosen one lit. y is the text's baseline; the row is
   pitch high about the middle of its text (text_mid), its line at the
   bottom, and the chosen one's light fills it from the line above (its
   lamp line over that one) to its own */
#define ROW 54.0f
static float pitch = ROW;
static void line(float x0, float x1, float y, const char *label, const char *value, int chosen, int on) {
  float mid = y - text_mid(FONT_BODY), top = mid - pitch * 0.5f, bottom = mid + pitch * 0.5f;
  if (chosen) {
    d_highlight(x0 - 18, top, x1 + 18, bottom, 10, 0.07f, 0.03f, 0.5f);
  }
  float lw = d_text(FONT_BODY, x0, y, label, argb(on ? C_WHITE : C_FAINT, 1)) - x0;
  if (value)
    d_text_fit(FONT_BODY, FONT_SMALL, x1 - fminf(d_text_w(FONT_BODY, value), x1 - x0 - lw - 24), y,
               x1 - x0 - lw - 24, value, argb(chosen ? C_WHITE : C_DIM, 1));
  d_rect(x0, bottom, x1, bottom + 1, argb(C_WHITE, 0.08f), argb(C_WHITE, 0.08f));
}

static void note(float x, float y, const char *s) { d_text(FONT_SMALL, x, y, s, argb(C_FAINT, 1)); }

/* choices side by side at the right, the chosen one a pill; returns the left edge */
static float seg(float x1, float y, const char *const *l, int n, int on, int lit) {
  float x = x1;
  for (int i = n - 1; i >= 0; i--) {
    float w = d_text_w(FONT_SMALL, l[i]), x0 = x - w - 32;
    float mid = y - text_mid(FONT_SMALL);
    if (i == on)
      d_pill(x0, mid - 17, x, mid + 17, lit ? C_WHITE : 0x3A3D44);
    d_text(FONT_SMALL, x0 + 16, y, l[i], argb(i == on ? (lit ? C_INK : C_WHITE) : C_DIM, 1));
    x = x0 - 8;
  }
  return x;
}

static void vrow(float x0, float x1, float y, const char *label, const char *sub, int chosen) {
  float mid = y - text_mid(FONT_BODY);
  if (chosen) {
    d_highlight(x0 - 18, mid - 26, x1 + 18, mid + 26, 10, 0.07f, 0.03f, 0.5f);
  }
  d_text(FONT_BODY, x0, sub ? y - 9 : y, label, argb(C_WHITE, 1));
  if (sub)
    d_text(FONT_SMALL, x0, y + 15, sub, argb(C_FAINT, 1));
  d_rect(x0, mid + 26, x1, mid + 27, argb(C_WHITE, 0.08f), argb(C_WHITE, 0.08f));
}

static void page_video(float x0, float x1, float y) {
  int use = video_choice(), uf, us;
  fam_of(use, &uf, &us);
  int r[8], n = vrows(r), cur = in_page ? r[item < n ? item : 0] : -1;
  /* the families: four cards, the one being chosen lit, the one in use marked */
  float gap = 12, cw = (x1 - x0 - gap * 3) / 4, ch = 74, cy = y - 12;
  for (int f = 0; f < NFAMS; f++) {
    float cx = x0 + (cw + gap) * f;
    int on = f == vfam, lit = on && cur == VR_FAMILY;
    d_highlight(cx, cy, cx + cw, cy + ch, 10, on ? (lit ? 0.12f : 0.08f) : 0.035f, on ? 0.04f : 0.015f,
                on ? (lit ? 1 : 0.5f) : 0);
    d_text_fit(FONT_BUTTON, FONT_SMALL, cx + 12, cy + 34, cw - 20, fams[f].name, argb(on ? C_WHITE : C_DIM, 1));
    const char *sub = f == uf ? "In use" : fams[f].size;
    if (f == uf || cw >= 150) /* 4:3: the sizes do not fit, only "In use" */
      d_text(FONT_SMALL, cx + 12, cy + 60, sub, argb(f == uf ? C_LAMP : C_FAINT, 1));
  }
  float yy = cy + ch + 44, step = 50;
  for (int i = 1; i < n; i++, yy += step) {
    int chosen = cur == r[i];
    switch (r[i]) {
    case VR_RATE: {
      /* a choice that is not in use: what it is, and how to try it */
      int m = fams[vfam].mode[vsub];
      char l[64];
      mx_snprintf(l, sizeof l, "%s" DOT "not in use yet", vmodes[m].name);
      vrow(x0, x1, yy, "Frame rate", m == use ? 0 : l, chosen);
      seg(x1, yy, fams[vfam].sub, fams[vfam].n, vsub, chosen);
      break;
    }
    case VR_COLOUR: {
      static const char *const l[2] = {"32-bit where it fits", "16-bit"};
      vrow(x0, x1, yy, "Colour", vfam == FAM_1080 ? "1080 is always 16-bit: 32 does not fit" : 0, chosen);
      if (vfam == FAM_1080)
        seg(x1, yy, l + 1, 1, 0, chosen);
      else
        seg(x1, yy, l, 2, cfg_get("video.depth", "32")[0] == '3' ? 0 : 1, chosen);
      break;
    }
    case VR_DITHER: {
      const char *l[1] = {dither_name(dither_choice())};
      vrow(x0, x1, yy, "Dithering", "16-bit colour", chosen);
      seg(x1, yy, l, 1, 0, chosen);
      break;
    }
    case VR_DTEST:
      vrow(x0, x1, yy, "Dithering test picture", 0, chosen);
      break;
    case VR_TV: {
      static const char *const l[2] = {"4:3", "16:9"};
      vrow(x0, x1, yy, "Television", "The shape of the picture", chosen);
      seg(x1, yy, l, 2, video_sd_wide(), chosen);
      break;
    }
    case VR_FLICKER: {
      static const char *const l[2] = {"Off", "On"};
      vrow(x0, x1, yy, "Flicker filter", "Softer, against flicker on interlaced lines", chosen);
      seg(x1, yy, l, 2, video_deflicker(), chosen);
      break;
    }
    }
  }
}

static void page_network(float x0, float x1, float y) {
  static const char *const names[3] = {"IP address", "Subnet mask", "Gateway"};
  for (int i = 0; i < 3; i++) {
    char v[24];
    mx_snprintf(v, sizeof v, "%u.%u.%u.%u", ip[i][0], ip[i][1], ip[i][2], ip[i][3]);
    int chosen = in_page && item == i;
    line(x0, x1, y + ROW * i, names[i], v, chosen, 1);
    if (chosen && edit) {
      /* the number being changed: the lamp's line under it */
      char part[24];
      int b = edit - 1;
      float px = x1 - d_text_w(FONT_BODY, v);
      for (int k = 0; k < b; k++) {
        mx_snprintf(part, sizeof part, "%u.", ip[i][k]);
        px += d_text_w(FONT_BODY, part);
      }
      mx_snprintf(part, sizeof part, "%u", ip[i][b]);
      float yy = y + ROW * i + 8;
      d_rect(px, yy, px + d_text_w(FONT_BODY, part), yy + 2, argb(C_LAMP, 1), argb(C_LAMP, 1));
    }
  }
  line(x0, x1, y + ROW * 3, ip_dirty ? "Save" : "Saved", 0, in_page && item == 3, ip_dirty);
  note(x0, y + ROW * 4, "The kernel reads these when it starts.");
  note(x0, y + ROW * 4 + 26, "Automatic (DHCP) and the status come with the OS's network calls.");
}

/* ---- Storage: the kernel's devices, the swap file, games from the PC ---- */
static u32 st_gen = ~0u;
/* where the swap file may go: a drive (not a memory card, not the PC) */
static int swap_ok(const struct pdev *d) {
  return d->kind != MX_DEV_MC && d->kind != MX_DEV_HOST && !(d->flags & MX_DEV_READONLY);
}
static void st_read(void) {
  if (pdevs_gen() == st_gen)
    return;
  st_gen = pdevs_gen();
  st_n = pdevs_read(st_dev, PDEV_MAX, 0);
  /* the choice: where the swap file is now */
  struct mx_swap_info si;
  st_pick = -1;
  if (mx_swap_info(&si) == 0 && si.path[0])
    for (int i = 0; i < st_n; i++)
      if (!strncmp(si.path, st_dev[i].root, strlen(st_dev[i].root)))
        st_pick = i;
}
static void swap_path(char *out, int len, int d) {
  if (d < 0)
    out[0] = 0;
  else
    mx_snprintf(out, len, "%s/PS2MAX/swap.bin", st_dev[d].root);
}

static void page_storage(float x0, float x1, float y) {
  st_read();
  for (int d = 0; d < st_n; d++) {
    const struct pdev *v = &st_dev[d];
    char l[48], val[64], b[16], f[16];
    mx_snprintf(l, sizeof l, "%s" DOT "%s", pdev_name(v), v->label[0] ? v->label : v->root);
    pdev_bytes(b, sizeof b, v->size);
    val[0] = 0;
    if (v->free)
      pdev_bytes(f, sizeof f, v->free), mx_snprintf(val, sizeof val, "%s free of %s", f, b);
    else if (v->size)
      mx_snprintf(val, sizeof val, "%s", b);
    int n = 0;
    for (int g = 0; g < ngames; g++)
      n += !strcmp(game_root(g), v->root);
    if (n) {
      int l0 = (int)strlen(val);
      mx_snprintf(val + l0, sizeof val - (u32)l0, "%s%d game%s", l0 ? DOT : "", n, n == 1 ? "" : "s");
    }
    if (v->flags & MX_DEV_SWAP) {
      int l0 = (int)strlen(val);
      mx_snprintf(val + l0, sizeof val - (u32)l0, "%sswap", l0 ? DOT : "");
    }
    line(x0, x1, y + ROW * d, l, val, in_page && item == d, 1);
  }
  /* the swap file: left and right choose the device, cross uses it */
  struct mx_swap_info si;
  memset(&si, 0, sizeof si);
  mx_swap_info(&si);
  const char *const state[] = {"None", "Waiting for its device", "Being made", "In use", "Could not be made"};
  int ys = st_n, chosen = in_page && item == ys;
  /* the choices side by side: None and the drives it may go on */
  const char *ch[PDEV_MAX + 1];
  int nch = 0, on = 0;
  ch[nch++] = "None";
  for (int d = 0; d < st_n; d++)
    if (swap_ok(&st_dev[d])) {
      if (d == st_pick)
        on = nch;
      ch[nch++] = st_dev[d].root;
    }
  line(x0, x1, y + ROW * ys, "Swap file", 0, chosen, 1);
  seg(x1, y + ROW * ys, ch, nch, on, chosen);
  line(x0, x1, y + ROW * (ys + 1), "Games from the PC (host:)", cfg_get("dev.host", "0")[0] == '1' ? "On" : "Off",
       in_page && item == ys + 1, 1);
  float ny = y + ROW * (ys + 2);
  char m[96];
  mx_snprintf(m, sizeof m, "Swap file: %s%s%s.", si.path[0] ? si.path : "none", si.path[0] ? DOT : "",
              si.path[0] ? state[si.state < 5 ? si.state : 0] : "");
  note(x0, ny, m);
  if (!si.path[0])
    d_text(FONT_SMALL, x0, ny + 26, "Without a swap file MXOS runs out of memory soon.", argb(C_WARN, 1));
  else
    note(x0, ny + 26, "Cross on a device: ready to remove. Games are in <device>/PS2MAX/.");
}

void settings_goto(int pg, int swap_row) {
  settings_enter();
  page = pg, in_page = 1, edit = 0;
  st_gen = ~0u;
  st_read();
  item = pg == P_STORAGE && swap_row ? st_n : 0;
  screen = SCR_SETTINGS;
}

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

static void page_pads(float x0, float x1, float y) {
  pr_read();
  mx_pad_pair_state(&pairing);
  int nports = 0;
  for (int i = 0; i < pr_n; i++) {
    float yy = y + ROW * i, mid = yy - text_mid(FONT_BODY);
    int chosen = in_page && item == i;
    char l[48], v[64];
    switch (pr[i].type) {
    case PR_PAD: {
      const struct mx_pad_info *in = &pr[i].in;
      static const char *const link[] = {"Port", "USB", "Bluetooth"};
      nports++;
      mx_snprintf(l, sizeof l, "P%d" DOT "%s", pr[i].port + 1, pad_kind(in->kind));
      if (chosen && ticks < rumble_until)
        mx_snprintf(v, sizeof v, "Rumbling");
      else if (in->link == MX_PAD_LINK_PORT)
        mx_snprintf(v, sizeof v, "Controller port %d", in->hw + 1);
      else
        mx_snprintf(v, sizeof v, "%s%s%s", link[in->link < 3 ? in->link : 0],
                    in->flags & MX_PAD_INFO_CHARGING ? DOT "charging" : "",
                    in->link == MX_PAD_LINK_BT && in->flags & MX_PAD_INFO_CABLE ? DOT "cable" : "");
      line(x0, x1, yy, l, 0, chosen, 1);
      /* at the right: the light's colour, the battery, the link */
      float xr = x1;
      if (in->battery != 255) {
        xr -= 34;
        battery(xr, mid, in->battery, in->flags & MX_PAD_INFO_CHARGING);
        xr -= 14;
      }
      if (has_light(in->kind) && (in->rgb[0] | in->rgb[1] | in->rgb[2])) {
        u32 rgb = (u32)in->rgb[0] << 16 | (u32)in->rgb[1] << 8 | in->rgb[2];
        /* the light bar as it is, made bright enough to see its colour */
        u32 m = in->rgb[0] > in->rgb[1] ? in->rgb[0] : in->rgb[1];
        m = m > in->rgb[2] ? m : in->rgb[2];
        if (m && m < 200) {
          u32 r = in->rgb[0] * 200 / m, g = in->rgb[1] * 200 / m, b = in->rgb[2] * 200 / m;
          rgb = r << 16 | g << 8 | b;
        }
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
      line(x0, x1, yy, "Bluetooth", val, chosen, pairing.state != MX_PAD_PAIR_NO_DONGLE);
      break;
    }
    case PR_BRIGHT:
      line(x0, x1, yy, "Light bars", 0, chosen, 1);
      seg(x1, yy, bright_l, 4, choice_of("pad.brightness", "25", bright_v, 4), chosen);
      break;
    case PR_IDLE:
      line(x0, x1, yy, "Idle, off after", 0, chosen, 1);
      seg(x1, yy, idle_l, 4, choice_of("pad.idle_off", "0", idle_v, 4), chosen);
      break;
    case PR_PAIRED: {
      const u8 *a = pr[i].pd.addr;
      mx_snprintf(l, sizeof l, "%s" DOT "paired", pad_kind(pr[i].pd.kind));
      mx_snprintf(v, sizeof v, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
      line(x0, x1, yy, l, v, chosen, 0);
      break;
    }
    }
  }
  if (!nports)
    note(x0, y + ROW * pr_n, "No controller: plug one in, or pair one over Bluetooth.");
  else
    note(x0, y + ROW * pr_n, "Players follow the ports: the controller ports first,"),
      note(x0, y + ROW * pr_n + 26, "then USB and Bluetooth in the order they came.");
}

/* the Controllers page's hints, for the row chosen */
static const char *const *pads_hints(void) {
  static const char *h[12];
  int n = 0;
  if (item < pr_n)
    switch (pr[item].type) {
    case PR_PAD:
      if (has_light(pr[item].in.kind) && pr[item].in.id)
        h[n++] = "L R", h[n++] = "Colour";
      h[n++] = SYM_CROSS, h[n++] = "Motors";
      if (pr[item].in.link == MX_PAD_LINK_BT)
        h[n++] = SYM_SQUARE, h[n++] = "Disconnect";
      break;
    case PR_PAIR:
      h[n++] = SYM_CROSS;
      h[n++] = pairing.state == MX_PAD_PAIR_SEARCHING || pairing.state == MX_PAD_PAIR_CONNECTING ? "Stop" : "Pair";
      break;
    case PR_BRIGHT:
    case PR_IDLE: h[n++] = "L R", h[n++] = "Change"; break;
    case PR_PAIRED: h[n++] = SYM_SQUARE, h[n++] = "Forget"; break;
    }
  h[n++] = SYM_CIRCLE, h[n++] = "Back", h[n] = 0;
  return h;
}

/* the Controllers page's buttons (up and down are the page's) */
static void pads_input(u32 p) {
  if (item >= pr_n)
    return;
  int d = p & MX_PAD_RIGHT ? 1 : p & MX_PAD_LEFT ? -1 : 0;
  switch (pr[item].type) {
  case PR_PAD: {
    const struct mx_pad_info *in = &pr[item].in;
    if (d && has_light(in->kind) && in->id) {
      int c = (colour_of(in->id) + d + NCOLOURS) % NCOLOURS;
      colour_set(in->id, c);
    }
    if (p & MX_PAD_CROSS && mx_pad_rumble(pr[item].port, 1, 160) >= 0)
      rumble_until = ticks + 30;
    if (p & MX_PAD_SQUARE && in->link == MX_PAD_LINK_BT && mx_pad_disconnect(pr[item].port) == 0)
      toast(N_PAD, "Disconnected", pad_kind(in->kind), 0);
    break;
  }
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
    int c = choice_of(key, b ? "25" : "0", v, 4) + d + (p & MX_PAD_CROSS ? 1 : 0);
    if (c != choice_of(key, b ? "25" : "0", v, 4)) {
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
}

static void page_system(float x0, float x1, float y) {
  line(x0, x1, y, "Shell", "MXSHELL2 0.2", 0, 1);
  char v[64] = "Not there";
  const struct mxk_services *k = &mx__kernel ? mx__kernel : 0;
  if (k)
    mx_snprintf(v, sizeof v, "Version %u", k->version);
  line(x0, x1, y + ROW, "Kernel", v, 0, 1);
  char buf[64] = {0};
  int fd = mx_fio_open("mc0:/MXOS/SERVICES.CFG", MX_FIO_RDONLY);
  if (fd >= 0) {
    int n = mx_fio_read(fd, buf, sizeof buf - 1);
    mx_fio_close(fd);
    buf[n > 0 ? n : 0] = 0;
    for (char *s = buf; *s; s++)
      if (*s == '\n' || *s == '\r')
        *s = 0;
  }
  line(x0, x1, y + ROW * 2, "Services", buf[0] ? buf : "None", 0, 1);
}

static void confirm_draw(void) {
  float w = 520, h = 190, x = (SCR_W - w) / 2, y = 250;
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.6f), argb(C_INK, 0.6f));
  d_shadow(x + w / 2, y + h / 2, w * 0.7f, h * 0.9f, 0.5f);
  d_card_lit(x, y, x + w, y + h, 14, 0x17191E, 1, 0.55f);
  d_text(FONT_TITLE, x + 32, y + 58, "Keep this mode?", argb(C_WHITE, 1));
  char m[48];
  int left = (int)((float)(confirm_until - ticks) * frame_dt) + 1;
  mx_snprintf(m, sizeof m, "The old one comes back in %d s.", left);
  d_text(FONT_BODY, x + 32, y + 92, m, argb(C_DIM, 1));
  float bx = x + 32;
  aa_under(0x17191E);
  bx += d_button(bx, y + 118, SYM_CROSS, "Keep", 1) + 12;
  d_button(bx, y + 118, SYM_CIRCLE, "Go back", 0);
  aa_under_room();
}

void settings_draw(void) {
  if (confirm_from >= 0 && (int)(ticks - confirm_until) >= 0)
    video_revert();
  spring_step(&sel, (float)page, W_FOCUS, frame_dt);
  room_begin(-1);
  room_light(MARGIN + 130, 146 + sel.x * 52, 360, 280, 0.06f);
  d_begin_2d();
  room_draw();

  /* the pages: the chosen one a lit pill */
  float y0 = 120, step = 52;
  float py = y0 + sel.x * step;
  if (in_page)
    d_pill_glass(MARGIN, py, MARGIN + 260, py + 52, 18, 0);
  else
    d_pill(MARGIN, py, MARGIN + 260, py + 52, C_WHITE);
  for (int i = 0; i < NPAGES; i++) {
    int on = i == page;
    u32 c = on ? (in_page ? C_WHITE : C_INK) : C_DIM;
    d_text(on ? FONT_BUTTON : FONT_BODY, MARGIN + 24, y0 + step * i + 26 + text_mid(FONT_BUTTON), page_name[i], argb(c, 1));
  }

  float x0 = MARGIN + 260 + 64, x1 = SCR_W - MARGIN;
  if (x1 > x0 + 620)
    x1 = x0 + 620;
  d_text(FONT_DISP, x0 - 2, 162, page_name[page], argb(C_WHITE, 1));
  float y = page == P_VIDEO ? 206 : 236;
  switch (page) {
  case P_VIDEO: page_video(x0, x1, y); break;
  case P_NETWORK: page_network(x0, x1, y); break;
  case P_STORAGE: page_storage(x0, x1, y); break;
  case P_PADS: page_pads(x0, x1, y); break;
  default: page_system(x0, x1, y); break;
  }
  if (ticks >= rumble_until && rumble_until) {
    for (int i = 0; i < MX_PAD_PORTS; i++)
      mx_pad_rumble(i, 0, 0);
    rumble_until = 0;
  }

  static const char *const h_list[] = {SYM_CROSS, "Open", "L1", "Section", SYM_CIRCLE, "Back", 0};
  static const char *const h_page[] = {SYM_CROSS, "Change", SYM_CIRCLE, "Back", 0};
  static const char *const h_edit[] = {"L R", "Number", "UP DOWN", "Change", SYM_CROSS, "Done", 0};
  if (confirm_from >= 0)
    confirm_draw();
  else
    hints_draw(edit ? h_edit : in_page && page == P_PADS ? pads_hints() : in_page ? h_page : h_list);
  status_draw();
}

/* the Video page: left and right change a row's choice, cross uses it */
static void video_input(u32 p) {
  int r[8], n = vrows(r);
  if (item >= n)
    item = n - 1;
  int d = p & MX_PAD_RIGHT ? 1 : p & MX_PAD_LEFT ? -1 : 0, x = p & MX_PAD_CROSS;
  if (p & MX_PAD_CIRCLE) {
    in_page = 0;
    return;
  }
  switch (r[item]) {
  case VR_FAMILY:
    if (d && vfam + d >= 0 && vfam + d < NFAMS) {
      int uf, us;
      fam_of(video_choice(), &uf, &us);
      vfam += d;
      /* in use: its own rate; else the console's region in SD, the first otherwise */
      vsub = vfam == uf ? us : vfam >= FAM_EDTV && mx_video_default() == &mx_video_pal ? 1 : 0;
    }
    if (x)
      video_set(fams[vfam].mode[vsub]);
    break;
  case VR_RATE:
    if (d && vsub + d >= 0 && vsub + d < fams[vfam].n)
      vsub += d;
    if (x)
      video_set(fams[vfam].mode[vsub]);
    break;
  case VR_COLOUR:
    if ((d || x) && vfam != FAM_1080) {
      cfg_set("video.depth", cfg_get("video.depth", "32")[0] == '3' ? "16" : "32");
      cfg_save();
      video_apply();
    }
    break;
  case VR_DITHER:
    if (d || x)
      dither_next(d < 0 ? -1 : 1);
    break;
  case VR_DTEST:
    if (x)
      screen = SCR_DITHER;
    break;
  case VR_TV:
    if (d || x) {
      cfg_set("video.aspect", video_sd_wide() ? "4:3" : "16:9");
      cfg_save();
      video_apply();
    }
    break;
  case VR_FLICKER:
    if (d || x) {
      cfg_set("video.deflicker", video_deflicker() ? "off" : "on");
      cfg_save();
      video_apply();
    }
    break;
  }
}

void settings_input(void) {
  u32 p = pad_pressed;
  if (confirm_from >= 0) {
    if (p & MX_PAD_CROSS)
      confirm_from = -1, cfg_save();
    else if (p & MX_PAD_CIRCLE)
      video_revert();
    return;
  }
  if (!in_page) {
    if (p & MX_PAD_UP && page > 0)
      page--;
    if (p & MX_PAD_DOWN && page + 1 < NPAGES)
      page++;
    if (p & (MX_PAD_CROSS | MX_PAD_RIGHT) && nitems())
      in_page = 1, item = 0, fam_of(video_choice(), &vfam, &vsub);
    /* L1: the section before (Settings is the last); circle: Home */
    if (p & (MX_PAD_CIRCLE | MX_PAD_L1))
      screen = SCR_HOME;
    return;
  }
  if (edit) {
    /* a number of an address: left and right choose, up and down change it */
    u8 *v = &ip[item][edit - 1];
    if (p & MX_PAD_LEFT && edit > 1)
      edit--;
    if (p & MX_PAD_RIGHT && edit < 4)
      edit++;
    if (pad_held & MX_PAD_UP && (p & MX_PAD_UP || ticks % 4 == 0))
      (*v)++, ip_dirty = 1;
    if (pad_held & MX_PAD_DOWN && (p & MX_PAD_DOWN || ticks % 4 == 0))
      (*v)--, ip_dirty = 1;
    if (p & (MX_PAD_CROSS | MX_PAD_CIRCLE))
      edit = 0;
    return;
  }
  if (p & MX_PAD_UP && item > 0)
    item--;
  if (p & MX_PAD_DOWN && item + 1 < nitems())
    item++;
  if (page == P_VIDEO) {
    video_input(p);
    return;
  }
  if (page == P_PADS) {
    if (p & MX_PAD_CIRCLE)
      in_page = 0;
    else
      pads_input(p);
    return;
  }
  if (page == P_STORAGE && item == st_n && p & (MX_PAD_LEFT | MX_PAD_RIGHT)) {
    /* the swap file's device: none, then the drives it may go on */
    int d = p & MX_PAD_RIGHT ? 1 : -1, at = st_pick;
    for (int k = 0; k <= st_n; k++) {
      at += d;
      if (at < -1)
        at = st_n - 1;
      if (at >= st_n)
        at = -1;
      if (at < 0 || swap_ok(&st_dev[at]))
        break;
    }
    st_pick = at;
    return;
  }
  if (p & (MX_PAD_CIRCLE | MX_PAD_LEFT))
    in_page = 0;
  if (!(p & MX_PAD_CROSS))
    return;
  switch (page) {
  case P_NETWORK:
    if (item < 3)
      edit = 1;
    else if (ip_dirty)
      ip_save();
    break;
  case P_STORAGE:
    if (item < st_n) {
      /* ready to remove: the kernel says why not when it is not */
      if (st_dev[item].kind != MX_DEV_MC && mx_dev_eject(st_dev[item].root) == 0)
        toast(N_DEVICE, "Ready to remove", pdev_name(&st_dev[item]), 0);
    } else if (item == st_n) {
      char path[40];
      swap_path(path, sizeof path, st_pick);
      if (mx_swap_set(path, 64) < 0)
        toast(N_WARN, "Swap file not set", path[0] ? path : "None", 1);
    } else {
      cfg_set("dev.host", cfg_get("dev.host", "0")[0] == '1' ? "0" : "1");
      cfg_save();
      games_search();
    }
    break;
  }
}

/* ---- the test picture: ramps of the shell's own colours ---- */
static void ramp(float y, float h, u32 from, u32 to, const char *label) {
  float x0 = MARGIN, x1 = SCR_W - MARGIN;
  const int n = 32;
  glDisable(GL_BLEND);
  glBegin(GL_TRIANGLE_STRIP);
  for (int i = 0; i <= n; i++) {
    float k = (float)i / n, x = x0 + (x1 - x0) * k, rgb[3];
    for (int c = 0; c < 3; c++) {
      float a = (float)(from >> (16 - 8 * c) & 255), b = (float)(to >> (16 - 8 * c) & 255);
      rgb[c] = a + (b - a) * k;
    }
    room_vertex(x, y, rgb), room_vertex(x, y + h, rgb);
  }
  glEnd();
  glEnable(GL_BLEND);
  d_label(x0, y - 10, label, argb(C_FAINT, 1));
}

void dither_draw(void) {
  room_begin(-1);
  room_light(SCR_W * 0.72f, 600, 260, 120, 0.16f);
  d_begin_2d();
  room_draw();
  d_label(MARGIN, 96, "Video" DOT "16-bit colour", argb(C_LAMP, 1));
  float x = d_text(FONT_TITLE, MARGIN - 1, 136, "Dithering: ", argb(C_WHITE, 1));
  d_text(FONT_TITLE, x, 136, dither_name(dither_choice()), argb(C_WHITE, 1));
  float y = 190;
  ramp(y, 60, 0x080808, 0x383838, "Ink to graphite"), y += 92;
  ramp(y, 60, 0x080808, 0x5A4A30, "Ink to lamp light");
  y += 92;
  ramp(y, 60, 0x101010, 0x45362A, "A tile's colour");
  y += 92;
  ramp(y, 60, 0x000000, 0xFFFFFF, "Black to white");
  d_label(MARGIN, 600, "A soft light on ink (right)", argb(C_FAINT, 1));
  static const char *const hints[] = {"L R", "Off, on, noise", SYM_CIRCLE, "Back", 0};
  hints_draw(hints);
}

void dither_input(void) {
  if (pad_pressed & (MX_PAD_RIGHT | MX_PAD_R1))
    dither_next(1);
  if (pad_pressed & (MX_PAD_LEFT | MX_PAD_L1))
    dither_next(-1);
  if (pad_pressed & MX_PAD_CIRCLE)
    settings_enter();
}
