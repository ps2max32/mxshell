/* MXSHELL2: Penumbra, the shell's design language, and what the parts share.
   Focus is light: one warm light follows the selection, the rest stays in
   half-shadow. Ink, graphite and warm white; the lamp colour is only ever
   the light.

   Layout units are the mockups' and square on the screen in every mode:
   720 of them high, 1280 wide at 16:9 (1080i/p, 720p) and 960 wide at 4:3
   (480p, 576p, NTSC 640 x 448, PAL 640 x 512; the last three with pixels
   that are not square): each axis its own scale. The fonts are made for
   each shape of pixel and drawn 1:1. */
#ifndef PN_H
#define PN_H

#include <mx/types.h>

/* ---- the screen ---- */
#define SCR_W scr_w
#define SCR_H 720.0f
extern float scr_w; /* 1280 or 960 */
/* the video modes (settings.c has their table) and the shapes of their
   pixels (font.c: an atlas each) */
enum { VM_240P, VM_288P, VM_480I, VM_576I, VM_480P, VM_576P, VM_720P, VM_720P50, VM_1080I, VM_1080I50,
       VM_1080P24, VM_1080P25, VM_1080P30, VM_MODES };
#define VM_HD(m) ((m) >= VM_720P)                     /* 16:9 always, no overscan */
#define VM_LOW(m) ((m) == VM_240P || (m) == VM_288P)  /* half the lines of 480i, 576i */
#define VM_RGB(m) ((m) <= VM_576I)                    /* the modes RGB carries */
enum { SHAPE_HD, SHAPE_HD540, SHAPE_P720, SHAPE_P576, SHAPE_P480, SHAPE_NTSC, SHAPE_PAL,
       /* the SD modes at 16:9 (video.aspect): anamorphic, 1280 units across 640 pixels */
       SHAPE_P576W, SHAPE_P480W, SHAPE_NTSCW, SHAPE_PALW,
       /* 240p, 288p: half the lines of NTSC and PAL, 4:3 and 16:9 */
       SHAPE_P240, SHAPE_P288, SHAPE_P240W, SHAPE_P288W };
extern int video_mode, video_shape;
/* the modes (video.c): the setting's key, its name, libgl's mode, the
   shape of its pixels (the font atlas), 16:9, frames a second */
struct vmode {
  const char *key, *name;
  int gl, shape, wide, hz;
};
extern const struct vmode vmodes[VM_MODES];
struct mx_screen_conf;
void video_conf(struct mx_screen_conf *c); /* <mx/screen.h>'s, over the shell's copy of the store */
int video_of_screen(u32 mode);             /* VM_* of an MX_SCREEN_* mode */
int video_choice(void);    /* video.mode as the kernel uses it; without it the region's 480i or 576i */
void video_follow(void);   /* the mode again if the video settings changed (MX_EVENT_SETTING) */
void video_trial_check(void); /* settings.c: the trial ends when the kernel put the mode back */
int overscan_test(void);   /* overscan.c: the test frame until a button is pressed */
extern int overscan_shot; /* tests: its picture saved, no wait */
void settings_vlist(int l); /* tests: a list of the Video page open */
void shell_away(void);     /* main.c: libgl and the covers put aside (a game, the overscan test) */
void shell_back(void);     /* and taken back */
int video_sd_wide(void);   /* video.aspect 16:9 (the SD modes) */
int video_deflicker(void); /* video.deflicker (NTSC, PAL) */
extern float px_x, px_y; /* pixels a unit, across and down */
#define MARGIN 96        /* left and right */
#define TOP 58           /* the status line's baseline */
#define FLOOR_Y 430.0f   /* tiles stand on this line */

/* ---- colours: 0xRRGGBB ---- */
#define C_INK 0x080808u /* steps of 8: 16-bit colour (1080i) keeps it neutral */
#define C_GRAPHITE 0x111317u
#define C_SLAB 0x1C1F24u
#define C_FLOOR 0x101010u
#define C_DIM 0xA3A6ABu   /* dim text */
#define C_FAINT 0x7D8187u /* labels, hints */
#define C_WHITE 0xECEAE6u /* warm white */
#define C_LAMP 0xE9C98Fu  /* the light */
#define C_WARN 0xE8A07Au  /* battery low, errors */
#define C_DEEP 0x101010u  /* the dark end of a tile's gradient */

/* ---- motion ---- */
/* a critically damped spring: x goes to its target without overshoot;
   omega sets how fast (about 4 / omega seconds to settle) */
