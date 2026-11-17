/* usbcopy: copies files to and from the USB stick, started from the PC
   through the host link (an MXOS program):

     usbcopy <from> <to> [<from> <to> ...]
     usbcopy @<list>      the pairs from a file, "<from> <to>" a line

   e.g. usbcopy host:coverhd.t8 usb0:/PS2MAX/quake1/coverhd.t8. The kernel
   (version 26) has the USB drivers loaded; usbcopy checks usb0: is there,
   then copies each file (any size, a megabyte at a time; its directories
   made where they are not there) and reads it back to compare. Ends with
   the number of files that failed. */
#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <string.h>

#define PIECE (1024 * 1024)
static u8 buf[PIECE] MX_ALIGNED(64), back[PIECE] MX_ALIGNED(64);

static int read_all(const char *path, u8 *to, u32 room) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  int n = 0, r;
  while ((u32)n < room && (r = mx_fio_read(fd, to + n, room - (u32)n)) > 0)
    n += r;
  mx_fio_close(fd);
  return n;
}

/* the directories of a path, made where they are not there */
static void dirs_of(const char *path) {
  char d[256];
  u32 n = (u32)strlen(path);
  if (n >= sizeof d)
    return;
  memcpy(d, path, n + 1);
  for (char *c = strchr(d, ':'); c && *c; c++)
    if (*c == '/' && c[-1] != ':') {
      *c = 0;
      mx_fio_mkdir(d, 0777); /* there already: fine */
      *c = '/';
    }
}

/* in pieces of PIECE bytes (any size), then read back piece by piece */
static int copy(const char *from, const char *to) {
  int in = mx_fio_open(from, MX_FIO_RDONLY);
  int size = in < 0 ? -1 : mx_fio_lseek(in, 0, MX_FIO_SEEK_END);
  if (size < 0 || mx_fio_lseek(in, 0, MX_FIO_SEEK_SET) != 0) {
    if (in >= 0)
      mx_fio_close(in);
    mx_printf("usbcopy: %s: not there\n", from);
    return -1;
  }
  dirs_of(to);
  int out = mx_fio_open(to, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (out < 0) {
    mx_fio_close(in);
    mx_printf("usbcopy: %s: cannot write (%d)\n", to, out);
    return -1;
  }
  int bad = 0;
  for (u32 done = 0; done < (u32)size && !bad; done += PIECE) {
    u32 n = (u32)size - done < PIECE ? (u32)size - done : PIECE;
    if (mx_fio_read(in, buf, n) != (int)n || mx_fio_write(out, buf, n) < 0)
      bad = 1;
  }
  mx_fio_close(in);
  mx_fio_close(out);
  /* what is read back decides */
  in = mx_fio_open(from, MX_FIO_RDONLY);
  out = mx_fio_open(to, MX_FIO_RDONLY);
  if (in < 0 || out < 0 || mx_fio_lseek(out, 0, MX_FIO_SEEK_END) != size || mx_fio_lseek(out, 0, MX_FIO_SEEK_SET) != 0)
    bad = 1;
  for (u32 done = 0; done < (u32)size && !bad; done += PIECE) {
    u32 n = (u32)size - done < PIECE ? (u32)size - done : PIECE;
    if (mx_fio_read(in, buf, n) != (int)n || mx_fio_read(out, back, n) != (int)n || memcmp(buf, back, n))
      bad = 1;
  }
  if (in >= 0)
    mx_fio_close(in);
  if (out >= 0)
    mx_fio_close(out);
  if (bad) {
    mx_printf("usbcopy: %s -> %s: not the same after writing\n", from, to);
    return -1;
  }
  mx_printf("usbcopy: %s -> %s: %d bytes\n", from, to, size);
  return 0;
}

int main(int argc, char **argv) {
  int d = mx_fio_dopen("usb0:/");
  if (d < 0) {
    mx_printf("usbcopy: no usb0:\n");
    return 1;
  }
  mx_fio_dclose(d);
  int bad = 0, n = 0;
  if (argc == 2 && argv[1][0] == '@') {
    static char list[64 * 1024];
    int len = read_all(argv[1] + 1, (u8 *)list, sizeof list - 1);
    if (len < 0) {
      mx_printf("usbcopy: %s: not there\n", argv[1] + 1);
      return 1;
    }
    list[len] = 0;
    for (char *s = list; *s;) {
      char *a = s, *b = 0, *e = s;
      while (*e && *e != '\n')
        e++;
      char *next = *e ? e + 1 : e;
      char *sp = a;
      while (sp < e && *sp != ' ')
        sp++;
      if (sp < e) {
        *sp = 0, b = sp + 1;
        char *end = e;
        while (end > b && (end[-1] == '\r' || end[-1] == ' '))
          end--;
        *end = 0;
      }
      s = next;
      if (b && *b)
        bad += copy(a, b) < 0, n++;
    }
  } else
    for (int i = 1; i + 1 < argc; i += 2)
      bad += copy(argv[i], argv[i + 1]) < 0, n++;
  mx_printf("usbcopy: %d of %d failed\n", bad, n);
  return bad;
}
