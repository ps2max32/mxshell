/* MXSHELL2: the shell of mx OS (PLAN.md). The kernel starts it; it shows the
   games on the devices, starts one with mx_loader_launch and waits for it,
   and comes back when it ends.

   Arguments, for pictures and tests from the host:
     dev=host        look for games on host: too (ps2client's directory)
     screen=hub|settings|boot   start on that screen
     right=<n>       the focus n tiles to the right
     note            a notification on the screen
     launch          start the game in focus at once (tests)
     shot=<frames>   the picture to host:mxshell2.tga after that many frames,
                     then the end marker */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/frame.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* the shell waits in memory while a game runs: a core of its own */
MX_CORE_HEAP(512 << 10);

int screen = SCR_BOOT;
u32 ticks;
u32 pad_pressed, pad_held;
static int pad_on, pads_seen[2], input_wait;
static u32 screen_at, back_at;

void status_draw(void) {
  /* the sections, the active one with the lamp's dot under it */
  float x = MARGIN;
  static const char *const tabs[2] = {"Play", "Settings"};
  for (int i = 0; i < 2; i++) {
    int on = (i == 0) == (screen != SCR_SETTINGS);
    float w = d_text_w(FONT_BODY, tabs[i]);
    d_text(FONT_BODY, x, TOP, tabs[i], argb(on ? C_WHITE : C_FAINT, 1));
    if (on) {
      d_glow(x + w / 2, TOP + 12, 8, 5, argb(C_LAMP, 0.6f));
      d_rect(x + w / 2 - 2, TOP + 10, x + w / 2 + 2, TOP + 14, argb(C_LAMP, 1), argb(C_LAMP, 1));
    }
    x += w + 34;
  }
  /* the pads: a ring per port, lit when a pad is there */
  float rx = SCR_W - MARGIN - 15;
  for (int p = 1; p >= 0; p--) {
    int on = pad_on && mx_pad_connected(p);
    float cy = TOP - 6;
    d_ring(rx, cy, 15, argb(C_WHITE, on ? 0.30f : 0.12f));
    char s[4] = {'P', (char)('1' + p), 0};
    float w = d_label_w(s) - 2;
    d_label(rx - w / 2, cy + text_mid(FONT_LABEL) * 1.15f, s, argb(on ? C_WHITE : C_FAINT, 1));
    rx -= 42;
  }
}

void hints_draw(const char *const *p) {
  float w = 0;
  for (int i = 0; p[i]; i += 2) {
    int sym = (unsigned char)p[i][0] < 8;
    w += (sym ? d_text_w(FONT_BODY, p[i]) : d_label_w(p[i])) + 8 + d_text_w(FONT_SMALL, p[i + 1]) + 26;
  }
  float x = SCR_W - MARGIN - w + 26, y = SCR_H - 36;
  for (int i = 0; p[i]; i += 2) {
    int sym = (unsigned char)p[i][0] < 8;
    x = sym ? d_text(FONT_BODY, x, y, p[i], argb(C_DIM, 1)) : d_label(x, y - 1, p[i], argb(C_DIM, 1));
    x = d_text(FONT_SMALL, x + 8, y, p[i + 1], argb(C_DIM, 1)) + 26;
  }
}

/* ---- power-on: a point of light, a horizon, the mark, the shelf ---- */
void boot_draw(void) {
  float t = (float)(ticks - screen_at) * frame_dt;
  float cx = SCR_W / 2, cy = FLOOR_Y;
  float k = clampf((t - 0.4f) / 0.5f, 0, 1);
  k = 1 - (1 - k) * (1 - k);
  float fade = t > 1.4f ? clampf(1 - (t - 1.4f) / 0.4f, 0, 1) : 1;
  float m = clampf((t - 0.8f) / 0.3f, 0, 1) * fade;
  /* the lights are the room's (opaque); the line and the mark over them */
  room_begin(-1);
  if (t < 0.4f)
    room_light(cx, cy, 24, 24, 0.9f * clampf(t / 0.2f, 0, 1));
  else if (t < 1.8f) {
    room_light(cx, cy, 320 * k + 1, 44, 0.16f * fade);
    if (m > 0)
      room_light(cx, cy - 60, 150, 90, 0.10f * m);
  }
  d_begin_2d();
  room_draw();
  if (t >= 0.4f && t < 1.8f) {
    float half = SCR_W * 0.42f * k;
    d_rect_h(cx - half, cy, cx, cy + 1.5f, argb(C_LAMP, 0), argb(C_LAMP, 0.9f * fade));
    d_rect_h(cx, cy, cx + half, cy + 1.5f, argb(C_LAMP, 0.9f * fade), argb(C_LAMP, 0));
    if (m > 0) {
      float w = d_text_w(FONT_DISP, "mx");
      d_text(FONT_DISP, cx - w / 2, cy - 36 - 12 * (1 - m), "mx", argb(C_WHITE, m));
    }
  }
  if (t >= 1.8f) {
    screen = SCR_HOME, screen_at = ticks;
    home_enter();
  }
}

