/* MXSHELL2: the shell of mx OS (PLAN.md), kernel version 26. The kernel
   starts it; it shows the games on the devices the kernel lists, starts one
   beside it (mx_proc_spawn) and is in front again when the game ends or the
   home button pauses it; the kernel's notifications as cards.

   Arguments, for pictures and tests from the host:
     dev=host        look for games on host: too (ps2client's directory)
     screen=hub|settings|boot   start on that screen
     right=<n>       the focus n tiles to the right (with pick: right n times on that row)
     sec=<n>         start in that section (0 Games .. 4 Settings)
     walk            (tests) the focus a tile on every 20 frames, there and back
     refl=a|b|c      (tests) the reflection's look
     mode=<video.mode>  (tests) video.mode for this run: 240p, 288p, 480i, 576i,
                        480p, 576p, 720p, 720p50, 1080i, 1080i50, 1080p24/25/30
     aspect=4:3|16:9 the television's shape in the SD modes
     deflicker=on|off the flicker filter in 480i and 576i
     hubk=<percent>  (tests) with screen=hub: the way in held there
     pick=<n>        (tests) with screen=settings: row n of the first page chosen
     page=<n>        (tests) Settings' page n open (2 Storage)
     vlist=<n>       (tests) with page=0: Video's list n open (1 the shell's
                     picture, 2 the modes this TV shows)
     overscan        (tests) the overscan test's picture to host:overscan.tga,
                     at once back (with page=0 pick=4 press=<frame>:x)
     room=vu         (tests) the room's lights on VU1 (room.vsm), not the EE
     stats           (tests) libgl's counters
     end=<frames>    (tests) the end marker after that many frames
     switch=<mode>,... every=<frames>   (tests) the modes one after the other
     note            a notification on the screen
     launch          start the game in focus at once (tests)
     shot=<frames>   the picture to host:mxshell.tga after that many frames,
                     then the end marker
     press=<frame>:<buttons>   (tests) buttons pressed at that frame (up to 8
                     of these): r l u d x o t s (right left up down cross
                     circle triangle square), 1 2 (L1 R1), h (home)
     noteat=<frame>  (tests) a notification at that frame */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/frame.h>
#include <mx/loader.h>
#include <mx/dev.h>
#include <mx/notify.h>
#include <mx/mx.h>
#include <mx/vm.h>
#include <mx/mem.h>
#include <mx/kernel.h>
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <stdlib.h>
#include <mx/setting.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* the shell waits in memory while a game runs: a core of its own */
MX_CORE_HEAP(512 << 10);

int screen = SCR_BOOT;
u32 pad_pressed, pad_held;
int pad_on;
static int input_wait;
static u32 screen_at, back_at;

void status_draw(void) {
  /* the sections, the active one with the lamp's dot under it */
  float x = MARGIN;
  int active = screen == SCR_SETTINGS ? SEC_SETTINGS : home_sec;
  for (int i = 0; i < NSECS; i++) {
    int on = i == active;
    float w = d_text_w(FONT_BODY, sec_name[i]);
    d_text(FONT_BODY, x, TOP, sec_name[i], argb(on ? C_WHITE : C_FAINT, 1));
    if (on) {
      d_glow(x + w / 2, TOP + 12, 8, 5, argb(C_LAMP, 0.6f));
      d_rect(x + w / 2 - 2, TOP + 10, x + w / 2 + 2, TOP + 14, argb(C_LAMP, 1), argb(C_LAMP, 1));
    }
    x += w + 30;
  }
  tray_draw();
}

/* ---- power-on: a point of light, a horizon, the mark, the shelf ----
   3.4 s: the point (0 - 0.8), the horizon drawn out (0.6 - 1.6), MXOS
   rising out of it (1.4 - 2.2), held, then all fading (2.8 - 3.4) */
