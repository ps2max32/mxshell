/* MXSHELL2: the overscan test (Settings > Video). The whole picture of the
   SD modes (480 and 576 lines, 240p and 288p) is 704 pixels wide (BT.601);
   a TV that shows all of it can take video.overscan off. libgl is put aside (as for a game) and the
   GS shows a frame of its own in the mode in use with overscan off: a
   1-pixel white border at the picture's edges, a second line 4 pixels in,
   and the area every TV shows (overscan on) in the lamp's colour. Until a
   button is pressed, or 30 s. */
#include <mx/debug.h>
#include <mx/display.h>
#include <mx/gs.h>
#include <mx/pad.h>
#include <mx/pkt.h>
#include <mx/rt.h>
#include <mx/screen.h>
#include <mx/thread.h>

#include "mxgl.h"
#include "pn.h"

static u32 pk[1024] MX_ALIGNED(64) MX_CORE_BSS; /* the DMAC reads it: 256 quadwords */
static struct mx_display disp[2];
int overscan_shot;

static void box(struct mx_pkt *p, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  mx_pkt_ad_begin(p, 4, 1);
  mx_pkt_ad(p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, 0x80, 1.0f));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
}

/* a 1-pixel outline of w x h at (x, y) */
static void outline(struct mx_pkt *p, u32 x, u32 y, u32 w, u32 h, u32 rgb) {
  box(p, x, y, x + w, y + 1, rgb);
  box(p, x, y + h - 1, x + w, y + h, rgb);
  box(p, x, y, x + 1, y + h, rgb);
  box(p, x + w - 1, y, x + w, y + h, rgb);
}

int overscan_test(void) {
  struct mx_screen_conf c;
  video_conf(&c);
  int vm = video_of_screen(c.mode);
  if (VM_HD(vm))
    return -1; /* HD has no overscan setting */
  c.full = 1;
  u32 lines = vm == VM_576I || vm == VM_576P ? 576 : vm == VM_480I || vm == VM_480P ? 480 : vm == VM_288P ? 288 : 240;
  u32 safe = lines == 576 ? 512 : lines == 480 ? 448 : lines == 288 ? 256 : 224;
  struct mx_screen s;
  if (mx_screen_fit(&c, c.mode, 704, lines, 0, &s) < 0)
    return -1;
  struct mx_video_mode m;
  mx_screen_video(&c, &s, &m);

  shell_away();
  mx_vram_reset();
  if (mx_gs_init_video(&m) < 0) {
    shell_back();
    return -1;
  }
  mx_gs.w = 704, mx_gs.h = lines, mx_gs.fbw = (704 + 63) / 64;
  u32 pages = mx_rt_pages(704, lines, MX_GS_PSM_CT32);
  mx_gs.fbp[0] = mx_gs.fbp[1] = mx_vram_alloc(pages);
  mx_gs.zbp = mx_vram_alloc(pages);
  mx_rt_init(&mx_gs.rt[0], mx_gs.fbp[0], 704, lines, MX_GS_PSM_CT32, mx_gs.zbp, MX_GS_PSM_Z24);
  mx_display_init(&disp[0], &mx_gs.rt[0]);
  disp[1] = disp[0];

  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_draw_env(&p, 0);
  mx_gs_clear(&p, 8, 8, 8);
  outline(&p, 0, 0, 704, lines, 0xECEAE6);
  outline(&p, 4, 4, 696, lines - 8, 0x7D8187);
  outline(&p, 32, (lines - safe) / 2, 640, safe, 0xE9C98F);
  /* the middle */
  u32 arm = lines / 24;
  box(&p, 352 - 20, lines / 2, 352 + 20, lines / 2 + 1, 0x7D8187);
  box(&p, 352, lines / 2 - arm, 352 + 1, lines / 2 + arm, 0x7D8187);
  mx_pkt_end(&p, 0, 0);
  if (mx_pkt_finish(&p) <= 0)
    mx_printf("mxshell: the overscan test's packet is too long\n");
  mx_pkt_writeback(&p);
  mx_gs_send(mx_pkt_start(&p));
  mx_gs_displays(disp, 2);
  mx_display_apply(&disp[0]);
  mx_printf("mxshell: overscan test, %s at %u x %u\n", mx_screen_name(c.mode), s.magh, s.magv);

  /* tests: its picture to host:overscan.tga, and back */
  int n = 30 * 50;
  if (overscan_shot) {
    mx_sleep_us(100000);
    mx_printf("mxshell: overscan test picture saved: %d\n", mx_gs_save_tga(0, "host:overscan.tga"));
    n = 0;
  }
  /* until a button is pressed (the ones held now let go first) */
  u32 held = ~0u;
  for (int i = 0; i < n; i++) {
    mx_sleep_us(20000);
    u32 b = pad_on ? mx_pad_buttons(0) | mx_pad_buttons(1) : 0;
    if (b & ~held)
      break;
    held &= b;
  }
  shell_back();
  return 0;
}