/* ---- starting a game ---- */
/* the lit tile grows to the whole screen and goes dark; then the game */
static void launch_draw(void) {
  float t = (float)(ticks - screen_at) * frame_dt;
  float k = clampf(t / 0.45f, 0, 1);
  float e = k * k * (3 - 2 * k);
  home_draw();
  d_begin_2d();
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.85f * e), argb(C_INK, 0.85f * e));
  float r[4];
  home_focus_rect(r);
  float x0 = r[0] * (1 - e), y0 = r[1] * (1 - e), x1 = (r[0] + r[2]) * (1 - e) + SCR_W * e,
        y1 = (r[1] + r[3]) * (1 - e) + SCR_H * e;
  u32 c = home_focus_colour();
  float dark = 1 - e;
  d_rect(x0, y0, x1, y1, argb(c, dark), argb(C_INK, 1));
  d_glow((x0 + x1) * 0.5f, y0, (x1 - x0) * 0.6f, 60 + 200 * e, argb(C_LAMP, 0.18f * dark));
}

static void launch(void) {
  int g = focus_game();
  if (g < 0)
    return;
  char name[48];
  strncpy(name, games[g].title, sizeof name - 1), name[sizeof name - 1] = 0;
  /* the game gets the GS, the pad's ports and as much memory as can be
     given: the covers go, the pipeline stops */
  games_textures_free();
  /* without beam racing: its records and arenas (2 MiB) given back */
  mxgl_set_mode(MXGL_SD);
  mx_frame_stop();
  int st = game_launch(g);
  video_apply();
  pad_on = mx_pad_init() >= 0;
  games_textures();
  input_wait = 1;
  screen = SCR_HOME, screen_at = ticks;
  back_at = ticks;
  struct mx_crash crash;
  if (st < 0)
    toast(N_WARN, "Could not start", name, 1);
  else if (mx_crash_last(&crash)) {
    char m[48];
    mx_snprintf(m, sizeof m, "%s at %08x", crash.what ? crash.what : "Crashed", crash.epc);
    toast(N_WARN, name, m, 1);
  } else if (st != 0) {
    char m[40];
    mx_snprintf(m, sizeof m, "Ended with status %d", st);
    toast(N_WARN, name, m, 1);
  }
}

static void pads_watch(void) {
  for (int p = 0; p < 2; p++) {
    int on = pad_on && mx_pad_connected(p);
    if (on != pads_seen[p] && ticks > 90) {
      char m[24];
      mx_snprintf(m, sizeof m, "Port %d", p + 1);
      toast(N_PAD, on ? "Controller connected" : "Controller removed", m, 0);
    }
    pads_seen[p] = on;
  }
}

static void read_pad(void) {
  pad_pressed = pad_held = 0;
  if (!pad_on)
    return;
  pad_pressed = mx_pad_pressed(0) | mx_pad_pressed(1);
  pad_held = mx_pad_buttons(0) | mx_pad_buttons(1);
  if (input_wait) {
    /* after a game: nothing counts until every button is let go */
    if (pad_held)
      pad_pressed = 0;
    else
      input_wait = 0;
  }
  if (pad_pressed && toast_dismiss())
    pad_pressed = 0;
}

#ifdef MX_BIOS_CALLS
/* the standalone build on PCSX2 (-elf: no arguments): host:mxshell2.txt */
static char *file_args[8], file_buf[128];
static int args_from_host(char ***argv) {
  int fd = mx_fio_open("host:mxshell2.txt", MX_FIO_RDONLY), n = 0, argc = 1;
  if (fd < 0)
    return 1;
  n = mx_fio_read(fd, file_buf, sizeof file_buf - 1);
  mx_fio_close(fd);
  file_buf[n > 0 ? n : 0] = 0;
  for (char *s = file_buf; *s && argc < 8;) {
    while (*s == ' ' || *s == '\n')
      *s++ = 0;
    if (!*s)
      break;
    file_args[argc++] = s;
    while (*s && *s != ' ' && *s != '\n')
      s++;
  }
  *argv = file_args;
  return argc;
}
#endif

/* tests: switch=<mode>[,<mode>...]: one after the other, 90 frames apart */
static const char *switch_to;

