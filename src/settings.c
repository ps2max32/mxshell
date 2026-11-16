/* MXSHELL2: Settings, with what the OS offers today (PLAN.md, phase A):
     Video        the system's video settings (kernel version 31): the mode
                  (tried for 15 s), the output, the TV's shape, overscan,
                  how programs are shown; the shell's own picture
     Network      the kernel's status (version 30), automatic (DHCP) or
                  manual addresses: the settings net.*
     Storage      the devices (the kernel's list), ejecting one, the swap file
     Controllers  which ports have a pad; the motors tried
     System       the shell, the kernel, the services the kernel starts
     Date and time  the kernel's time (version 29): set, the zone, summer
                  time, the clock's format */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/kernel.h>
#include <mx/net.h>
#include <mx/pad.h>
#include <mx/screen.h>
#include <mx/sif.h>
#include <mx/time.h>
#include <mx/setting.h>
#include <mx/video.h>
#include <math.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* only libmxk has the kernel's table (the standalone build: none) */
#pragma weak mx__kernel

static const char *const page_name[NPAGES] = {"Video", "Network", "Storage", "Controllers", "Date and time", "System"};
static int page, in_page, item, edit;
/* Date and time: its rows, the field being changed (TF_*; 0 none) and the
   date and time being set */
enum { TR_DATE, TR_ZONE, TR_DST, TR_FORMAT, TR_ROWS };
enum { TF_DAY = 1, TF_MONTH, TF_YEAR, TF_HOUR, TF_MINUTE };
static int tedit;
static struct mx_tm tset;
static void toggle(const char *key, const char *a, const char *b, const char *def);
/* Storage: the kernel's devices, the swap file's device chosen (-1 none) */
static struct pdev st_dev[PDEV_MAX];
static int st_n, st_pick;
static struct spring sel;

/* ---- Network (kernel version 30, <mx/net.h>): the kernel's status, and
   the set-up as settings: net.mode "dhcp" or "manual" (net.ip, net.mask,
   net.gw, net.dns), which the kernel applies a second after the last
   change. The manual addresses are edited here and written together ---- */
enum { NR_MODE, NR_IP, NR_MASK, NR_GW, NR_DNS, NR_USE, NR_ROWS };
static const char *const net_key[NR_USE] = {0, "net.ip", "net.mask", "net.gw", "net.dns"};
static u8 ip[NR_USE][4]; /* the manual addresses, NR_IP..NR_DNS */
static int ip_dirty;

static int net_dhcp(void) { return !strcmp(cfg_get("net.mode", "manual"), "dhcp"); }

static void addr_of(u32 a, u8 *o) { o[0] = (u8)a, o[1] = (u8)(a >> 8), o[2] = (u8)(a >> 16), o[3] = (u8)(a >> 24); }

/* a dotted address; 0 if it is not one */
static int addr_parse(const char *t, u8 *o) {
  for (int b = 0; b < 4; b++) {
    u32 v = 0, n = 0;
    while (*t >= '0' && *t <= '9' && n < 4)
      v = v * 10 + (u32)(*t++ - '0'), n++;
    if (!n || v > 255 || (b < 3 && *t++ != '.'))
      return 0;
    o[b] = (u8)v;
  }
  return !*t;
}

/* the fields: the settings, else what the network uses now, else the
   kernel's defaults */
static void ip_load(void) {
  static const u8 def[NR_USE][4] = {{0}, {192, 168, 1, 10}, {255, 255, 255, 0}, {192, 168, 1, 1}, {0, 0, 0, 0}};
  struct mx_net_status ns;
  int st = mx_net_status(&ns) == 0 && ns.ip;
  u32 now[NR_USE] = {0, st ? ns.ip : 0, st ? ns.mask : 0, st ? ns.gw : 0, st ? ns.dns : 0};
  for (int r = NR_IP; r < NR_USE; r++) {
    memcpy(ip[r], def[r], 4);
    if (!addr_parse(cfg_get(net_key[r], ""), ip[r]) && now[r])
      addr_of(now[r], ip[r]);
  }
  ip_dirty = 0;
}

/* the manual set-up in use: the four addresses, then the mode (the kernel
   applies them all a second after the last) */
static void ip_save(void) {
  for (int r = NR_IP; r < NR_USE; r++) {
    char t[16];
    if (r == NR_DNS && !(ip[r][0] | ip[r][1] | ip[r][2] | ip[r][3]))
      t[0] = 0;
    else
      mx_snprintf(t, sizeof t, "%u.%u.%u.%u", ip[r][0], ip[r][1], ip[r][2], ip[r][3]);
    cfg_set(net_key[r], t);
  }
  cfg_set("net.mode", "manual");
  cfg_save();
  ip_dirty = 0;
}

static int vrows(int *r);

/* ---- the items of each page ---- */
static int nitems(void) {
  switch (page) {
  case P_VIDEO: {
    int r[16];
    return vrows(r);
  }
  case P_NETWORK: return net_dhcp() ? 1 : NR_ROWS;
  case P_STORAGE: return st_n + 2;
  case P_PADS: return 1; /* pads.c has its own rows */
  case P_TIME: return TR_ROWS;
  default: return 0;
  }
}

/* the Video page (the system's settings, <mx/screen.h>, kernel version 31):
   the mode as four cards and its standard under them, the output, the TV's
   shape and overscan, how programs are shown, and two lists of their own:
   the modes this TV shows (video.allowed) and the shell's own picture */
