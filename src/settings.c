/* MXSHELL2: Settings, with what the OS offers today (PLAN.md, phase A):
     Video        the shell's mode: the console's own, NTSC or PAL (libgl's modes)
     Network      IPCONFIG.DAT on the card: the kernel reads it at its start
     Storage      where the shell looks for games, and what it found
     Controllers  which ports have a pad; the motors tried
     System       the shell, the kernel, the services the kernel starts
   The rest (battery, light colour, network status, time) comes with the
   OS's parts (O1-O8). */
#include <mx/debug.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/video.h>
#include <math.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* only libmxk has the kernel's table (the standalone build: none) */
#pragma weak mx__kernel

enum { P_VIDEO, P_NETWORK, P_STORAGE, P_PADS, P_SYSTEM, NPAGES };
static const char *const page_name[NPAGES] = {"Video", "Network", "Storage", "Controllers", "System"};
static int page, in_page, item, edit;
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

/* ---- the items of each page ---- */
static int nitems(void) {
  switch (page) {
  case P_VIDEO: return VM_MODES + 5;
  case P_NETWORK: return 4;
  case P_STORAGE: return NDEVS + 1;
  case P_PADS: return 2;
  default: return 0;
  }
}

/* the modes: the setting's key, its name, libgl's mode, the shape of its
   pixels (the font atlas), 16:9, frames a second */
static const struct {
  const char *key, *name;
  int gl, shape, wide, hz;
} vmodes[VM_MODES] = {
    [VM_1080I] = {"1080i", "1080i  1920 x 1080, 60 Hz", MXGL_1080I, SHAPE_HD, 1, 60},
    [VM_1080I50] = {"1080i50", "1080i  1920 x 1080, 50 Hz", MXGL_1080I50, SHAPE_HD, 1, 50},
    [VM_1080P30] = {"1080p30", "1080p  1920 x 1080, 30 Hz", MXGL_1080P30, SHAPE_HD540, 1, 30},
    [VM_720P] = {"720p", "720p  1280 x 720, 60 Hz", MXGL_720P, SHAPE_P720, 1, 60},
    [VM_720P50] = {"720p50", "720p  1280 x 720, 50 Hz", MXGL_720P50, SHAPE_P720, 1, 50},
    [VM_576P] = {"576p", "576p  640 x 576, 50 Hz", MXGL_576P, SHAPE_P576, 0, 50},
    [VM_480P] = {"480p", "480p  640 x 480, 60 Hz", MXGL_480P, SHAPE_P480, 0, 60},
    [VM_NTSC] = {"ntsc", "NTSC  640 x 448, 60 Hz", MXGL_NTSC, SHAPE_NTSC, 0, 60},
    [VM_PAL] = {"pal", "PAL  640 x 512, 50 Hz", MXGL_PAL, SHAPE_PAL, 0, 50},
};
int video_mode = VM_1080I, video_shape = SHAPE_HD;
float scr_w = 1280, px_x = 1.5f, px_y = 1.5f;

/* video.aspect: the television's shape in the SD modes (NTSC, PAL, 480p,
   576p), 4:3 or 16:9 (anamorphic: the same 640 pixels across a wide
   screen); HD is always 16:9 */
static int sd_wide(void) { return !strcmp(cfg_get("video.aspect", "4:3"), "16:9"); }

/* video.deflicker: in NTSC and PAL, a softer picture against flicker
   (libgl's mxgl_deflicker: the PS2's two read circuits a line apart,
   blended). It needs full-height buffers, so those modes are then drawn
   without beam racing (16-bit colour); off, beam racing draws every line of
   the picture, for the television's deinterlacer */
static int deflicker(void) { return !strcmp(cfg_get("video.deflicker", "off"), "on"); }

static int video_choice(void) {
  const char *v = cfg_get("video.mode", "1080i");
  for (int i = 0; i < VM_MODES; i++)
    if (!strcmp(v, vmodes[i].key))
      return i;
  /* "sd": the console's own, from its region */
  return mx_video_default() == &mx_video_pal ? VM_PAL : VM_NTSC;
}

/* the mode from the settings: video.mode (a key of vmodes, or sd), and
   video.depth (32: 24-bit colour where it fits, 16: dithered) */
