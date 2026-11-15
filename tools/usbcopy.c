/* usbcopy: copies files to and from the USB stick, started from the PC
   through the host link (an MXOS program):

     usbcopy <from> <to> [<from> <to> ...]
     usbcopy @<list>      the pairs from a file, "<from> <to>" a line

   e.g. usbcopy host:coverhd.t8 usb0:/PS2MAX/quake1/coverhd.t8. The kernel
   (version 26) has the USB drivers loaded; usbcopy checks usb0: is there,
   then copies each file and reads it back to compare. Ends with the number
   of files that failed. */
#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <string.h>

static u8 buf[256 * 1024], back[256 * 1024];

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

static int copy(const char *from, const char *to) {
  int n = read_all(from, buf, sizeof buf);
  if (n < 0 || n == (int)sizeof buf) {
    mx_printf("usbcopy: %s: %s\n", from, n < 0 ? "not there" : "too large");
    return -1;
  }
  int fd = mx_fio_open(to, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("usbcopy: %s: cannot write (%d)\n", to, fd);
    return -1;
  }
  int w = mx_fio_write(fd, buf, (u32)n);
  mx_fio_close(fd);
  int m = read_all(to, back, sizeof back);
  /* what is read back decides (a write to host: answers 0, not the bytes) */
  (void)w;
  if (m != n || memcmp(buf, back, (u32)n)) {
    mx_printf("usbcopy: %s: wrote %d of %d, read back %d: not the same\n", to, w, n, m);
    return -1;
  }
  mx_printf("usbcopy: %s -> %s: %d bytes\n", from, to, n);
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
    static char list[8192];
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
