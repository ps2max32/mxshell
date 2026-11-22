/* Writing a file beside the work that makes its bytes: two buffers of
   AW_BUF, one filled (the download, the unpacking) while a thread of its own
   writes the other to the device in one piece.

   A USB drive takes a large write much faster than many small ones (the
   PS2's OHCI: about 0.9 MB/s at its best), and the network goes on while the
   drive writes, so a download runs at the slower of the two instead of
   their times added: written as received, 16 KiB a TLS record, it was
   about 200 KB/s. */
#include "store.h"

#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <string.h>

#define AW_BUF (256u << 10)

static u8 buf[2][AW_BUF] MX_ALIGNED(64);
static volatile u32 queued[2];
static int sem_free = -1, sem_full = -1;
static int fd, cur, wcur;
static u32 fill;
static volatile int err;
static u8 stack[16 << 10] MX_ALIGNED(16) MX_IRQ_BSS; /* a thread stack: core */

static void writer(void *arg) {
  (void)arg;
  for (;;) {
    mx_sema_wait(sem_full);
    u32 n = queued[wcur];
    if (n && !err && mx_fio_write(fd, buf[wcur], n) != 0)
      err = 1;
    queued[wcur] = 0;
    wcur ^= 1;
    mx_sema_signal(sem_free);
  }
}

void aw_begin(int file) {
  if (sem_free < 0) {
    sem_free = mx_sema_create(2, 2);
    sem_full = mx_sema_create(0, 2);
    mx_thread_start(writer, 0, stack, sizeof stack, mx_thread_prio(mx_thread_self()));
  }
  fd = file, err = 0, fill = 0, cur = 0, wcur = 0;
  mx_sema_wait(sem_free); /* the first buffer */
}

static void submit(void) {
  queued[cur] = fill;
  mx_sema_signal(sem_full);
  cur ^= 1, fill = 0;
  mx_sema_wait(sem_free); /* the other one, once it is written */
}

int aw_write(const void *data, u32 n) {
  const u8 *p = data;
  while (n && !err) {
    u32 k = AW_BUF - fill < n ? AW_BUF - fill : n;
    memcpy(buf[cur] + fill, p, k);
    fill += k, p += k, n -= k;
    if (fill == AW_BUF)
      submit();
  }
  return err ? -1 : 0;
}

int aw_end(void) {
  if (fill)
    submit();
  /* both written: the one held now is free, wait for the other */
  mx_sema_wait(sem_free);
  mx_sema_signal(sem_free);
  mx_sema_signal(sem_free);
  return err ? -1 : 0;
}