#define BOOT_END 3.4f
void boot_draw(void) {
  float t = (float)(ticks - screen_at) * frame_dt;
  float cx = SCR_W / 2, cy = FLOOR_Y;
  float k = clampf((t - 0.6f) / 1.0f, 0, 1);
  k = 1 - (1 - k) * (1 - k) * (1 - k);
  float fade = t > 2.8f ? clampf(1 - (t - 2.8f) / 0.6f, 0, 1) : 1;
  float m = clampf((t - 1.4f) / 0.8f, 0, 1);
  m = m * m * (3 - 2 * m);
  /* the lights are the room's (opaque); the line and the mark over them */
  room_begin(-1);
  float p = clampf(t / 0.4f, 0, 1) * (1 - k);
  if (p > 0)
    room_light(cx, cy, 24, 24, 0.9f * p * fade);
  if (k > 0)
    room_light(cx, cy, 320 * k + 1, 44, 0.16f * fade);
  if (m > 0)
    room_light(cx, cy - 60, 220, 100, 0.10f * m * fade);
  d_begin_2d();
  room_draw();
  if (k > 0) {
    float half = SCR_W * 0.42f * k;
    d_rect_h(cx - half, cy, cx, cy + 1.5f, argb(C_LAMP, 0), argb(C_LAMP, 0.9f * fade));
    d_rect_h(cx, cy, cx + half, cy + 1.5f, argb(C_LAMP, 0.9f * fade), argb(C_LAMP, 0));
  }
  if (m > 0) {
    const char *mark = "MXOS";
    float w = d_text_w(FONT_DISP, mark);
    d_text(FONT_DISP, cx - w / 2, cy - 36 - 16 * (1 - m), mark, argb(C_WHITE, m * fade));
  }
  if (t >= BOOT_END) {
    screen = SCR_HOME, screen_at = ticks;
    home_enter();
  }
}

/* ---- starting a game ----
   The lit tile goes to the middle of the screen as a poster (the hub's
   slab, upright and larger) while the rest of the shelf goes dark; a line
   under it fills while the game is made ready; then black, and the game */
#define LAUNCH_T 1.05f
static void launch_draw(void) {
  float t = (float)(ticks - screen_at) * frame_dt;
  float k = clampf(t / 0.45f, 0, 1);
  k = k * k * (3 - 2 * k);
  struct tile_look poster;
  memset(&poster, 0, sizeof poster);
  poster.h = 470, poster.w = 470 / 1.41f;
  poster.x = (SCR_W - poster.w) / 2, poster.y = 84;
  poster.lit = 1, poster.fade = 1;
  home_scene(k, &poster);
  d_begin_2d();
  float p = clampf((t - 0.4f) / 0.5f, 0, 1), lw = 300, lx = (SCR_W - lw) / 2, ly = 596;
  if (k > 0.99f) {
    d_bar(lx, ly, lx + lw, ly + 3, p, C_LAMP, 1);
    d_glow(lx + lw * p, ly + 1.5f, 14, 10, argb(C_LAMP, 0.5f));
  }
  float b = clampf((t - 0.9f) / 0.15f, 0, 1);
  if (b > 0)
    d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, b), argb(C_INK, b));
}

/* The home button: the shell starts a game beside it (mx_proc_spawn) and
   waits for the kernel's event: its end, or the home button pressed in it,
   which pauses it and brings the shell in front. The shell then shows the
   game's hub: Resume (mx_proc_front) or Close (mx_proc_kill).
   The kernel's events come to a port that a thread of the shell's own
   always receives on, in front or not: the kernel's message slots are few
   and shared, and the hints (a device, a notification, a controller) must
   not keep them. The hints only set flags; the main loop reads the state
   (the device list, the notification ring) when they are set. */
static int ev_port, ev_sema;
/* a game's end or home press, kept until game_wait takes it: it may come
   before the shell waits (a game closed ends at once) */
#define EV_KEEP 8
static volatile struct {
  int proc, home, status;
} ev_kept[EV_KEEP];
static volatile int ev_devices, ev_notes, ev_settings; /* hints */
static int run_id;          /* the paused game's process */
int run_game = -1;
static char run_name[48], run_dir[32], run_root[12];
#define AT_HOME (-1000)     /* game_wait: the home button, not an end */
/* tests: hometest, the home button's way through (fakegame "0 home=<n>"
   presses it): the hub's picture, Resume, at the second press Close */
static int home_test, home_count;
static u32 home_at;

static u8 ev_stack[8192] MX_ALIGNED(16) MX_IRQ_BSS;
static void ev_thread(void *arg) {
  (void)arg;
  for (;;) {
    struct mx_event e;
    struct mx_msg m;
    memset(&e, 0, sizeof e);
    if (mx_msg_receive(ev_port, &e, sizeof e, &m) < 0)
      return;
    if (m.from != 0)
      continue;
    if (e.type == MX_EVENT_DEVICE)
      ev_devices = 1;
    else if (e.type == MX_EVENT_NOTIFY)
      ev_notes = 1;
    else if (e.type == MX_EVENT_SETTING)
      ev_settings = 1;
    else if (e.type == MX_EVENT_HOME || e.type == MX_EVENT_END) {
      int k = 0;
      while (k < EV_KEEP - 1 && ev_kept[k].proc)
        k++;
      ev_kept[k].home = e.type == MX_EVENT_HOME, ev_kept[k].status = e.status;
      ev_kept[k].proc = (int)e.proc;
      mx_sema_signal(ev_sema);
    }
  }
}

