/* MXSHELL2: the video mode from the settings, for the shell and its apps:
   libgl's mode, the layout's units and the font of its shape of pixel.

   The system's keys (<mx/screen.h>, kernel version 31) are read as the
   kernel reads them (mx_screen_conf_from over the shell's copy of the
   store): video.mode, video.out, video.aspect; the kernel shows its own
   screen and every program in that mode. The shell's own: video.depth,
   video.deflicker, video.beam. */
#include <mx/debug.h>
#include <mx/screen.h>
#include <mx/video.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* the modes: the system's name, what the page says, libgl's mode, the
   shape of its pixels (the font atlas), 16:9, frames a second */
const struct vmode vmodes[VM_MODES] = {
    [VM_240P] = {"240p", "240p  640 x 224, 60 Hz", MXGL_240P, SHAPE_P240, 0, 60},
    [VM_288P] = {"288p", "288p  640 x 256, 50 Hz", MXGL_288P, SHAPE_P288, 0, 50},
    [VM_480I] = {"480i", "480i  640 x 448, 60 Hz", MXGL_NTSC, SHAPE_NTSC, 0, 60},
    [VM_576I] = {"576i", "576i  640 x 512, 50 Hz", MXGL_PAL, SHAPE_PAL, 0, 50},
    [VM_480P] = {"480p", "480p  640 x 480, 60 Hz", MXGL_480P, SHAPE_P480, 0, 60},
    [VM_576P] = {"576p", "576p  640 x 576, 50 Hz", MXGL_576P, SHAPE_P576, 0, 50},
    [VM_720P] = {"720p", "720p  1280 x 720, 60 Hz", MXGL_720P, SHAPE_P720, 1, 60},
    [VM_720P50] = {"720p50", "720p  1280 x 720, 50 Hz", MXGL_720P50, SHAPE_P720, 1, 50},
    [VM_1080I] = {"1080i", "1080i  1920 x 1080, 60 Hz", MXGL_1080I, SHAPE_HD, 1, 60},
    [VM_1080I50] = {"1080i50", "1080i  1920 x 1080, 50 Hz", MXGL_1080I50, SHAPE_HD, 1, 50},
    /* 1080p: drawn as 1920 x 540, each line shown twice */
    [VM_1080P24] = {"1080p24", "1080p  1920 x 1080, 24 Hz", MXGL_1080P24, SHAPE_HD540, 1, 24},
    [VM_1080P25] = {"1080p25", "1080p  1920 x 1080, 25 Hz", MXGL_1080P25, SHAPE_HD540, 1, 25},
    [VM_1080P30] = {"1080p30", "1080p  1920 x 1080, 30 Hz", MXGL_1080P30, SHAPE_HD540, 1, 30},
};
int video_mode = VM_480I, video_shape = SHAPE_NTSC;
float scr_w = 960, px_x = 1, px_y = 1;

/* the shell's copy of the store, for mx_screen_conf_from */
static int cget(const char *key, char *value, u32 size) {
  const char *v = cfg_get(key, "");
  u32 n = (u32)strlen(v);
  if (!n || n >= size)
    return 0;
  memcpy(value, v, n + 1);
  return (int)n;
}

void video_conf(struct mx_screen_conf *c) { mx_screen_conf_from(c, cget); }

int video_of_screen(u32 mode) {
  for (int i = 0; i < VM_MODES; i++)
    if (mx_screen_mode_of(vmodes[i].key) == mode)
      return i;
  return VM_480I;
}

/* video.aspect: the television's shape in the 480 and 576 modes, 4:3 or
   16:9 (anamorphic: the same 640 pixels across a wide screen); HD is
   always 16:9 */
int video_sd_wide(void) { return !strcmp(cfg_get("video.aspect", "4:3"), "16:9"); }

/* video.deflicker: in 480i and 576i, a softer picture against flicker
   (libgl's mxgl_deflicker: the PS2's two read circuits a line apart,
   blended). It needs full-height buffers, so those modes are then drawn
   without beam racing (16-bit colour); off, beam racing draws every line of
   the picture, for the television's deinterlacer */
int video_deflicker(void) { return !strcmp(cfg_get("video.deflicker", "off"), "on"); }

/* video.mode as the kernel uses it: the region's 480i or 576i without it,
   with a name it does not know, or with an HD or progressive mode on RGB */
int video_choice(void) {
  struct mx_screen_conf c;
  video_conf(&c);
  return video_of_screen(c.mode);
}

/* what video_apply set up for: video_follow sets the mode again when it
   changed under the shell */
static char applied[48];
static void now_key(char *k, u32 n) {
  struct mx_screen_conf c;
  video_conf(&c);
  mx_snprintf(k, n, "%u %u %u %s %s", c.mode, c.rgb, c.wide, cfg_get("video.depth", "32"),
              cfg_get("video.deflicker", "off"));
}

/* the mode from the settings, and video.depth (32: 24-bit colour where it
   fits, 16: dithered) */
void video_apply(void) {
  int m = video_choice();
  now_key(applied, sizeof applied);
  int laced = m == VM_480I || m == VM_576I, soft = laced && video_deflicker();
  int flags = cfg_get("video.beam", "1")[0] == '1' && !soft ? MXGL_BEAM : 0;
  if (flags && cfg_get("video.depth", "32")[0] == '3')
    flags |= MXGL_RGB24;
  mxgl_set_mode(vmodes[m].gl | flags);
  mxgl_deflicker(soft);
  /* libgl falls back to the console's own mode for one it cannot set */
  int got = mxgl_mode();
  video_mode = m;
  if (got != vmodes[m].gl)
    video_mode = got == MXGL_PAL ? VM_576I : VM_480I;
  video_shape = vmodes[video_mode].shape;
  int wide = vmodes[video_mode].wide;
  if (!wide && video_sd_wide()) {
    static const int w16[] = {[SHAPE_P576] = SHAPE_P576W, [SHAPE_P480] = SHAPE_P480W, [SHAPE_NTSC] = SHAPE_NTSCW,
                              [SHAPE_PAL] = SHAPE_PALW, [SHAPE_P240] = SHAPE_P240W, [SHAPE_P288] = SHAPE_P288W};
    video_shape = w16[video_shape], wide = 1;
  }
  int w, h;
  mxgl_screen(&w, &h);
  scr_w = wide ? 1280.0f : 960.0f;
  px_x = (float)w / scr_w, px_y = (float)h / 720.0f;
  if (video_shape == SHAPE_HD540)
    px_y = 0.75f; /* 1080p: drawn as 540 lines, each shown twice */
  mxgl_window((int)scr_w, 720);
  frame_dt = 1.0f / (float)vmodes[video_mode].hz;
  /* 16-bit colour is dithered (draw.c, dither.c) */
  dither_bias = mxgl_mode_flags() & MXGL_RGB24 ? 0 : 4;
  dither_apply();
  text_init();
}

void video_follow(void) {
  char k[48];
  now_key(k, sizeof k);
  if (strcmp(k, applied)) {
    mx_printf("mxshell: video settings changed: %s\n", k);
    video_apply();
  }
}