void video_apply(void) {
  int m = video_choice();
  int laced = m == VM_NTSC || m == VM_PAL, soft = laced && deflicker();
  int flags = cfg_get("video.beam", "1")[0] == '1' && !soft ? MXGL_BEAM : 0;
  if (flags && cfg_get("video.depth", "32")[0] == '3')
    flags |= MXGL_RGB24;
  mxgl_set_mode(vmodes[m].gl | flags);
  mxgl_deflicker(soft);
  /* libgl falls back to the console's own mode for one it cannot set */
  int got = mxgl_mode();
  video_mode = m;
  if (got != vmodes[m].gl)
    video_mode = got == MXGL_PAL ? VM_PAL : VM_NTSC;
  video_shape = vmodes[video_mode].shape;
  int wide = vmodes[video_mode].wide;
  if (!wide && sd_wide()) {
    static const int w16[] = {[SHAPE_P576] = SHAPE_P576W, [SHAPE_P480] = SHAPE_P480W, [SHAPE_NTSC] = SHAPE_NTSCW,
                              [SHAPE_PAL] = SHAPE_PALW};
    video_shape = w16[video_shape], wide = 1;
  }
  int w, h;
  mxgl_screen(&w, &h);
  scr_w = wide ? 1280.0f : 960.0f;
  px_x = (float)w / scr_w, px_y = (float)h / 720.0f;
  if (video_shape == SHAPE_HD540)
    px_y = 0.75f; /* 1080p30: drawn as 540 lines, each shown twice */
  mxgl_window((int)scr_w, 720);
  frame_dt = 1.0f / (float)vmodes[video_mode].hz;
  /* 16-bit colour is dithered (draw.c, dither.c) */
  dither_bias = mxgl_mode_flags() & MXGL_RGB24 ? 0 : 4;
  dither_apply();
  text_init();
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
    d_rect(x0 - 18, top, x1 + 18, bottom, argb(C_WHITE, 0.07f), argb(C_WHITE, 0.03f));
    d_rect(x0 - 18, top, x1 + 18, top + 1, argb(C_LAMP, 0.5f), argb(C_LAMP, 0.5f));
  }
  float lw = d_text(FONT_BODY, x0, y, label, argb(on ? C_WHITE : C_FAINT, 1)) - x0;
  if (value)
    d_text_fit(FONT_BODY, FONT_SMALL, x1 - fminf(d_text_w(FONT_BODY, value), x1 - x0 - lw - 24), y,
               x1 - x0 - lw - 24, value, argb(chosen ? C_WHITE : C_DIM, 1));
  d_rect(x0, bottom, x1, bottom + 1, argb(C_WHITE, 0.08f), argb(C_WHITE, 0.08f));
}

static void note(float x, float y, const char *s) { d_text(FONT_SMALL, x, y, s, argb(C_FAINT, 1)); }