enum { FAM_1080, FAM_720, FAM_EDTV, FAM_SD, NFAMS };
static const struct {
  const char *size;
  int n, mode[5];
  const char *sub[5];
} fams[NFAMS] = { /* the region's 60 Hz first, its 50 Hz next */
    {"1920 x 1080", 5, {VM_1080I, VM_1080I50, VM_1080P24, VM_1080P25, VM_1080P30}, {"60i", "50i", "24p", "25p", "30p"}},
    {"1280 x 720", 2, {VM_720P, VM_720P50}, {"60p", "50p"}},
    {"640 x 480 / 576", 2, {VM_480P, VM_576P}, {"NTSC  60p", "PAL  50p"}},
    {"640 x 448 / 512", 4, {VM_480I, VM_576I, VM_240P, VM_288P}, {"480i", "576i", "240p", "288p"}},
};
static int vfam, vsub; /* the one being chosen (not yet in use) */
enum { VL_MAIN, VL_PICTURE, VL_ALLOWED };
static int vlist;

static void fam_of(int m, int *f, int *sub) {
  for (int i = 0; i < NFAMS; i++)
    for (int j = 0; j < fams[i].n; j++)
      if (fams[i].mode[j] == m) {
        *f = i, *sub = j;
        return;
      }
  *f = FAM_SD, *sub = 0;
}

/* the region's own mode: every TV of the region shows it, on any cable */
static int region_sd(void) { return mx_video_default() == &mx_video_pal ? VM_576I : VM_480I; }
/* RGB (SCART, composite, S-video) carries 480i, 576i, 240p and 288p only */
static int needs_component(int vm) { return !VM_RGB(vm); }

/* the rows of the Video page and its lists */
enum { VR_FAMILY, VR_STD, VR_OUT, VR_TV, VR_OVERSCAN, VR_SCALE, VR_ALLOWED, VR_PICTURE,
       VR_COLOUR, VR_DITHER, VR_DTEST, VR_FLICKER, VR_MODE /* + MX_SCREEN_*: video.allowed's */ };
static int vrows(int *r) {
  int n = 0;
  if (vlist == VL_PICTURE) {
    r[n++] = VR_COLOUR, r[n++] = VR_DITHER, r[n++] = VR_DTEST, r[n++] = VR_FLICKER;
  } else if (vlist == VL_ALLOWED) {
    struct mx_screen_conf c;
    video_conf(&c);
    for (int vm = 0; vm < VM_MODES && n < 15; vm++) {
      u32 m = mx_screen_mode_of(vmodes[vm].key);
      if (c.allowed >> m & 1)
        r[n++] = VR_MODE + (int)m;
    }
  } else {
    r[n++] = VR_FAMILY, r[n++] = VR_STD, r[n++] = VR_OUT, r[n++] = VR_TV, r[n++] = VR_OVERSCAN;
    r[n++] = VR_SCALE, r[n++] = VR_ALLOWED, r[n++] = VR_PICTURE;
  }
  return n;
}

/* ---- the trial: a new mode or output is kept only when confirmed. The
   kernel puts the confirmed one back 15 s after the change (the TV may show
   nothing); the shell asks, and puts it back itself on circle or when its
   own 15 s are over ---- */
static int trial, os_ask;
static u32 trial_until;

/* "<mode> <out>": now, and the one confirmed (none: the region's mode) */
static void now_text(char *t, u32 n) {
  struct mx_screen_conf c;
  video_conf(&c);
  mx_snprintf(t, n, "%s %s", mx_screen_name(c.mode), c.rgb ? "rgb" : "component");
}
static void confirmed_text(char *t, u32 n) {
  const char *v = cfg_get("video.confirm", "");
  if (*v)
    mx_snprintf(t, n, "%s", v);
  else
    mx_snprintf(t, n, "%s component", vmodes[region_sd()].key);
}

static void video_put(const char *mode, const char *out) {
  cfg_set("video.mode", mode);
  cfg_set("video.out", out);
  cfg_save();
  video_apply();
}

/* back to the confirmed mode and output */
static void video_back(void) {
  char t[MX_SETTING_VALUE];
  confirmed_text(t, sizeof t);
  char *sp = strchr(t, ' ');
  const char *out = "component";
  if (sp)
    *sp = 0, out = sp + 1;
  trial = 0;
  video_put(t, out);
}

static void video_try(int vm, int rgb) {
  char before[24];
  now_text(before, sizeof before);
  struct mx_screen_conf c;
  video_conf(&c);
  if (vm == video_of_screen(c.mode) && rgb == c.rgb)
    return;
  video_put(vmodes[vm].key, rgb ? "rgb" : "component");
  if (video_mode != vm) {
    /* libgl could not set it and fell back: back to the old one */
    char m[48];
    mx_snprintf(m, sizeof m, "%s does not run here", vmodes[vm].key);
    char *sp = strchr(before, ' ');
    if (sp)
      *sp = 0;
    video_put(before, sp ? sp + 1 : "component");
    toast(N_WARN, "Mode not available", m, 1);
    return;
  }
  char now[24], conf[MX_SETTING_VALUE];
  now_text(now, sizeof now);
  confirmed_text(conf, sizeof conf);
  trial = strcmp(now, conf) != 0;
  trial_until = ticks + (u32)(15.0f / frame_dt);
}

/* kept: video.confirm, and the mode one this TV shows */
static void video_keep(void) {
  char now[24], list[MX_SETTING_VALUE] = "";
  now_text(now, sizeof now);
  cfg_set("video.confirm", now);
  struct mx_screen_conf c;
  video_conf(&c);
  for (int m = 1; m < MX_SCREEN_MODES; m++)
    if (c.allowed >> m & 1) {
      u32 l = (u32)strlen(list);
      mx_snprintf(list + l, sizeof list - l, "%s%s", l ? " " : "", mx_screen_name((u32)m));
    }
  cfg_set("video.allowed", list);
  cfg_save();
  trial = 0;
}

