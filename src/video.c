/* MXSHELL2: the video mode from the settings (video.mode, video.depth,
   video.aspect, video.deflicker), for the shell and its apps: libgl's mode,
   the layout's units and the font of its shape of pixel. */
#include <mx/video.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

/* the modes: the setting's key, its name, libgl's mode, the shape of its
   pixels (the font atlas), 16:9, frames a second */
const struct vmode vmodes[VM_MODES] = {
    [VM_1080I] = {"1080i", "1080i  1920 x 1080, 60 Hz", MXGL_1080I, SHAPE_HD, 1, 60},
    [VM_1080I50] = {"1080i50", "1080i  1920 x 1080, 50 Hz", MXGL_1080I50, SHAPE_HD, 1, 50},
    [VM_1080P30] = {"1080p30", "1080p  1920 x 1080, 30 Hz", MXGL_1080P30, SHAPE_HD540, 1, 30},
    [VM_1080P25] = {"1080p25", "1080p  1920 x 1080, 25 Hz", MXGL_1080P25, SHAPE_HD540, 1, 25},
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
int video_sd_wide(void) { return !strcmp(cfg_get("video.aspect", "4:3"), "16:9"); }

/* video.deflicker: in NTSC and PAL, a softer picture against flicker
   (libgl's mxgl_deflicker: the PS2's two read circuits a line apart,
   blended). It needs full-height buffers, so those modes are then drawn
   without beam racing (16-bit colour); off, beam racing draws every line of
   the picture, for the television's deinterlacer */
int video_deflicker(void) { return !strcmp(cfg_get("video.deflicker", "off"), "on"); }

int video_choice(void) {
  /* without a setting "sd", the console's own mode from its region (PAL or
     NTSC): every television and cable shows that */
  const char *v = cfg_get("video.mode", "sd");
  for (int i = 0; i < VM_MODES; i++)
    if (!strcmp(v, vmodes[i].key))
      return i;
  return mx_video_default() == &mx_video_pal ? VM_PAL : VM_NTSC;
}

/* the mode from the settings: video.mode (a key of vmodes, or sd), and
   video.depth (32: 24-bit colour where it fits, 16: dithered) */
void video_apply(void) {
  int m = video_choice();
  int laced = m == VM_NTSC || m == VM_PAL, soft = laced && video_deflicker();
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
  if (!wide && video_sd_wide()) {
    static const int w16[] = {[SHAPE_P576] = SHAPE_P576W, [SHAPE_P480] = SHAPE_P480W, [SHAPE_NTSC] = SHAPE_NTSCW,
                              [SHAPE_PAL] = SHAPE_PALW};
    video_shape = w16[video_shape], wide = 1;
  }
  int w, h;
  mxgl_screen(&w, &h);
  scr_w = wide ? 1280.0f : 960.0f;
  px_x = (float)w / scr_w, px_y = (float)h / 720.0f;
  if (video_shape == SHAPE_HD540)
    px_y = 0.75f; /* 1080p30, 1080p25: drawn as 540 lines, each shown twice */
  mxgl_window((int)scr_w, 720);
  frame_dt = 1.0f / (float)vmodes[video_mode].hz;
  /* 16-bit colour is dithered (draw.c, dither.c) */
  dither_bias = mxgl_mode_flags() & MXGL_RGB24 ? 0 : 4;
  dither_apply();
  text_init();
}