/* the game in front until it ends or the home button pauses it: its exit
   status, or AT_HOME */
static int game_wait(int id) {
  for (;;) {
    for (int k = 0; k < EV_KEEP; k++)
      if (ev_kept[k].proc == id) {
        int home = ev_kept[k].home, status = ev_kept[k].status;
        ev_kept[k].proc = 0;
        if (home)
          return AT_HOME;
        int st;
        mx_proc_wait(id, &st, 1);
        return status;
      }
    mx_sema_wait(ev_sema);
  }
}

/* the shell gives the game the GS, the pad's ports and as much memory as
   can be given: the covers go, and libgl gives back its arenas, beam
   racing's records and the rest it holds (mxgl_suspend stops the pipeline) */
void shell_away(void) {
  games_textures_free();
  mxgl_suspend();
}

/* and takes them back: libgl's memory (if the game's memory is not back
   yet, a while later), the mode as set, the pads, the covers */
void shell_back(void) {
  mxgl_resume();
  for (int i = 0; mxgl_suspended() && i < 50; i++)
    mx_sleep_us(100000), mxgl_resume();
  video_apply();
  pad_on = mx_pad_init() >= 0;
  games_textures();
  input_wait = 1;
  screen_at = ticks;
  back_at = ticks;
}

static void game_ended(const char *name, int st);

/* after game_wait: the hub of the paused game, or Home after its end */
static void game_away(int id, int g, int st) {
  shell_back();
  if (st == AT_HOME) {
    run_id = id, run_game = g;
    mx_printf("mxshell: home: %s paused\n", run_name);
    home_count++, home_at = ticks;
    screen = SCR_HUB;
    return;
  }
  /* an app (the store, Files) may have put programs on a device or taken
     them away: the shelf is read again */
  int app = g >= 0 && g < ngames && games[g].sec == SEC_APPS;
  run_id = 0, run_game = -1;
  screen = SCR_HOME;
  game_ended(run_name, st);
  if (app && !games_searching)
    games_search();
}

/* the paused game in front again */
static void game_resume(void) {
  int id = run_id, g = run_game;
  mx_printf("mxshell: resume %s\n", run_name);
  shell_away();
  if (mx_proc_front(id) < 0) {
    /* it is not there any more: its end is an event already, or lost */
    int st = 0;
    if (mx_proc_wait(id, &st, 1) != 1)
      st = -1;
    game_away(id, g, st);
    return;
  }
  /* back here: the next press of the home button, or its end */
  game_away(id, g, game_wait(id));
}

void game_close(void) {
  if (!run_id)
    return;
  mx_printf("mxshell: close %s\n", run_name);
  /* the shell paused while it ends in front, then here again: its mode */
  int id = run_id;
  run_id = 0, run_game = -1;
  if (mx_proc_kill(id, 0) == 0)
    game_wait(id);
  video_apply();
  input_wait = 1;
}

static void launch(void) {
  int g = focus_game();
  if (g < 0)
    return;
  if (run_id && g == run_game) {
    game_resume();
    return;
  }
  if (run_id) {
    char m[56];
    mx_snprintf(m, sizeof m, "%s", run_name);
    game_close();
    toast(N_INFO, "Closed", m, 0);
  }
  strncpy(run_name, games[g].title, sizeof run_name - 1), run_name[sizeof run_name - 1] = 0;
  strcpy(run_dir, games[g].dir), strcpy(run_root, game_root(g));
  shell_away();
  /* what stays of the shell beside the game: its pinned frames (the
     kernel's are counted too) */
  struct mx_mem_pressure mp;
  mx_mem_pressure(&mp);
  mx_printf("mxshell: launch %s: %u KB pinned, %u KB free\n", run_name, mp.pinned_frames * (MX_VM_PAGE / 1024),
            mp.free_frames * (MX_VM_PAGE / 1024));
  int id;
  if (games[g].setup[0]) {
    /* its setup first (Quake 3: its data made for the PS2, with its own
       screen): 0 the game, 2 stopped by the user, else what went wrong */
    id = game_setup(g);
    int st = id > 0 ? game_wait(id) : -1;
    mx_printf("mxshell: %s: setup %s: status %d\n", run_name, games[g].setup, st);
    if (st != 0) {
      game_away(id, g, st == 2 ? 0 : st);
      return;
    }
  }
  id = game_launch(g);
  game_away(id, g, id > 0 ? game_wait(id) : -1);
}

