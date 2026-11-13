/* A stand-in for a game in the shell's tests: an MXOS program that waits a
   second and ends with the status its first argument gives (0 without). */
#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/thread.h>

int main(int argc, char **argv) {
  int st = 0;
  if (argc > 1)
    for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++)
      st = st * 10 + *s - '0';
  mx_printf("fakegame: running, ends with %d\n", st);
  mx_sleep_us(1000000);
  return st;
}