struct spring {
  float x, v;
};
#define W_FOCUS 24.0f /* the focus moving: ~170 ms */
#define W_LAYER 13.0f /* a layer coming in or going: ~300 ms */
#define W_MOVE 8.0f   /* launch, the camera: ~500 ms */
void spring_step(struct spring *s, float target, float omega, float dt);
extern float frame_dt; /* 1/60 or 1/50 s */

/* ---- drawing (draw.c) ---- */
/* the 2D layer: pixel coordinates, (0, 0) top left */
void d_begin_2d(void);
/* a rectangle, colours top and bottom (0xAARRGGBB, alpha 0..255) */
void d_rect(float x0, float y0, float x1, float y1, u32 top, u32 bottom);
/* the same, colours left and right */
void d_rect_h(float x0, float y0, float x1, float y1, u32 left, u32 right);
/* the room: the background with its lights and shadows, worked out per
   vertex and drawn opaque (translucent layers darken 16-bit colour where
   they fade out). room_begin with the floor's line (< 0: none), the
   lights and shadows, then room_draw first in the frame. room_col gives
   its colour at a point (the reflections mix with it). */
void room_begin(float floor_y);
/* how much of the floor shows, 1 .. 0 (room_begin: 1): a screen without one coming in */
void room_floor(float k);
void room_light(float cx, float cy, float rx, float ry, float a);
void room_shadow(float cx, float cy, float rx, float ry, float a);
void room_draw(void);
void room_col(float x, float y, float rgb[3]);
void room_vertex(float x, float y, const float rgb[3]);
extern int dither_bias;
/* a soft light: an ellipse, bright in the middle, added to what is there */
void d_glow(float cx, float cy, float rx, float ry, u32 argb);
/* a cone of light from (cx, y0) widening to rx at y1, added */
void d_cone(float cx, float y0, float y1, float rx, u32 argb);
/* surfaces (surface.c): every rounded shape, with the same smooth edge
   (a rim that blends with what is under it) and the same light (the
   lamp's colour along the top edge, round the corners) */
struct surface {
  float x0, y0, x1, y1, r;
  u32 fill;    /* rgb */
  float glass; /* >= 0: in place of fill, what is under it lighter by glass */
  u32 line;    /* an outline of a pixel, argb; 0: none */
  float lamp;  /* 0..1: the lamp along the top edge */
  float a;     /* the whole surface's opacity (1: opaque inside) */
  int hollow;  /* only the line, no inside (a ring) */
  int grad;    /* the fill from fill at the top (opacity fa) to fill_b at the bottom (fa_b) */
  u32 fill_b;
  float fa, fa_b;
};
void surface_draw(const struct surface *s);
/* the lamp's part of an edge whose outward normal has ny (-1: straight up) */
float lamp_edge(float ny, float lamp);
void d_vertex(float x, float y, const float rgb[3], float a);
/* glass takes the colour under it: the room's, or the one set by aa_under */
void aa_under(u32 rgb);
void aa_under_room(void);
void d_pill(float x0, float y0, float x1, float y1, u32 rgb);
/* glass: what is under it, lighter by lighter (0..255), with an outline */
void d_pill_glass(float x0, float y0, float x1, float y1, float lighter, u32 line_argb);
void d_card(float x0, float y0, float x1, float y1, float radius, u32 rgb);
/* a card with the lamp along its top (0..1) and an opacity a */
void d_card_lit(float x0, float y0, float x1, float y1, float radius, u32 rgb, float a, float lamp);
/* a translucent white highlight (a chosen row), with the lamp along its top */
void d_highlight(float x0, float y0, float x1, float y1, float r, float a_top, float a_bottom, float lamp);
/* a bar: a faint track, and the part k (0..1) in c */
void d_bar(float x0, float y0, float x1, float y1, float k, u32 c, float a);
/* a ring of a pixel */
void d_ring(float cx, float cy, float r, u32 line_argb);
/* a soft shadow: a dark ellipse, blended */
void d_shadow(float cx, float cy, float rx, float ry, float alpha);
/* a button as in the mockups: a pill with a pad symbol and its label;
   primary: warm white with a glow, else glass with an outline. Returns
   its width */
float d_button(float x, float y, const char *sym, const char *label, int primary);

/* text (font.c): sizes from font/font.h (FONT_LABEL ... FONT_DISP);
   x, y the baseline's start in units; returns the x after it */
