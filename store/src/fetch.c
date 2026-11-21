/* The store's worker: one thread below the screens' priority that does all
   of the network and file work (a TLS handshake is half a second of the
   EE's), so the screens keep their frames. The main thread gives it a job
   or wants pictures; it says how far it is in job and the pictures' state.

   Pictures are checked against the index's SHA-256 and kept on the device
   the store installs to, <device>/PS2MAX/.store/img/<sha256>, so a picture
   that did not change is not loaded again. */
#include "https.h"
#include "store.h"

#include <mbedtls/sha256.h>
#include <mx/kernel.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/pic.h>
#include <mx/thread.h>
#include <stdlib.h>
#include <string.h>

#define FETCH_EFILE (-20)  /* a file that does not open or read */
#define FETCH_EBIG (-21)   /* larger than the caller takes */
#define FETCH_ESHA (-22)   /* not what the index says */

char site_base[160] = "https://ps2max32.github.io/store/";
struct picture covers[MAX_ENTRIES], pages[MAX_ENTRIES][PAGE_PICS], view_pic;
volatile int index_state;
char index_message[96];
struct job job;
struct sdev sdevs[SDEV_MAX];
int nsdevs, cur_dev = -1;

u32 kernel_version(void) { return mx__kernel ? mx__kernel->version : 0; }

const char *fetch_error(int e) {
  if (e == FETCH_EFILE)
    return "The file is not there";
  if (e == FETCH_EBIG)
    return "Larger than the index says";
  if (e == FETCH_ESHA)
    return "Not the file the index names";
  return https_strerror(e);
}

int fetch(const char *url, fetch_sink sink, void *ctx) {
  if (!strncmp(url, "https://", 8)) {
    static struct https_result r;
    int e = https_get(url, (https_sink)sink, ctx, &r);
    if (e)
      mx_printf("store: %s: %s %s (status %d)\n", url, https_strerror(e), https_tls_error(), r.status);
    return e;
  }
  if (!strchr(url, ':')) {
    /* relative to the site */
    char full[320];
    mx_snprintf(full, sizeof full, "%s%s", site_base, url);
    return fetch(full, sink, ctx);
  }
  /* a file (tests: host:site/...) */
  int fd = mx_fio_open(url, MX_FIO_RDONLY);
  if (fd < 0)
    return FETCH_EFILE;
  static u8 chunk[32 << 10];
  int n, r = 0;
  while ((n = mx_fio_read(fd, chunk, sizeof chunk)) > 0)
    if (sink(ctx, chunk, (u32)n) < 0) {
      r = HTTPS_ESINK;
      break;
    }
  mx_fio_close(fd);
  return n < 0 ? FETCH_EFILE : r;
}

/* ---- into memory ---- */
struct mem {
  u8 *data;
  u32 n, max;
};
static int mem_sink(void *ctx, const void *d, u32 size) {
  struct mem *m = ctx;
  if (m->n + size > m->max)
    return -1;
  memcpy(m->data + m->n, d, size);
  m->n += size;
  return 0;
}

int write_file(const char *path, const void *data, u32 n) {
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  int r = mx_fio_write(fd, data, n);
  mx_fio_close(fd);
  return r == 0 ? 0 : -1;
}

void mkdirs(const char *path) {
  char p[256];
  mx_snprintf(p, sizeof p, "%s", path);
  for (char *s = strchr(p, ':'); s && *s; s++)
    if (*s == '/' && s[-1] != ':') {
      *s = 0;
      mx_fio_mkdir(p, 0777);
      *s = '/';
    }
}

/* ---- pictures ---- */
static void hex(const u8 *h, char *out) {
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++)
    out[2 * i] = d[h[i] >> 4], out[2 * i + 1] = d[h[i] & 15];
  out[64] = 0;
}

/* a picture's bytes: from the cache, else the site (then into the cache) */
u8 *pic_bytes(const struct pic_ref *ref, u32 max, u32 *n) {
  u8 *buf = malloc(max);
  if (!buf)
    return 0;
  char name[72], path[128];
  hex(ref->sha, name);
  u8 h[32];
  if (cur_dev >= 0) {
    mx_snprintf(path, sizeof path, "%s/PS2MAX/.store/img/%s", sdevs[cur_dev].root, name);
    int fd = mx_fio_open(path, MX_FIO_RDONLY);
    if (fd >= 0) {
      int got = mx_fio_read(fd, buf, max);
      mx_fio_close(fd);
      if (got > 0) {
        mbedtls_sha256(buf, (u32)got, h, 0);
        if (!memcmp(h, ref->sha, 32)) {
          *n = (u32)got;
          return buf;
        }
      }
    }
  }
  struct mem m = {buf, 0, max};
  int r = fetch(ref->path, mem_sink, &m);
  mbedtls_sha256(buf, m.n, h, 0);
  if (r || memcmp(h, ref->sha, 32)) {
    mx_printf("store: %s: %s\n", ref->path, r ? fetch_error(r) : "its SHA-256 is not the index's");
    free(buf);
    return 0;
  }
  if (cur_dev >= 0 && sdevs[cur_dev].writable) {
    mkdirs(path);
    if (write_file(path, buf, m.n))
      mx_printf("store: %s: not kept\n", path);
  }
  *n = m.n;
  return buf;
}