static void page_video(float x0, float x1, float y) {
  int c = video_choice();
  /* nine modes and five settings: closer rows */
  const float row = 30;
  for (int i = 0; i < VM_MODES; i++) {
    float yy = y + row * i;
    int chosen = in_page && item == i;
    if (chosen)
      d_rect(x0 - 18, yy - text_mid(FONT_BODY) - row * 0.5f, x1 + 18, yy - text_mid(FONT_BODY) + row * 0.5f,
             argb(C_WHITE, 0.07f), argb(C_WHITE, 0.03f));
    d_text(FONT_BODY, x0, yy, vmodes[i].name, argb(i == c ? C_WHITE : C_DIM, 1));
    if (i == c)
      d_text(FONT_BODY, x1 - d_text_w(FONT_BODY, "On"), yy, "On", argb(C_LAMP, 1));
  }
  int deep = cfg_get("video.depth", "32")[0] == '3';
  float yy = y + row * VM_MODES + 8;
  pitch = 40;
  line(x0, x1, yy, "Colour", deep ? "32-bit where it fits" : "16-bit", in_page && item == VM_MODES, 1);
  line(x0, x1, yy + 40, "Dithering (16-bit colour)", dither_name(dither_choice()), in_page && item == VM_MODES + 1, 1);
  line(x0, x1, yy + 80, "Dithering test picture", 0, in_page && item == VM_MODES + 2, 1);
  line(x0, x1, yy + 120, "Television (SD modes)", sd_wide() ? "16:9" : "4:3", in_page && item == VM_MODES + 3, 1);
  line(x0, x1, yy + 160, "Flicker filter (NTSC, PAL)", deflicker() ? "On" : "Off", in_page && item == VM_MODES + 4, 1);
  pitch = ROW;
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

static void page_storage(float x0, float x1, float y) {
  static const char *const keys[NDEVS] = {"dev.usb", "dev.hdd", "dev.host"};
  for (int d = 0; d < NDEVS; d++) {
    int on = cfg_get(keys[d], d == DEV_HOST ? "0" : "1")[0] == '1';
    char v[32];
    int n = 0;
    for (int i = 0; i < ngames; i++) {
      char p[16];
      game_path(p, sizeof p, i, "");
      n += !strncmp(p, dev_root[d], strlen(dev_root[d]));
    }
    mx_snprintf(v, sizeof v, !on ? "Off" : dev_found[d] ? "%d games" : "Not found", n);
    char l[32];
    mx_snprintf(l, sizeof l, "%s" DOT "%s", dev_name[d], dev_root[d]);
    line(x0, x1, y + ROW * d, l, v, in_page && item == d, on);
  }
  line(x0, x1, y + ROW * NDEVS, "Look again", 0, in_page && item == NDEVS, 1);
  note(x0, y + ROW * (NDEVS + 1), "Games are in <device>/PS2MAX/<game>/.");
}

static void page_pads(float x0, float x1, float y) {
  for (int p = 0; p < 2; p++) {
    char l[24];
    mx_snprintf(l, sizeof l, "Port %d", p + 1);
    int on = mx_pad_connected(p);
    const char *v = !on ? "No controller" : ticks < rumble_until && item == p ? "Rumbling" : SYM_CROSS "  Try the motors";
    line(x0, x1, y + ROW * p, l, v, in_page && item == p, on);
  }
  note(x0, y + ROW * 2, "USB and Bluetooth pads take the ports in turn.");
  note(x0, y + ROW * 2 + 26, "Battery, light and turning off come with the OS's pad calls.");
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
  d_rect(x, y, x + w, y + h, argb(0x17191E, 0.97f), argb(0x131519, 0.97f));
  d_rect(x, y, x + w, y + 1, argb(C_LAMP, 0.55f), argb(C_LAMP, 0.55f));
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

  /* the categories: the chosen one a lit pill */
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
    mx_pad_rumble(0, 0, 0), mx_pad_rumble(1, 0, 0);
    rumble_until = 0;
  }

  static const char *const h_list[] = {SYM_CROSS, "Open", SYM_CIRCLE, "Back", 0};
  static const char *const h_page[] = {SYM_CROSS, "Change", SYM_CIRCLE, "Back", 0};
  static const char *const h_edit[] = {"L R", "Number", "UP DOWN", "Change", SYM_CROSS, "Done", 0};
  if (confirm_from >= 0)
    confirm_draw();
  else
    hints_draw(edit ? h_edit : in_page ? h_page : h_list);
  status_draw();
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
      in_page = 1, item = 0;
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
  if (p & (MX_PAD_CIRCLE | MX_PAD_LEFT))
    in_page = 0;
  if (!(p & MX_PAD_CROSS))
    return;
  switch (page) {
  case P_VIDEO:
    if (item < VM_MODES)
      video_set(item);
    else if (item == VM_MODES + 1)
      dither_next(1);
    else if (item == VM_MODES + 2)
      screen = SCR_DITHER;
    else if (item == VM_MODES + 3) {
      cfg_set("video.aspect", sd_wide() ? "4:3" : "16:9");
      cfg_save();
      video_apply();
    } else if (item == VM_MODES + 4) {
      cfg_set("video.deflicker", deflicker() ? "off" : "on");
      cfg_save();
      video_apply();
    }
    else {
      cfg_set("video.depth", cfg_get("video.depth", "32")[0] == '3' ? "16" : "32");
      cfg_save();
      video_apply();
    }
    break;
  case P_NETWORK:
    if (item < 3)
      edit = 1;
    else if (ip_dirty)
      ip_save();
    break;
  case P_STORAGE:
    if (item < NDEVS) {
      static const char *const keys[NDEVS] = {"dev.usb", "dev.hdd", "dev.host"};
      int on = cfg_get(keys[item], item == DEV_HOST ? "0" : "1")[0] == '1';
      cfg_set(keys[item], on ? "0" : "1");
      cfg_save();
    }
    games_search();
    break;
  case P_PADS:
    if (mx_pad_connected(item) && mx_pad_rumble(item, 1, 160) >= 0)
      rumble_until = ticks + 30;
    break;
  }
}