/* a crash is the kernel's notification */
static void game_ended(const char *name, int st) {
  struct mx_crash crash;
  if (st < 0)
    toast(N_WARN, "Could not start", name, 1);
  else if (!mx_crash_last(&crash) && st != 0) {
    char m[40];
    mx_snprintf(m, sizeof m, "Ended with status %d", st);
    toast(N_WARN, name, m, 1);
  }
}

/* the kernel's news while the shell is in front: the devices (the games
   on them again), the notifications and the settings; after a game too (the flags, or a
   counter that moved meanwhile) */
static u32 dev_seen;
static void news(void) {
  if (ev_devices || pdevs_gen() != dev_seen) {
    ev_devices = 0;
    dev_seen = pdevs_gen();
    if (!games_searching)
      games_search();
  }
  if (ev_notes || ticks % 30 == 0) {
    ev_notes = 0;
    notes_poll();
  }
  if (ev_settings) { /* the kernel's own changes (swap.path, ...) */
    ev_settings = 0;
    cfg_refresh();
    video_follow(); /* the kernel put a mode back (no confirm, L1 L2 R1 R2) */
    video_trial_check();
  }
}

/* triangle on a card with an action: its page */
static void note_action(void) {
  int a = toast_action();
  if (!a || screen == SCR_LAUNCH)
    return;
  toast_done();
  settings_goto(a == MX_NOTIFY_ACTION_PADS ? P_PADS : P_STORAGE, a == MX_NOTIFY_ACTION_SWAP);
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
  if (pad_pressed & MX_PAD_TRIANGLE && toast_action()) {
    note_action();
    pad_pressed = 0;
  }
  if (pad_pressed && toast_dismiss())
    pad_pressed = 0;
}