#include "../font/font.h"
void text_init(void); /* for the mode set: again after a change */
float d_text(int size, float x, float y, const char *s, u32 argb);
float d_text_w(int size, const char *s);
/* LABEL style: capitals with tracking */
float d_label(float x, float y, const char *s, u32 argb);
float d_label_w(const char *s);
/* text that fits in w: size, else a smaller one down to min_size, else cut */
float d_text_fit(int size, int min_size, float x, float y, float w, const char *s, u32 argb);
float text_line(int size);
float text_mid(int size); /* line middle to baseline, in units */
/* the pad's symbols as characters, for d_text */
#define SYM_CROSS "\x01"
#define SYM_CIRCLE "\x02"
#define SYM_SQUARE "\x03"
#define SYM_TRIANGLE "\x04"
#define DOT " \x05 " /* the middle dot between parts */

/* the 3D layer (tile.c): a perspective in which, at the depth of the
   shelf, a unit is a pixel of the screen */
void d_begin_3d(void);
struct tile_look {
  float x, y, w, h;   /* the front face on the screen: top left, size (units) */
  float turn;         /* turned about its vertical axis, degrees */
  float lit;          /* 0 resting .. 1 focused */
  u32 base;           /* its colour (no cover) */
  int tex;            /* its cover texture (GL name) or 0 */
  float tex_u, tex_v; /* the part of the texture the cover uses */
  float fade;         /* 1 .. 0: how much of it is drawn */
};
/* the light above the shelf, in units (x, height above the floor) */
void light_at(float x, float y);
void d_tile(const struct tile_look *t);
void d_tile_reflection(const struct tile_look *t);

/* ---- the games (games.c) ---- */
#define MAX_GAMES 64
struct game {
  char dir[32], title[48], year[8], type[24], elf[48], args[96], desc[160], cover[32], cover_hd[32], saves[48], data[48];
  /* setup=: a program started before the game, each time (it says quickly
     when there is nothing to do): status 0 starts the game, 2 (stopped by
     the user) goes back quietly; its arguments setup_args= ({data} as in args=) */
  char setup[32], setup_args[96];
  u32 base;  /* a colour from the title, for a tile without a cover */
  int tex;   /* the cover's texture, 0 none */
  float tex_v; /* the part of its height the cover takes */
  u32 size_kb;
  u8 sec;    /* the section it is shown in (game.ini: section=media|emulation|apps), SEC_GAMES */
  u8 order;  /* game.ini order=<n>: by it first (0 without), then by title */
};
/* Home's sections (L1 and R1 go through them): what a program is, and Settings */
enum { SEC_GAMES, SEC_MEDIA, SEC_EMU, SEC_APPS, SEC_SETTINGS, NSECS };
extern const char *const sec_name[NSECS];
extern int home_sec;
extern struct game games[MAX_GAMES];
extern int ngames;
/* the storage devices (devlist.c): the kernel's list (<mx/dev.h>, kernel
   version 26: it loads the drivers and sees devices come and go) and host:
   when asked for */
#define PDEV_MAX 8
struct pdev {
  char root[12];  /* "usb0:" */
  char label[24]; /* the volume's label, "" none */
  u8 kind, fs, flags; /* MX_DEV_*, MX_DEV_FS_*, MX_DEV_SWAP/BUSY/READONLY */
  u64 size, free; /* bytes, 0 not known */
};
int pdevs_read(struct pdev *d, int max, int host); /* the devices there now (host: last if asked) */
u32 pdevs_gen(void);                               /* changes when the list does */
const char *pdev_name(const struct pdev *d);        /* "USB drive", "Memory card 2" */
void pdev_bytes(char *out, int len, u64 bytes);     /* "29.8 GB" */
/* the devices the shell looked for games on (games.c): the kernel's
   without the memory cards, host: with dev.host */
extern struct pdev gdevs[PDEV_MAX];
extern int ngdevs;
extern int gdev_games[PDEV_MAX]; /* games found on each */
/* a step of looking (call a few times a second): 1 when done */
int games_step(void);
void games_restart(void);
/* look again, a step each quarter second from the main loop */
void games_search(void);
extern int games_searching;
extern int host_only;                              /* tests: host: alone */
const char *game_root(int i);                      /* "usb0:" */
int games_find(const char *root, const char *dir); /* the game again after a search, -1 gone */
void games_textures(void); /* the covers as textures (after a game, again) */
void games_textures_free(void);
/* start game i beside the shell (mx_proc_spawn): its process, or -1 */
int game_launch(int i);
int game_setup(int i); /* its setup= program started, as game_launch */
/* a game paused behind the shell by the home button:
   its index in games, -1 none; game_close ends it (main.c) */
extern int run_game;
void game_close(void);
void game_path(char *out, int len, int i, const char *file);