/* a mode taken off video.allowed (not the region's, not the one in use) */
static void allowed_remove(u32 mode) {
  struct mx_screen_conf c;
  video_conf(&c);
  char list[MX_SETTING_VALUE] = "";
  for (u32 m = 1; m < MX_SCREEN_MODES; m++)
    if (c.allowed >> m & 1 && m != mode) {
      u32 l = (u32)strlen(list);
      mx_snprintf(list + l, sizeof list - l, "%s%s", l ? " " : "", mx_screen_name(m));
    }
  cfg_set("video.allowed", list);
  cfg_save();
}

/* the kernel put the confirmed mode back (its 15 s, L1 L2 R1 R2): no question */
void video_trial_check(void) {
  if (!trial)
    return;
  char now[24], conf[MX_SETTING_VALUE];
  now_text(now, sizeof now);
  confirmed_text(conf, sizeof conf);
  if (!strcmp(now, conf))
    trial = 0;
}

/* tests: Video's list l open (1 the shell's picture, 2 the modes) */
void settings_vlist(int l) { vlist = l; }

/* tests: in the page, row n chosen */
void settings_pick(int n) {
  in_page = 1, item = n;
  if (page == P_PADS) { /* tests: row n of the list, opened */
    pads_enter();
    for (int i = 0; i < n; i++)
      pads_input(MX_PAD_DOWN);
    pads_input(MX_PAD_CROSS);
  }
}

void settings_enter(void) {
  screen = SCR_SETTINGS;
  fam_of(video_choice(), &vfam, &vsub);
  in_page = 0, item = 0, edit = 0, tedit = 0, vlist = VL_MAIN;
  ip_load();
}

/* the rows of a page, as in the mockups: label left, value right, a line
   under each; the chosen one lit. y is the text's baseline; the row is
   pitch high about the middle of its text (text_mid), its line at the
   bottom, and the chosen one's light fills it from the line above (its
   lamp line over that one) to its own */
