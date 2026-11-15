/* A stand-in for a game in the shell's tests: an MXOS program that says how
   much memory it has (the frame pool: what a game gets beside the kernel and
   what stays of the shell), waits a second and ends with the status its
   first argument gives (0 without). With a second argument "home" it runs
   for 30 s instead, a step each 20 ms in which the kernel may take the home
   button (kernel version 25: the shell in front, this one paused), and with
   "home=<n>" it presses it itself at step n and 4 s after (the kernel's
   "@@PAD" test lines, a test kernel only). Before the first press it adds
   a notification (kernel version 26), which the shell shows in front. */
#include <mx/debug.h>
#include <mx/gs.h>
#include <mx/video.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <string.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/notify.h>
#include <mx/thread.h>
#include <mx/vm.h>
#include <stdlib.h>

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
  mx_printf("fakegame: running, ends with %d\n", st);
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