/* ---- notifications (toast.c) ---- */
enum { N_INFO, N_PAD, N_DEVICE, N_WARN };
void toast(int kind, const char *title, const char *text, int urgent);
void toast_draw(void);
void toast_step(void);
int toast_dismiss(void); /* an urgent one waiting for a button: 1 if taken */
void notes_poll(void);   /* the kernel's new notifications as cards */
int toast_action(void);  /* the lit card's action (MX_NOTIFY_ACTION_*), 0 none */
void toast_done(void);   /* the lit card away */
void notes_boot_done(void); /* the boot screen is over: the quiet start ends soon */
void ram_step(void);     /* the swap file's card: there while MXOS has none */

/* ---- the settings (settings_store.c): a copy of the kernel's store ---- */
const char *cfg_get(const char *key, const char *def);
void cfg_set(const char *key, const char *value); /* saved by cfg_save */
void cfg_arg(const char *key, const char *value); /* this run only */
void cfg_refresh(void);                           /* the store's changes */
void cfg_load(void);
void cfg_save(void);

/* ---- screens ---- */
enum { SCR_BOOT, SCR_HOME, SCR_HUB, SCR_SETTINGS, SCR_LAUNCH, SCR_DITHER, SCR_GAMEPADS };
extern int screen;
extern u32 ticks;
extern u32 pad_pressed, pad_held;
void home_draw(void);
void home_input(void);
void home_enter(void);
void home_focus_rect(float r[4]);
/* Home with the game's hub k (0 .. 1) of the way in: the lit tile on its
   way to slab, the rest fading (hub.c) */
void home_scene(float k, const struct tile_look *slab);
u32 home_focus_colour(void);
void hub_draw(void);
void hub_input(void);
void settings_draw(void);
void settings_input(void);
void settings_enter(void);
enum { P_VIDEO, P_NETWORK, P_STORAGE, P_PADS, P_TIME, P_SYSTEM, NPAGES };
/* the date and time (clock.c, kernel version 29) */
struct mx_tm;
struct clock_zone {
  int min; /* east of UTC */
  const char *place;
};
extern const struct clock_zone clock_zones[];
extern const int clock_nzones;
extern const char *const clock_month[12], *const clock_wday[7];
int clock_zone(void);  /* time.zone */
int clock_dst(void);   /* time.dst on */
int clock_24h(void);   /* clock.format 24 (the default) */
int clock_mdays(int year, int month);
int clock_weekday(int year, int month, int day); /* 0 Sunday */
long long clock_utc_of(const struct mx_tm *local); /* the zone's and summer time's offset taken off */
int clock_now(struct mx_tm *tm);                   /* the local time; -1 if not known */
void clock_hm(const struct mx_tm *tm, char *s, unsigned n); /* "14:05" or "2:05 PM" */
int clock_text(char *s, unsigned n);                 /* the clock now; -1 if not known */
void settings_goto(int page, int swap_row); /* a page open (a notification's action) */
void video_apply(void);
void boot_draw(void);
/* dithering into 16 bits (dither.c) */
void dither_apply(void);
void dither_next(int step);
int dither_choice(void);
const char *dither_name(int d);
void dither_draw(void);
void dither_input(void);
int focus_game(void); /* the game the shelf has lit, or -1 */
void status_draw(void);
/* the tray at the top right (tray.c): the players' pads, the devices */
void tray_draw(void);
extern int pad_on; /* the pads started (main.c) */
/* a settings page's rows (settings.c): a row with its label and value, a
   note under the rows, choices side by side (returns their left edge) */
void row_line(float x0, float x1, float y, const char *label, const char *value, int chosen, int on);
void row_note(float x, float y, const char *s);
float row_seg(float x1, float y, const char *const *l, int n, int on, int lit);
/* the controllers' page (pads.c), in Settings and in the game menu: act,
   it takes the buttons; crumb, a label above it (the game menu's) */
void pads_enter(void);
void pads_draw(float x0, float x1, float y, int act, const char *crumb);
int pads_input(u32 pressed); /* 0: circle, leave it */
const char *const *pads_hints(const char *back);
/* the game menu's Controllers (hub.c) */
void gamepads_draw(void);
void gamepads_input(void);
/* hints at the bottom right: pairs of symbol, label; NULL ends */
void hints_draw(const char *const *pairs);

/* a screen fading in or out: 1 .. 0 for all of the 2D layer drawn with it
   (text and lines through argb, the rounded shapes mixed with what is
   under them) */
extern float ui_fade;
static inline u32 argb(u32 rgb, float a) {
  int v = (int)(a * ui_fade * 255.0f + 0.5f);
  v = v < 0 ? 0 : v > 255 ? 255 : v;
  return (u32)v << 24 | rgb;
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

#endif