#define ROW 54.0f
static float pitch = ROW;
void row_line(float x0, float x1, float y, const char *label, const char *value, int chosen, int on) {
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

void row_note(float x, float y, const char *s) { d_text(FONT_SMALL, x, y, s, argb(C_FAINT, 1)); }

/* choices side by side at the right, the chosen one a pill; returns the left edge */
float row_seg(float x1, float y, const char *const *l, int n, int on, int lit) {
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

/* a value at the right of a row, and the arrow of a row that opens a list */
static void vvalue(float x0, float x1, float y, const char *v, int chosen, int opens) {
  float w = opens ? 22 : 0;
  if (opens)
    d_text(FONT_BODY, x1 - 12, y, ">", argb(C_FAINT, 1));
  if (v)
    d_text_fit(FONT_SMALL, FONT_SMALL, x1 - w - fminf(d_text_w(FONT_SMALL, v), x1 - x0 - 300), y, x1 - x0 - 300, v,
               argb(chosen ? C_WHITE : C_DIM, 1));
}

static const char *const seg_onoff[2] = {"On", "Off"};

/* the shell's own picture: colour, dithering, the flicker filter */
static void video_row_picture(float x0, float x1, float yy, int what, int chosen) {
  int sd = video_mode == VM_480I || video_mode == VM_576I;
  switch (what) {
  case VR_COLOUR: {
    static const char *const l[2] = {"32-bit where it fits", "16-bit"};
    int hd = VM_HD(video_mode);
    vrow(x0, x1, yy, "Colour", hd ? "HD is always 16-bit: 32 does not fit" : 0, chosen);
    if (hd)
      row_seg(x1, yy, l + 1, 1, 0, chosen);
    else
      row_seg(x1, yy, l, 2, cfg_get("video.depth", "32")[0] == '3' ? 0 : 1, chosen);
    break;
  }
  case VR_DITHER: {
    const char *l[1] = {dither_name(dither_choice())};
    vrow(x0, x1, yy, "Dithering", "16-bit colour", chosen);
    row_seg(x1, yy, l, 1, 0, chosen);
    break;
  }
  case VR_DTEST:
    vrow(x0, x1, yy, "Dithering test picture", 0, chosen);
    vvalue(x0, x1, yy, 0, chosen, 1);
    break;
  case VR_FLICKER: {
    static const char *const l[2] = {"Off", "On"};
    vrow(x0, x1, yy, "Flicker filter", sd ? "Softer, against flicker on interlaced lines" : "480i and 576i only",
         chosen);
    row_seg(x1, yy, l, 2, video_deflicker(), chosen);
    break;
  }
  }
}

static void page_video(float x0, float x1, float y) {
  struct mx_screen_conf c;
  video_conf(&c);
  int use = video_of_screen(c.mode), uf, us;
  fam_of(use, &uf, &us);
  int r[16], n = vrows(r), cur = in_page ? r[item < n ? item : 0] : -1;
  int hd = VM_HD(use);
  if (vlist != VL_MAIN) {
    /* a list of its own: under the page's title, what it is */
    d_label(x0, y - 2, vlist == VL_PICTURE ? "Video" DOT "the shell's picture" : "Video" DOT "modes this TV shows",
            argb(C_LAMP, 1));
    float yy = y + 56;
    for (int i = 0; i < n; i++, yy += 54) {
      int chosen = cur == r[i];
      if (r[i] < VR_MODE) {
        video_row_picture(x0, x1, yy, r[i], chosen);
        continue;
      }
      u32 m = (u32)(r[i] - VR_MODE);
      int vm = video_of_screen(m);
      const char *sub = vm == region_sd() ? "The console's own: always" : m == c.mode ? "In use" : 0;
      vrow(x0, x1, yy, vmodes[vm].name, sub, chosen);
    }
    if (vlist == VL_ALLOWED) {
      row_note(x0, yy + 6, "A mode is added when you keep it.");
      row_note(x0, yy + 32, "Programs set to Native may use these.");
    }
    return;
  }
  /* the modes: four cards, the one being chosen lit, the one in use marked */
  float gap = 12, cw = (x1 - x0 - gap * 3) / 4, ch = 74, cy = y - 12;
  for (int f = 0; f < NFAMS; f++) {
    float cx = x0 + (cw + gap) * f;
    int on = f == vfam, lit = on && cur == VR_FAMILY, off = c.rgb && needs_component(fams[f].mode[0]);
    d_highlight(cx, cy, cx + cw, cy + ch, 10, on ? (lit ? 0.12f : 0.08f) : 0.035f, on ? 0.04f : 0.015f,
                on ? (lit ? 1 : 0.5f) : 0);
    /* its name: the standard chosen (this card's), in use, or the region's */
    int k = f == vfam ? vsub : f == uf ? us : fams[f].n > 1 && region_sd() == VM_576I;
    d_text_fit(FONT_BUTTON, FONT_SMALL, cx + 12, cy + 34, cw - 20, vmodes[fams[f].mode[k]].key,
               argb(off ? C_FAINT : on ? C_WHITE : C_DIM, 1));
    const char *sub = f == uf ? "In use" : off ? "Component only" : fams[f].size;
    if (f == uf || off || cw >= 150) /* 4:3: the sizes do not fit, only "In use" */
      d_text_fit(FONT_SMALL, FONT_SMALL, cx + 12, cy + 60, cw - 20, sub, argb(f == uf ? C_LAMP : C_FAINT, 1));
  }
  float yy = cy + ch + 44, step = 54;
  for (int i = 1; i < n; i++, yy += step) {
    int chosen = cur == r[i];
    switch (r[i]) {
    case VR_STD: {
      int m = fams[vfam].mode[vsub];
      char l[64];
      mx_snprintf(l, sizeof l, "%s" DOT "not in use yet", vmodes[m].key);
      vrow(x0, x1, yy, "Standard", m == use ? 0 : l, chosen);
      row_seg(x1, yy, fams[vfam].sub, fams[vfam].n, vsub, chosen);
      break;
    }
    case VR_OUT: {
      static const char *const l[2] = {"Component", "RGB"};
      vrow(x0, x1, yy, "Output", c.rgb ? "SCART, composite, S-video: 480i, 576i, 240p, 288p" : "Component (YPbPr): every mode",
           chosen);
      row_seg(x1, yy, l, 2, c.rgb, chosen);
      break;
    }
    case VR_TV: {
      static const char *const l[2] = {"4:3", "16:9"};
      vrow(x0, x1, yy, "TV shape", hd ? "Not HD: that is always 16:9" : "The shape of the picture", chosen);
      row_seg(x1, yy, l, 2, hd ? 1 : c.wide, chosen);
      break;
    }
    case VR_OVERSCAN:
      vrow(x0, x1, yy, "Overscan", hd ? "Not HD" : "Does the TV show the whole picture?", chosen);
      row_seg(x1, yy, seg_onoff, 2, c.full, chosen);
      break;
    case VR_SCALE: {
      static const char *const l[2] = {"Stretch", "Native"};
      vrow(x0, x1, yy, "Programs",
           c.native ? "In a mode of their own where this TV shows it" : "All in this mode", chosen);
      row_seg(x1, yy, l, 2, c.native, chosen);
      break;
    }
    case VR_ALLOWED: {
      char l[112] = "";
      for (int vm = 0; vm < VM_MODES; vm++)
        if (c.allowed >> mx_screen_mode_of(vmodes[vm].key) & 1) {
          u32 k = (u32)strlen(l);
          mx_snprintf(l + k, sizeof l - k, "%s%s", k ? "  " : "", vmodes[vm].key);
        }
      vrow(x0, x1, yy, "Modes this TV shows", 0, chosen);
      vvalue(x0, x1, yy, l, chosen, 1);
      break;
    }
    case VR_PICTURE: {
      char l[48];
      int rgb16 = hd || cfg_get("video.depth", "32")[0] != '3';
      mx_snprintf(l, sizeof l, "%s%s", rgb16 ? "16-bit" DOT : "32-bit", rgb16 ? dither_name(dither_choice()) : "");
      vrow(x0, x1, yy, "The shell's picture", 0, chosen);
      vvalue(x0, x1, yy, l, chosen, 1);
      break;
    }
    }
  }
}

/* an address at the right of a row; part (1-4) underlined while edited */
static void addr_value(float x1, float y, const u8 *a, int chosen, int part) {
  char v[24];
  mx_snprintf(v, sizeof v, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  float px = x1 - d_text_w(FONT_BODY, v);
  d_text(FONT_BODY, px, y, v, argb(chosen ? C_WHITE : C_DIM, 1));
  if (!part)
    return;
  char t[24];
  for (int k = 0; k < part - 1; k++) {
    mx_snprintf(t, sizeof t, "%u.", a[k]);
    px += d_text_w(FONT_BODY, t);
  }
  mx_snprintf(t, sizeof t, "%u", a[part - 1]);
  d_rect(px, y + 8, px + d_text_w(FONT_BODY, t), y + 10, argb(C_LAMP, 1), argb(C_LAMP, 1));
}

static void page_network(float x0, float x1, float y) {
  struct mx_net_status ns;
  int have = mx_net_status(&ns) == 0 && ns.up, dhcp = net_dhcp();
  /* the status */
  char v[64], sub[64];
  if (!have)
    mx_snprintf(v, sizeof v, "Not started");
  else if (!ns.link)
    mx_snprintf(v, sizeof v, "No cable");
  else
    mx_snprintf(v, sizeof v, "Connected" DOT "%u Mbit/s%s", ns.speed, ns.duplex ? ", full duplex" : "");
  sub[0] = 0;
  if (have)
    mx_snprintf(sub, sizeof sub, "MAC %02x:%02x:%02x:%02x:%02x:%02x", ns.mac[0], ns.mac[1], ns.mac[2], ns.mac[3],
                ns.mac[4], ns.mac[5]);
  vrow(x0, x1, y, "Status", have ? sub : "The network driver is not there", 0);
  d_text(FONT_BODY, x1 - d_text_w(FONT_BODY, v), y, v, argb(have && ns.link ? C_LAMP : C_DIM, 1));

  /* how the address is set */
  float yy = y + ROW;
  int chosen = in_page && item == NR_MODE;
  if (dhcp && have && ns.dhcp == MX_NET_DHCP_BOUND && ns.lease) {
    u32 h = ns.lease / 3600, m = ns.lease / 60 % 60;
    if (h)
      mx_snprintf(sub, sizeof sub, "From the network (DHCP)" DOT "%u h %u min left", h, m);
    else
      mx_snprintf(sub, sizeof sub, "From the network (DHCP)" DOT "%u min left", m);
  } else if (dhcp)
    mx_snprintf(sub, sizeof sub, "%s", have && ns.dhcp == MX_NET_DHCP_SEARCHING ? "Asking the network for one"
                                                                                 : "From the network (DHCP)");
  else
    mx_snprintf(sub, sizeof sub, "Set here");
  static const char *const l[2] = {"Automatic", "Manual"};
  vrow(x0, x1, yy, "Address", sub, chosen);
  row_seg(x1, yy, l, 2, !dhcp, chosen);

  /* the addresses: in use (automatic), or the ones set here */
  static const char *const names[NR_USE] = {0, "IP address", "Subnet mask", "Gateway", "DNS server"};
  for (int r = NR_IP; r < NR_USE; r++) {
    yy += ROW;
    chosen = in_page && !dhcp && item == r;
    vrow(x0, x1, yy, names[r], 0, chosen);
    if (dhcp) {
      u32 a = !have ? 0 : r == NR_IP ? ns.ip : r == NR_MASK ? ns.mask : r == NR_GW ? ns.gw : ns.dns;
      u8 o[4];
      addr_of(a, o);
      if (a)
        addr_value(x1, yy, o, 0, 0);
      else
        d_text(FONT_BODY, x1 - d_text_w(FONT_BODY, "None"), yy, "None", argb(C_FAINT, 1));
    } else
      addr_value(x1, yy, ip[r], chosen, chosen && edit ? edit : 0);
  }
  yy += ROW;
  if (!dhcp) {
    chosen = in_page && item == NR_USE;
    const char *u = ip_dirty ? "Use these addresses" : "In use";
    float mid = yy - text_mid(FONT_BODY);
    if (chosen)
      d_highlight(x0 - 18, mid - 26, x1 + 18, mid + 26, 10, 0.07f, 0.03f, 0.5f);
    d_text(FONT_BODY, x0, yy, u, argb(ip_dirty ? C_WHITE : C_FAINT, 1));
    yy += ROW;
  }
  row_note(x0, yy - 20, dhcp ? "A PC's link (ps2client) uses the address above." : "A new address is used a second after it is set.");
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
    row_line(x0, x1, y + ROW * d, l, val, in_page && item == d, 1);
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
  row_line(x0, x1, y + ROW * ys, "Swap file", 0, chosen, 1);
  row_seg(x1, y + ROW * ys, ch, nch, on, chosen);
  row_line(x0, x1, y + ROW * (ys + 1), "Games from the PC (host:)", cfg_get("dev.host", "0")[0] == '1' ? "On" : "Off",
       in_page && item == ys + 1, 1);
  float ny = y + ROW * (ys + 2);
  char m[96];
  mx_snprintf(m, sizeof m, "Swap file: %s%s%s.", si.path[0] ? si.path : "none", si.path[0] ? DOT : "",
              si.path[0] ? state[si.state < 5 ? si.state : 0] : "");
  row_note(x0, ny, m);
  if (!si.path[0])
    d_text(FONT_SMALL, x0, ny + 26, "Without a swap file MXOS runs out of memory soon.", argb(C_WARN, 1));
  else
    row_note(x0, ny + 26, "Cross on a device: ready to remove. Games are in <device>/PS2MAX/.");
}

void settings_goto(int pg, int swap_row) {
  settings_enter();
  page = pg, in_page = 1, edit = 0;
  pads_enter();
  st_gen = ~0u;
  st_read();
  item = pg == P_STORAGE && swap_row ? st_n : 0;
  screen = SCR_SETTINGS;
}

/* ---- Date and time (kernel version 29): the date and time set (also the
   PS2's clock), the zone, summer time, the clock's format ---- */

static int zone_index(void) {
  int z = clock_zone(), best = 0;
  for (int i = 0; i < clock_nzones; i++)
    if (clock_zones[i].min <= z)
      best = i;
  return best;
}

static void zone_text(int min, char *s, u32 n) {
  int a = min < 0 ? -min : min;
  mx_snprintf(s, n, "UTC%c%02d:%02d", min < 0 ? '-' : '+', a / 60, a % 60);
}

/* the date and time at the right of a row: "Mon 16 Nov 2026  14:05", the
   field being changed underlined */
static void date_value(float x1, float y, const struct mx_tm *tm, int chosen, int field) {
  char p[6][16];
  int f[6] = {0, TF_DAY, TF_MONTH, TF_YEAR, TF_HOUR, TF_MINUTE};
  mx_snprintf(p[0], 16, "%s ", clock_wday[clock_weekday(tm->year, tm->month, tm->day)]);
  mx_snprintf(p[1], 16, "%u", tm->day);
  mx_snprintf(p[2], 16, "%s", clock_month[tm->month - 1]);
  mx_snprintf(p[3], 16, "%u", tm->year);
  int h12 = !clock_24h();
  mx_snprintf(p[4], 16, "%u", h12 ? (tm->hour % 12 ? tm->hour % 12 : 12) : tm->hour);
  mx_snprintf(p[5], 16, "%02u%s", tm->minute, h12 ? (tm->hour < 12 ? " AM" : " PM") : "");
  static const char *const gap[6] = {"", " ", " ", " ", "   ", ":"}; /* before each part */
  float w = 0;
  for (int i = 0; i < 6; i++)
    w += d_text_w(FONT_BODY, gap[i]) + d_text_w(FONT_BODY, p[i]);
  float x = x1 - w;
  u32 c = argb(chosen ? C_WHITE : C_DIM, 1);
  for (int i = 0; i < 6; i++) {
    x = d_text(FONT_BODY, x, y, gap[i], c);
    float x2 = d_text(FONT_BODY, x, y, p[i], c);
    if (field && f[i] == field) {
      float uw = i == 5 && h12 ? d_text_w(FONT_BODY, "00") : x2 - x;
      d_rect(x, y + 8, x + uw, y + 10, argb(C_LAMP, 1), argb(C_LAMP, 1));
    }
    x = x2;
  }
}

static void page_time(float x0, float x1, float y) {
  struct mx_tm now;
  int known = clock_now(&now) == 0;
  int chosen = in_page && item == TR_DATE;
  if (tedit)
    vrow(x0, x1, y, "Date and time", "Being set", chosen);
  else
    vrow(x0, x1, y, "Date and time", known ? 0 : "Not known: set it", chosen);
  if (tedit)
    date_value(x1, y, &tset, 1, tedit);
  else if (known)
    date_value(x1, y, &now, chosen, 0);
  else
    vvalue(x0, x1, y, "Not set", chosen, 0);

  float yy = y + ROW;
  int zi = zone_index();
  char z[16];
  zone_text(clock_zones[zi].min, z, sizeof z);
  chosen = in_page && item == TR_ZONE;
  vrow(x0, x1, yy, "Time zone", clock_zones[zi].place, chosen);
  d_text(FONT_BODY, x1 - d_text_w(FONT_BODY, z), yy, z, argb(chosen ? C_WHITE : C_DIM, 1));

  yy += ROW;
  chosen = in_page && item == TR_DST;
  vrow(x0, x1, yy, "Summer time", "An hour later", chosen);
  row_seg(x1, yy, seg_onoff, 2, !clock_dst(), chosen);

  yy += ROW;
  static const char *const fmt[2] = {"24-hour", "12-hour"};
  chosen = in_page && item == TR_FORMAT;
  vrow(x0, x1, yy, "Clock", 0, chosen);
  row_seg(x1, yy, fmt, 2, !clock_24h(), chosen);
  row_note(x0, yy + ROW - 20, "The PS2's own clock keeps the time, also when it is off.");
}

static void time_set(void) {
  tset.second = 0;
  if (mx_time_set(clock_utc_of(&tset)) < 0)
    toast(N_WARN, "Time not set", "The PS2's clock could not be written", 1);
}

/* the date and time being set: left and right choose a field, up and down
   change it, cross sets it, circle leaves it */
static void time_edit_input(u32 p) {
  if (p & MX_PAD_LEFT && tedit > TF_DAY)
    tedit--;
  if (p & MX_PAD_RIGHT && tedit < TF_MINUTE)
    tedit++;
  int d = 0;
  if (pad_held & MX_PAD_UP && (p & MX_PAD_UP || ticks % 4 == 0))
    d = 1;
  if (pad_held & MX_PAD_DOWN && (p & MX_PAD_DOWN || ticks % 4 == 0))
    d = -1;
  if (d) {
    int y = tset.year, m = tset.month, dd = tset.day, h = tset.hour, mi = tset.minute;
    switch (tedit) {
    case TF_DAY: dd = (dd - 1 + d + clock_mdays(y, m)) % clock_mdays(y, m) + 1; break;
    case TF_MONTH: m = (m - 1 + d + 12) % 12 + 1; break;
    case TF_YEAR: y = y + d < 2000 ? 2099 : y + d > 2099 ? 2000 : y + d; break;
    case TF_HOUR: h = (h + d + 24) % 24; break;
    case TF_MINUTE: mi = (mi + d + 60) % 60; break;
    }
    if (dd > clock_mdays(y, m))
      dd = clock_mdays(y, m);
    tset.year = (u16)y, tset.month = (u8)m, tset.day = (u8)dd, tset.hour = (u8)h, tset.minute = (u8)mi;
  }
  if (p & MX_PAD_CROSS)
    time_set(), tedit = 0;
  if (p & MX_PAD_CIRCLE)
    tedit = 0;
}

static void time_input(u32 p) {
  int d = p & MX_PAD_RIGHT ? 1 : p & MX_PAD_LEFT ? -1 : 0, x = p & MX_PAD_CROSS;
  switch (item) {
  case TR_DATE:
    if (x) {
      if (clock_now(&tset) < 0) {
        memset(&tset, 0, sizeof tset);
        tset.year = 2026, tset.month = 1, tset.day = 1, tset.hour = 12;
      }
      tedit = TF_DAY;
    }
    break;
  case TR_ZONE:
    if (d) {
      int i = zone_index() + d;
      if (i >= 0 && i < clock_nzones) {
        char v[8];
        mx_snprintf(v, sizeof v, "%d", clock_zones[i].min);
        cfg_set("time.zone", v);
        cfg_save();
      }
    }
    break;
  case TR_DST:
    if (d || x)
      toggle("time.dst", "on", "off", "off");
    break;
  case TR_FORMAT:
    if (d || x)
      toggle("clock.format", "12", "24", "24");
    break;
  }
}

static void page_system(float x0, float x1, float y) {
  row_line(x0, x1, y, "Shell", "MXSHELL2 0.2", 0, 1);
  char v[64] = "Not there";
  const struct mxk_services *k = &mx__kernel ? mx__kernel : 0;
  if (k)
    mx_snprintf(v, sizeof v, "Version %u", k->version);
  row_line(x0, x1, y + ROW, "Kernel", v, 0, 1);
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
  row_line(x0, x1, y + ROW * 2, "Services", buf[0] ? buf : "None", 0, 1);
}

/* a question over the page: a title, a line, cross and circle */
static void ask_draw(const char *title, const char *line, const char *yes, const char *no) {
  float w = 560, h = 190, x = (SCR_W - w) / 2, y = 250;
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.6f), argb(C_INK, 0.6f));
  d_shadow(x + w / 2, y + h / 2, w * 0.7f, h * 0.9f, 0.5f);
  d_card_lit(x, y, x + w, y + h, 14, 0x17191E, 1, 0.55f);
  d_text(FONT_TITLE, x + 32, y + 58, title, argb(C_WHITE, 1));
  d_text_fit(FONT_BODY, FONT_SMALL, x + 32, y + 92, w - 64, line, argb(C_DIM, 1));
  float bx = x + 32;
  aa_under(0x17191E);
  bx += d_button(bx, y + 118, SYM_CROSS, yes, 1) + 12;
  d_button(bx, y + 118, SYM_CIRCLE, no, 0);
  aa_under_room();
}

static void confirm_draw(void) {
  char m[80];
  struct mx_screen_conf c;
  video_conf(&c);
  int left = (int)((float)(trial_until - ticks) * frame_dt) + 1;
  mx_snprintf(m, sizeof m, "%s%s. The old one comes back in %d s.", mx_screen_name(c.mode), c.rgb ? " over RGB" : "",
              left > 0 ? left : 0);
  ask_draw("Keep this video mode?", m, "Keep", "Go back");
}

void settings_draw(void) {
  if (trial && (int)(ticks - trial_until) >= 0)
    video_back();
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
  case P_PADS: pads_draw(x0, x1, y + 28, in_page, 0); break;
  case P_TIME: page_time(x0, x1, y); break;
  default: page_system(x0, x1, y); break;
  }

  static const char *const h_list[] = {SYM_CROSS, "Open", "L1", "Section", SYM_CIRCLE, "Back", 0};
  static const char *const h_page[] = {SYM_CROSS, "Change", SYM_CIRCLE, "Back", 0};
  static const char *const h_edit[] = {"L R", "Number", "UP DOWN", "Change", SYM_CROSS, "Done", 0};
  static const char *const h_list2[] = {SYM_CROSS, "Remove", SYM_CIRCLE, "Back", 0};
  static const char *const h_try[] = {"L R", "Choose", SYM_CROSS, "Try it", SYM_CIRCLE, "Back", 0};
  static const char *const h_test[] = {"L R", "On, off", SYM_CROSS, "Test frame", SYM_CIRCLE, "Back", 0};
  static const char *const h_open[] = {SYM_CROSS, "Open", SYM_CIRCLE, "Back", 0};
  static const char *const h_choose[] = {"L R", "Choose", SYM_CIRCLE, "Back", 0};
  static const char *const h_tedit[] = {"L R", "Field", "UP DOWN", "Change", SYM_CROSS, "Set", SYM_CIRCLE, "Cancel", 0};
  const char *const *h_video = h_page;
  if (in_page && page == P_VIDEO) {
    int r[16], n = vrows(r), w = n > 0 ? r[item < n ? item : n - 1] : -1;
    struct mx_screen_conf c;
    video_conf(&c);
    u32 m = w >= VR_MODE ? (u32)(w - VR_MODE) : 0;
    int removable = m && m != c.mode && video_of_screen(m) != region_sd();
    h_video = vlist == VL_ALLOWED ? (removable ? h_list2 : h_open + 2)
              : w == VR_FAMILY || w == VR_STD ? h_try
              : w == VR_OVERSCAN ? h_test
              : w == VR_ALLOWED || w == VR_PICTURE || w == VR_DTEST ? h_open
                                                                    : h_page;
  }
  if (trial)
    confirm_draw();
  else if (os_ask)
    ask_draw("Did you see all of the frame?", "Its white line at all four edges of the screen.", "Yes: overscan off",
             "No: keep it on");
  else
    hints_draw(edit ? h_edit
               : tedit ? h_tedit
               : in_page && page == P_TIME && (item == TR_ZONE || item == TR_DST || item == TR_FORMAT) ? h_choose
               : in_page && page == P_PADS ? pads_hints("Back")
               : in_page && page == P_VIDEO ? h_video
               : in_page && page == P_NETWORK && item == NR_MODE ? h_choose
               : in_page ? h_page
                         : h_list);
  status_draw();
}

