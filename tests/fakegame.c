/* A stand-in for a game in the shell's tests: an MXOS program that says how
   much memory it has (the frame pool: what a game gets beside the kernel and
   what stays of the shell), waits a second and ends with the status its
   first argument gives (0 without). With a second argument "home" it runs
   for 30 s instead, a step each 20 ms in which the kernel may take the home
   button (kernel version 25: the shell in front, this one paused), and with
   "home=<n>" it presses it itself at step n and 4 s after (the kernel's
   "@@PAD" test lines, a test kernel only). Before the first press it adds
   a notification (kernel version 26), which the shell shows in front. With
   "trash" it fills all of GS memory with a pattern first (what a big game
   leaves behind for the shell); "trash=<MB>" also writes that much memory
   (the shell's pageable memory to the swap file, as a big game does). */
#include <mx/debug.h>
#include <mx/gs.h>
#include <mx/video.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <mx/pkt.h>
#include <string.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/notify.h>
#include <mx/thread.h>
#include <mx/vm.h>
#include <stdlib.h>

/* 4 MiB of GS memory as a 1024 x 1024 CT32 buffer, 8 lines at a time (DMA
   from the core) */
static u32 noise[1024 * 8] MX_ALIGNED(64) MX_FAST_BSS;
static u32 up[256] MX_ALIGNED(64) MX_FAST_BSS;
static void trash_vram(void) {
  for (u32 i = 0; i < 1024 * 8; i++)
    noise[i] = 0x80000000u | (i * 2654435761u >> 8);
  mx_dcache_writeback(noise, sizeof noise);
  for (u32 y = 0; y < 1024; y += 8) {
    struct mx_pkt p;
    mx_pkt_begin(&p, up, sizeof up, 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_upload(&p, 0, 16, MX_GS_PSM_CT32, 0, y, 1024, 8, noise);
    mx_gs_finish_ad(&p);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    u32 f0 = mx_gs.finish;
    mx_gs_send(up);
    mx_gs_wait_finish(f0, 1);
  }
  mx_printf("fakegame: GS memory filled\n");
}

int main(int argc, char **argv) {
  int st = 0;
  if (argc > 1)
    for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++)
      st = st * 10 + *s - '0';
  struct mx_mem_pressure p;
  mx_mem_pressure(&p);
  mx_printf("fakegame: frames %u (%u KB), free %u (%u KB), pinned %u (%u KB)\n", p.frames,
            p.frames * (MX_VM_PAGE / 1024), p.free_frames, p.free_frames * (MX_VM_PAGE / 1024), p.pinned_frames,
            p.pinned_frames * (MX_VM_PAGE / 1024));
  for (int i = 1; i < argc; i++) /* what the shell passed ({data}, {version}) */
    mx_printf("fakegame: argv[%d] %s\n", i, argv[i]);
  mx_printf("fakegame: running, ends with %d\n", st);
  if (argc > 2 && !strncmp(argv[2], "trash", 5)) {
    mx_gs_init_mode(640, 448, 0);
    trash_vram();
    u32 mb = argv[2][5] == '=' ? (u32)atoi(argv[2] + 6) : 0;
    for (u32 i = 0; i < mb; i++) {
      static char *volatile kept[128];
      char *m = malloc(1u << 20);
      kept[i & 127] = m;
      if (!m) {
        mx_printf("fakegame: %u MB written, no more memory\n", i);
        break;
      }
      memset(m, 0x5A, 1u << 20);
    }
    mx_mem_pressure(&p);
    mx_printf("fakegame: %u MB written, free %u KB\n", mb, p.free_frames * (MX_VM_PAGE / 1024));
    mx_sleep_us(1000000);
    return st;
  }
  if (argc > 2 && !strncmp(argv[2], "home", 4)) {
    int press = argv[2][4] == '=' ? atoi(argv[2] + 5) : -1;
    /* a video mode told to the kernel: its home button needs one */
    int pal = mx_video_default() == &mx_video_pal;
    mx_gs_init_mode(640, pal ? 512 : 448, pal ? MX_GS_PAL : 0);
    for (int n = 0; n < 1500; n++) {
      if (press >= 0 && n == press - 25)
        mx_printf("fakegame: notification %d\n",
                  mx_notify(MX_NOTIFY_PROGRAM, 0, "Saved", "A Home Test kept its progress on the memory card"));
      if (press >= 0 && (n == press || n == press + 200))
        mx_printf("@@PAD %x 300\n", MX_PAD_HOME);
      if (n % 100 == 0)
        mx_printf("fakegame: step %d\n", n);
      /* what a frame boundary does: the pad read, then the home button */
      struct mx_pad_state ps;
      (void)mx__kernel->pad_read(0, &ps, 0);
      u32 pages = mx__kernel->home_wanted();
      if (pages)
        mx__kernel->home_run(~0u, pages);
      mx_sleep_us(20000);
    }
    return st;
  }
  mx_sleep_us(1000000);
  return st;
}