int main(int argc, char **argv) {
  int shot = 0, right = 0, note = 0, start = SCR_BOOT, go = 0;
  cfg_load();
#ifdef MX_BIOS_CALLS
  if (argc <= 1)
    argc = args_from_host(&argv);
#endif
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "dev=host"))
      cfg_set("dev.host", "1");
    else if (!strncmp(a, "shot=", 5))
      for (const char *s = a + 5; *s >= '0' && *s <= '9'; s++)
        shot = shot * 10 + *s - '0';
    else if (!strncmp(a, "right=", 6))
      right = a[6] - '0';
    else if (!strncmp(a, "mode=", 5))
      cfg_set("video.mode", a + 5);
    else if (!strncmp(a, "switch=", 7))
      switch_to = a + 7;
    else if (!strcmp(a, "nobeam"))
      cfg_set("video.beam", "0");
    else if (!strcmp(a, "rgb16"))
      cfg_set("video.depth", "16");
    else if (!strcmp(a, "launch"))
      go = 1, start = SCR_HOME;
    else if (!strcmp(a, "note"))
      note = 1;
    else if (!strcmp(a, "dev=hostonly"))
      cfg_set("dev.host", "1"), cfg_set("dev.usb", "0"), cfg_set("dev.hdd", "0");
    else if (!strcmp(a, "screen=hub"))
      start = SCR_HUB;
    else if (!strcmp(a, "screen=settings"))
      start = SCR_SETTINGS;
    else if (!strcmp(a, "screen=dither"))
      start = SCR_DITHER;
    else if (!strncmp(a, "dither=", 7))
      cfg_set("video.dither", a + 7);
    else if (!strcmp(a, "screen=home"))
      start = SCR_HOME;
  }

  mxgl_init();
  mxgl_show_stats = 0;
  pad_on = mx_pad_init() >= 0;
  /* Start + Select held at power-on: the console's own SD mode, for a
     television that cannot show the one set */
  if (pad_on) {
    u32 held = 0;
    for (int i = 0; i < 20 && !held; i++) {
      mx_sleep_us(25000);
      held = (mx_pad_buttons(0) | mx_pad_buttons(1)) & (MX_PAD_START | MX_PAD_SELECT);
    }
    if (held == (MX_PAD_START | MX_PAD_SELECT)) {
      cfg_set("video.mode", "sd");
      cfg_save();
    }
  }
  video_apply();
  frame_dt = mxgl_mode() == MXGL_PAL ? 1.0f / 50 : 1.0f / 60;
  games_search();
  if (note) {
    toast(N_DEVICE, "USB drive ready", "usb0" DOT "8 games", 0);
    toast(N_PAD, "Controller connected", "Port 2", 0);
  }

  for (;;) {
    ticks++;
    read_pad();
    if (games_searching && ticks % 15 == 0 && games_step()) {
      games_searching = 0;
      games_textures();
      for (int d = 0; d < NDEVS; d++)
        if (dev_found[d] && d != DEV_HOST && ticks > 150)
          toast(N_DEVICE, d == DEV_USB ? "USB drive ready" : "Hard disk ready", dev_root[d], 0);
      if (start != SCR_BOOT) {
        /* a picture of a screen: once the games are there */
        for (int i = 0; i < right; i++) {
          pad_pressed = MX_PAD_RIGHT;
          home_input();
        }
        home_enter();
        if (start == SCR_SETTINGS)
          settings_enter();
        else if (start == SCR_DITHER)
          screen = SCR_DITHER;
        else
          screen = start;
        start = SCR_BOOT;
        if (go)
          screen = SCR_LAUNCH;
      }
    }
    pads_watch();
    toast_step();

    switch (screen) {
    case SCR_HOME:
      home_input();
      break;
    case SCR_HUB:
      hub_input();
      break;
    case SCR_SETTINGS:
      settings_input();
      break;
    case SCR_DITHER:
      dither_input();
      break;
    }
    if (screen == SCR_LAUNCH && !screen_at)
      screen_at = ticks;

    glClearColor((C_INK >> 16 & 255) / 255.0f, (C_INK >> 8 & 255) / 255.0f, (C_INK & 255) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    static int last = -1;
    if (screen != last)
      screen_at = ticks, last = screen;
    switch (screen) {
    case SCR_BOOT:
      boot_draw();
      break;
    case SCR_HOME:
      home_draw();
      break;
    case SCR_HUB:
      hub_draw();
      break;
    case SCR_SETTINGS:
      settings_draw();
      break;
    case SCR_LAUNCH:
      launch_draw();
      break;
    case SCR_DITHER:
      dither_draw();
      break;
    }
    d_begin_2d();
    if (back_at && ticks - back_at < 24) {
      /* back from a game: the shelf comes out of the dark */
      float k = 1 - (float)(ticks - back_at) / 24;
      d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, k * k), argb(C_INK, k * k));
    }
    toast_draw();
    mxgl_end_frame();
    mxgl_frame_done();

    if (switch_to && *switch_to && ticks % 90 == 0) {
      char m[16];
      int n = 0;
      while (*switch_to && *switch_to != ',' && n < 15)
        m[n++] = *switch_to++;
      m[n] = 0;
      if (*switch_to == ',')
        switch_to++;
      mx_printf("mxshell2: switch to %s\n", m);
      cfg_set("video.mode", m);
      video_apply();
    }
    if (screen == SCR_LAUNCH && ticks - screen_at > 30)
      launch();
    if (shot && ticks == (u32)shot) {
      int r = mxgl_screenshot("host:mxshell2.tga");
      mx_printf("mxshell2: shot %d\n", r);
      MX_TEST_END(r ? 1 : 0);
      return 0;
    }
  }
}