/* the row of the main list that opened a list */
static int row_of(int what) {
  int r[16], n;
  vlist = VL_MAIN;
  n = vrows(r);
  for (int i = 0; i < n; i++)
    if (r[i] == what)
      return i;
  return 0;
}

static void toggle(const char *key, const char *a, const char *b, const char *def) {
  cfg_set(key, strcmp(cfg_get(key, def), a) ? a : b);
  cfg_save();
}

/* the Video page: left and right change a row's choice, cross uses it */
static void video_input(u32 p) {
  int r[16], n = vrows(r);
  if (item >= n)
    item = n - 1;
  int d = p & MX_PAD_RIGHT ? 1 : p & MX_PAD_LEFT ? -1 : 0, x = p & MX_PAD_CROSS;
  struct mx_screen_conf c;
  video_conf(&c);
  int hd = VM_HD(video_of_screen(c.mode));
  if (p & MX_PAD_CIRCLE) {
    if (vlist == VL_MAIN)
      in_page = 0;
    else
      item = row_of(vlist == VL_PICTURE ? VR_PICTURE : VR_ALLOWED);
    return;
  }
  if (n <= 0)
    return;
  int what = r[item];
  if (what >= VR_MODE) {
    u32 m = (u32)(what - VR_MODE);
    int vm = video_of_screen(m);
    if (x && vm != region_sd() && m != c.mode) {
      allowed_remove(m);
      vrows(r);
      n = vrows(r);
      if (item >= n)
        item = n - 1;
    }
    return;
  }
  switch (what) {
  case VR_FAMILY:
  case VR_STD:
    if (d && what == VR_FAMILY && vfam + d >= 0 && vfam + d < NFAMS) {
      int uf, us;
      fam_of(video_of_screen(c.mode), &uf, &us);
      vfam += d;
      /* in use: its own standard; else the console's region */
      vsub = vfam == uf ? us : fams[vfam].n > 1 && region_sd() == VM_576I ? 1 : 0;
    }
    if (d && what == VR_STD && vsub + d >= 0 && vsub + d < fams[vfam].n)
      vsub += d;
    if (x) {
      int vm = fams[vfam].mode[vsub];
      if (c.rgb && needs_component(vm))
        toast(N_WARN, "Needs component", "Set Output to Component first", 1);
      else
        video_try(vm, c.rgb);
    }
    break;
  case VR_OUT:
    if (d || x) {
      int rgb = !c.rgb, vm = video_of_screen(c.mode);
      if (rgb && needs_component(vm))
        vm = region_sd(); /* RGB carries 480i, 576i, 240p and 288p only */
      video_try(vm, rgb);
      fam_of(video_choice(), &vfam, &vsub);
    }
    break;
  case VR_TV:
    if ((d || x) && !hd) {
      toggle("video.aspect", "16:9", "4:3", "4:3");
      video_apply();
    }
    break;
  case VR_OVERSCAN:
    if (d && !hd)
      toggle("video.overscan", "off", "on", "on");
    if (x && !hd && overscan_test() == 0)
      os_ask = 1;
    break;
  case VR_SCALE:
    if (d || x)
      toggle("video.scale", "native", "stretch", "stretch");
    break;
  case VR_ALLOWED:
  case VR_PICTURE:
    if (x)
      vlist = what == VR_ALLOWED ? VL_ALLOWED : VL_PICTURE, item = 0;
    break;
  case VR_COLOUR:
    if ((d || x) && !hd) {
      toggle("video.depth", "16", "32", "32");
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
  case VR_FLICKER:
    if (d || x) {
      toggle("video.deflicker", "on", "off", "off");
      video_apply();
    }
    break;
  }
}

void settings_input(void) {
  u32 p = pad_pressed;
  if (trial) {
    if (p & MX_PAD_CROSS)
      video_keep();
    else if (p & MX_PAD_CIRCLE)
      video_back();
    return;
  }
  if (os_ask) { /* after the overscan test: did the TV show all of it? */
    if (p & (MX_PAD_CROSS | MX_PAD_CIRCLE)) {
      cfg_set("video.overscan", p & MX_PAD_CROSS ? "off" : "on");
      cfg_save();
      os_ask = 0;
    }
    return;
  }
  if (!in_page) {
    if (p & MX_PAD_UP && page > 0)
      page--;
    if (p & MX_PAD_DOWN && page + 1 < NPAGES)
      page++;
    if (p & (MX_PAD_CROSS | MX_PAD_RIGHT) && nitems()) {
      in_page = 1, item = 0, vlist = VL_MAIN, fam_of(video_choice(), &vfam, &vsub);
      if (page == P_PADS)
        pads_enter();
    }
    /* L1: the section before (Settings is the last); circle: Home */
    if (p & (MX_PAD_CIRCLE | MX_PAD_L1))
      screen = SCR_HOME;
    return;
  }
  if (tedit) {
    time_edit_input(p);
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
  if (page == P_PADS) {
    if (!pads_input(p))
      in_page = 0;
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
  if (page == P_TIME && (p & (MX_PAD_RIGHT | MX_PAD_CROSS) || (p & MX_PAD_LEFT && item != TR_DATE))) {
    time_input(p);
    return;
  }

  if (page == P_NETWORK && item == NR_MODE && p & (MX_PAD_LEFT | MX_PAD_RIGHT | MX_PAD_CROSS)) {
    /* Automatic, or Manual with the addresses in use now (the address
       stays until it is changed here) */
    int manual = p & MX_PAD_CROSS ? net_dhcp() : !!(p & MX_PAD_RIGHT);
    if (manual == !net_dhcp())
      return;
    if (manual) {
      struct mx_net_status ns;
      if (mx_net_status(&ns) == 0 && ns.ip) {
        addr_of(ns.ip, ip[NR_IP]), addr_of(ns.mask, ip[NR_MASK]), addr_of(ns.gw, ip[NR_GW]);
        addr_of(ns.dns, ip[NR_DNS]);
      }
      ip_save();
    } else {
      cfg_set("net.mode", "dhcp");
      cfg_save();
    }
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
    if (item >= NR_IP && item < NR_USE)
      edit = 1;
    else if (item == NR_USE && ip_dirty)
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