#ifdef MX_BIOS_CALLS
/* the standalone build on PCSX2 (-elf: no arguments): host:mxshell.txt */
static char *file_args[8], file_buf[128];
static int args_from_host(char ***argv) {
  int fd = mx_fio_open("host:mxshell.txt", MX_FIO_RDONLY), n = 0, argc = 1;
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

/* tests: switch=<mode>[,<mode>...]: one after the other, every= frames
   apart (90) */
static const char *switch_to;
static u32 switch_every = 90;

/* tests: buttons pressed at a frame (with shot=, an animation frame by
   frame: each frame is the same in every run, frame_dt being fixed) */
static struct {
  u32 at, buttons;
} presses[8];
static int npresses;
static u32 note_at;

static u32 press_mask(const char *s) {
  u32 m = 0;
  for (; *s; s++)
    switch (*s) {
    case 'r': m |= MX_PAD_RIGHT; break;
    case 'l': m |= MX_PAD_LEFT; break;
    case 'u': m |= MX_PAD_UP; break;
    case 'd': m |= MX_PAD_DOWN; break;
    case 'x': m |= MX_PAD_CROSS; break;
    case 'o': m |= MX_PAD_CIRCLE; break;
    case 't': m |= MX_PAD_TRIANGLE; break;
    case 's': m |= MX_PAD_SQUARE; break;
    case '1': m |= MX_PAD_L1; break;
    case '2': m |= MX_PAD_R1; break;
    case 'h': m |= MX_PAD_HOME; break;
    }
  return m;
}

int main(int argc, char **argv) {
  int page_arg = -1, vlist_arg = 0;
  int shot = 0, right = 0, note = 0, start = SCR_BOOT, go = 0, stats = 0, end = 0, walk = 0, pick = -1;
  cfg_load();
  /* the kernel's events: a game's end, the home button, the hints (none
     in the standalone build) */
  ev_port = mx_port_create("mxshell");
  if (ev_port > 0 && mx_proc_events(ev_port) == 0) {
    ev_sema = mx_sema_create(0, 64);
    mx_thread_start(ev_thread, 0, ev_stack, sizeof ev_stack, mx_thread_prio(mx_thread_self()) - 1);
  }
#ifdef MX_BIOS_CALLS
  if (argc <= 1)
    argc = args_from_host(&argv);
#endif
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "dev=host"))
      cfg_arg("dev.host", "1");
    else if (!strncmp(a, "shot=", 5))
      for (const char *s = a + 5; *s >= '0' && *s <= '9'; s++)
        shot = shot * 10 + *s - '0';
    else if (!strncmp(a, "right=", 6))
      right = a[6] - '0';
    else if (!strncmp(a, "sec=", 4)) {
      home_sec = a[4] - '0';
      if (home_sec >= SEC_SETTINGS) /* Settings is its own screen */
        home_sec = SEC_APPS, start = SCR_SETTINGS;
    }
    else if (!strncmp(a, "mode=", 5))
      cfg_arg("video.mode", a + 5);
    else if (!strncmp(a, "aspect=", 7))
      cfg_arg("video.aspect", a + 7);
    else if (!strncmp(a, "deflicker=", 10))
      cfg_arg("video.deflicker", a + 10);
    else if (!strncmp(a, "switch=", 7))
      switch_to = a + 7;
    else if (!strncmp(a, "every=", 6))
      switch_every = (u32)atoi(a + 6);
    else if (!strncmp(a, "refl=", 5)) {
      extern int refl_style;
      refl_style = a[5] - 'a';
    } else if (!strcmp(a, "room=vu")) {
      extern int room_on_vu;
      room_on_vu = 1;
    } else if (!strncmp(a, "page=", 5))
      page_arg = atoi(a + 5), start = SCR_SETTINGS;
    else if (!strncmp(a, "pick=", 5))
      pick = atoi(a + 5);
    else if (!strncmp(a, "vlist=", 6))
      vlist_arg = atoi(a + 6);
    else if (!strcmp(a, "overscan"))
      overscan_shot = 1;
    else if (!strcmp(a, "walk"))
      walk = 1;
    else if (!strcmp(a, "stats"))
      stats = 1;
    else if (!strncmp(a, "end=", 4))
      end = atoi(a + 4);
    else if (!strcmp(a, "nobeam"))
      cfg_arg("video.beam", "0");
    else if (!strcmp(a, "rgb16"))
      cfg_arg("video.depth", "16");
    else if (!strcmp(a, "hometest"))
      home_test = 1;
    else if (!strcmp(a, "launch"))
      go = 1, start = SCR_HOME;
    else if (!strcmp(a, "note"))
      note = 1;
    else if (!strncmp(a, "press=", 6) && npresses < 8) {
      const char *c = strchr(a, ':');
      presses[npresses].at = (u32)atoi(a + 6);
      presses[npresses++].buttons = c ? press_mask(c + 1) : 0;
    } else if (!strncmp(a, "noteat=", 7))
      note_at = (u32)atoi(a + 7);
    else if (!strcmp(a, "dev=hostonly"))
      cfg_arg("dev.host", "1"), host_only = 1;
    else if (!strncmp(a, "hubk=", 5)) {
      extern float hub_pin;
      hub_pin = (float)atoi(a + 5) / 100;
    } else if (!strcmp(a, "screen=hub"))
      start = SCR_HUB;
    else if (!strcmp(a, "screen=settings"))
      start = SCR_SETTINGS;
    else if (!strcmp(a, "screen=gamepads")) /* tests: the game menu's Controllers, the first game as if paused */
      run_game = 0, start = SCR_GAMEPADS;
    else if (!strcmp(a, "screen=dither"))
      start = SCR_DITHER;
    else if (!strncmp(a, "dither=", 7))
      cfg_arg("video.dither", a + 7);
    else if (!strcmp(a, "screen=home"))
      start = SCR_HOME;
    else if (!strncmp(a, "set=", 4)) { /* set=<key>=<value>: saved, as Settings does */
      char k[MX_SETTING_KEY];
      const char *eq = strchr(a + 4, '=');
      u32 n = eq ? (u32)(eq - a - 4) : 0;
      if (eq && n < sizeof k) {
        memcpy(k, a + 4, n), k[n] = 0;
        cfg_set(k, eq + 1);
        cfg_save();
      }
    } else if (!strncmp(a, "show=", 5)) { /* the store's value, read again */
      cfg_load();
      mx_printf("mxshell: setting %s = '%s'\n", a + 5, cfg_get(a + 5, "(not set)"));
    }
  }

  mxgl_init();
  mxgl_show_stats = stats;
  pad_on = mx_pad_init() >= 0;
  video_apply();
  dev_seen = pdevs_gen();
  games_search();
  if (note) {
    toast(N_DEVICE, "USB drive ready", "usb0" DOT "8 games", 0);
    toast(N_PAD, "Controller connected", "Port 2", 0);
  }

  for (;;) {
    ticks++;
    read_pad();
    for (int i = 0; i < npresses; i++)
      if (presses[i].at == ticks)
        pad_pressed |= presses[i].buttons, pad_held |= presses[i].buttons;
    if (note_at && ticks == note_at)
      toast(N_PAD, "Controller connected", "DualShock 4" DOT "Bluetooth" DOT "player 2", 0);
    /* the covers of a device as soon as its games are found, not after the
       devices after it (an HDD that is not there takes 10 s to give up on) */
    static int covered;
    if (ngames != covered) {
      games_textures();
      covered = ngames;
    }
    if (games_searching && ticks % 15 == 0 && games_step()) {
      games_searching = 0;
      games_textures();
      /* the paused game's tile again (or none: its device went) */
      if (run_id)
        run_game = games_find(run_root, run_dir);
      if (start != SCR_BOOT) {
        /* a picture of a screen: once the games are there */
        for (int i = 0; start != SCR_SETTINGS && i < right; i++) {
          pad_pressed = MX_PAD_RIGHT;
          home_input();
        }
        home_enter();
        if (start == SCR_SETTINGS) {
          settings_enter();
          if (page_arg >= 0)
            settings_goto(page_arg, 0);
          if (vlist_arg)
            settings_vlist(vlist_arg);
          if (pick >= 0) {
            void settings_pick(int n);
            settings_pick(pick);
            for (int i = 0; i < right; i++) {
              pad_pressed = MX_PAD_RIGHT;
              settings_input();
            }
          }
        }
        else if (start == SCR_DITHER)
          screen = SCR_DITHER;
        else
          screen = start;
        start = SCR_BOOT;
        if (go)
          screen = SCR_LAUNCH;
      }
    }
    news();
    toast_step();
    ram_step();
    if (screen != SCR_BOOT)
      notes_boot_done();

    if (walk && screen == SCR_HOME && ticks % 20 == 0) {
      /* there and back along the row: 8 steps one way, 8 the other */
      pad_pressed = (ticks / 20) % 16 < 8 ? MX_PAD_RIGHT : MX_PAD_LEFT;
    }
    /* the home button with a game paused: back to it */
    if (run_id && pad_pressed & MX_PAD_HOME && screen != SCR_LAUNCH) {
      pad_pressed = 0;
      game_resume();
    }
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
    case SCR_GAMEPADS:
      gamepads_input();
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
    case SCR_GAMEPADS:
      gamepads_draw();
      break;
    }
    d_begin_2d();
    if (back_at && ticks - back_at < 24) {
      /* back from a game: the shelf comes out of the dark */
      float k = 1 - (float)(ticks - back_at) / 24;
      d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, k * k), argb(C_INK, k * k));
    }
    if (screen != SCR_BOOT)
      toast_draw();
    mxgl_end_frame();
    mxgl_frame_done();

    if (switch_to && *switch_to && switch_every && ticks % switch_every == 0) {
      char m[16];
      int n = 0;
      while (*switch_to && *switch_to != ',' && n < 15)
        m[n++] = *switch_to++;
      m[n] = 0;
      if (*switch_to == ',')
        switch_to++;
      mx_printf("mxshell: switch to %s\n", m);
      cfg_arg("video.mode", m);
      video_apply();
    }
    if (screen == SCR_LAUNCH && (float)(ticks - screen_at) * frame_dt > LAUNCH_T)
      launch();
    if (home_test && run_id && ticks - home_at == 90) {
      int r = mxgl_screenshot(home_count == 1 ? "host:mxshell-home1.tga" : "host:mxshell-home2.tga");
      mx_printf("mxshell: hometest: press %d, the hub's picture %d\n", home_count, r);
      if (home_count == 1)
        game_resume();
      else {
        game_close();
        mx_printf("mxshell: hometest: closed, %s\n", run_id ? "still there" : "gone");
        MX_TEST_END(run_id ? 1 : 0);
        return 0;
      }
    }
    if (end && ticks == (u32)end) {
      MX_TEST_END(0);
      return 0;
    }
    if (shot && ticks == (u32)shot) {
      int r = mxgl_screenshot("host:mxshell.tga");
      mx_printf("mxshell: shot %d\n", r);
      MX_TEST_END(r ? 1 : 0);
      return 0;
    }
  }
}
