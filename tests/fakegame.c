/* A stand-in for a game in the shell's tests: an MXOS program that says how
   much memory it has (the frame pool: what a game gets beside the kernel and
   what stays of the shell), waits a second and ends with the status its
   first argument gives (0 without). */
#include <mx/debug.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/thread.h>
#include <mx/vm.h>

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
  mx_sleep_us(1000000);
  return st;
}