/* pixels of w x h into tw x th, each the mean of the ones it covers */
static void scale(const u32 *src, u32 w, u32 h, u32 *dst, u32 tw, u32 th) {
  for (u32 y = 0; y < th; y++) {
    u32 y0 = y * h / th, y1 = (y + 1) * h / th;
    if (y1 <= y0)
      y1 = y0 + 1;
    for (u32 x = 0; x < tw; x++) {
      u32 x0 = x * w / tw, x1 = (x + 1) * w / tw;
      if (x1 <= x0)
        x1 = x0 + 1;
      u32 r = 0, g = 0, b = 0, c = 0;
      for (u32 yy = y0; yy < y1; yy++)
        for (u32 xx = x0; xx < x1; xx++) {
          u32 p = src[yy * w + xx];
          r += p & 255, g += p >> 8 & 255, b += p >> 16 & 255, c++;
        }
      dst[y * tw + x] = (r / c) | (g / c) << 8 | (b / c) << 16 | 0x80u << 24;
    }
  }
}

static void pic_do(struct picture *p) {
  p->state = PIC_BUSY;
  u32 n = 0;
  u8 *b = pic_bytes(p->ref, p->cover ? COVER_BYTES : 1u << 20, &n);
  if (!b) {
    p->state = PIC_FAILED;
    return;
  }
  if (p->cover) {
    if (n != COVER_BYTES) {
      free(b);
      p->state = PIC_FAILED;
      return;
    }
    p->data = b;
    p->state = PIC_READY;
    return;
  }
  struct mx_pic pic;
  int r = mx_pic_decode(b, n, &pic);
  free(b);
  if (r != MX_PIC_OK) {
    p->state = PIC_FAILED;
    return;
  }
  u32 *px = malloc((u32)p->w * p->h * 4);
  if (px)
    scale(pic.pixels, pic.width, pic.height, px, p->w, p->h);
  mx_pic_free(&pic);
  p->data = (u8 *)px;
  p->state = px ? PIC_READY : PIC_FAILED;
}

static struct picture *pic_wanted(void) {
  if (view_pic.state == PIC_WANT)
    return &view_pic;
  for (int e = 0; e < nentries; e++)
    for (int i = 0; i < PAGE_PICS; i++)
      if (pages[e][i].state == PIC_WANT)
        return &pages[e][i];
  for (int i = 0; i < nentries; i++)
    if (covers[i].state == PIC_WANT)
      return &covers[i];
  return 0;
}

/* ---- the index ---- */
static char index_buf[INDEX_MAX + 1];
static void load_index(void) {
  index_state = IX_LOADING;
  mx_snprintf(index_message, sizeof index_message, "Loading the index");
  static u8 sig[128];
  struct mem m = {(u8 *)index_buf, 0, INDEX_MAX}, s = {sig, 0, sizeof sig};
  int r = fetch("index.ini", mem_sink, &m);
  if (!r)
    r = fetch("index.sig", mem_sink, &s);
  if (r) {
    mx_snprintf(index_message, sizeof index_message, "No index: %s", fetch_error(r));
    index_state = IX_FAILED;
    return;
  }
  if (catalog_load(index_buf, m.n, sig, s.n) < 0) {
    mx_snprintf(index_message, sizeof index_message, "%s", catalog_error);
    index_state = IX_FAILED;
    return;
  }
  mx_printf("store: index of %s, %d programs\n", index_generated, nentries);
  scan_installed();
  for (int i = 0; i < nentries; i++) {
    memset(&covers[i], 0, sizeof covers[i]);
    covers[i].ref = &entries[i].cover;
    covers[i].cover = 1;
    covers[i].state = entries[i].cover.path ? PIC_WANT : PIC_NONE;
  }
  index_state = IX_READY;
}

/* ---- the thread ---- */
static int sema = -1;
static u8 stack[96 << 10] MX_ALIGNED(16) MX_IRQ_BSS;

static void worker(void *arg) {
  (void)arg;
  for (;;) {
    if (job.kind != J_NONE) {
      switch (job.kind) {
      case J_INDEX: load_index(); break;
      case J_INSTALL: install_entry(job.entry, job.dev); break;
      case J_REMOVE: remove_entry(job.entry); break;
      case J_SCAN: scan_installed(); break;
      }
      job.kind = J_NONE;
      continue;
    }
    struct picture *p = pic_wanted();
    if (p) {
      pic_do(p);
      continue;
    }
    mx_sema_wait(sema);
  }
}

void worker_start(void) {
  sema = mx_sema_create(0, 1000);
  mx_thread_start(worker, 0, stack, sizeof stack, mx_thread_prio(mx_thread_self()) + 1);
}
int worker_busy(void) { return job.kind != J_NONE; }
void worker_job(int kind, int entry, int dev, int replace) {
  if (job.kind != J_NONE)
    return;
  job.entry = entry, job.dev = dev, job.cancel = 0, job.replace = replace, job.phase = PH_DOWNLOAD;
  job.done = job.total = job.files = job.nfiles = job.rate = 0;
  job.message[0] = 0;
  job.kind = kind;
  mx_sema_signal(sema);
}
void worker_poke(void) {
  if (sema >= 0)
    mx_sema_signal(sema);
}
