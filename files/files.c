/* Files: an app of MXSHELL2 (its Apps section). The memory cards, the USB
   drive, the HDD and the host: browse them, select, copy, move, rename,
   delete, make folders. Penumbra as the shell: the shell's drawing, font,
   video mode and notifications (src/), its own main loop.

   Three panes: the devices, the folder, and (16:9) what is selected. What
   the OS cannot do yet is done another way, until mxio has the calls
   (asked for): a folder is an entry without a size or an extension that
   opens as one (no type in a directory entry), rename and move are a copy and a delete (no rename),
   and there is no free space to show. An ELF starts with
   mx_loader_launch, Files waiting for its end (a kernel of version 24:
   programs started by programs). */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <stdlib.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

MX_CORE_HEAP(512 << 10);

u32 pad_pressed, pad_held;
int screen;
int pad_on; /* pn.h */

/* ---- the devices: the kernel's list (devlist.c), then the PC (host:),
   looked at when chosen ---- */
enum { ST_UNKNOWN, ST_LOOKING, ST_READY, ST_NONE };
static struct {
  char root[16], name[24];
  int state, kind;
} devs[PDEV_MAX];
static int ndev;

/* ---- the folder ---- */
#define MAXE 1024
#define NAME 96
struct ent {
  char name[NAME];
  u32 size;
  u8 dir, mark;
};
static struct ent *ents;
static int nents, sel, scroll, cdev = -1, listed_ok;
static char cwd[256];
enum { P_DEVICES, P_LIST };
static int pane = P_DEVICES, dsel = 2;

/* ---- what is going on ---- */
enum { M_BROWSE, M_MENU, M_CONFIRM, M_KEYS, M_BUSY };
static int mode = M_BROWSE;

/* the clipboard: paths copied or cut */
#define MAXCLIP 64
static char clip[MAXCLIP][256];
static u8 clip_dir[MAXCLIP];
static int nclip, clip_move;

/* ---- small things ---- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_cmp(const char *a, const char *b) {
  for (; *a && lower(*a) == lower(*b); a++, b++)
    ;
  return lower((u8)*a) - lower((u8)*b);
}
static int ends_with(const char *s, const char *ext) {
  int a = (int)strlen(s), b = (int)strlen(ext);
  return a >= b && !name_cmp(s + a - b, ext);
}
static void join(char *out, int n, const char *dir, const char *name) {
  int l = (int)strlen(dir);
  mx_snprintf(out, n, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name);
}
static const char *base_name(const char *path) {
  const char *b = path;
  for (const char *s = path; *s; s++)
    if (*s == '/' || *s == ':')
      b = s + 1;
  return b;
}
static void fmt_size(char *o, int n, u64 b) {
  if (b < 1024)
    mx_snprintf(o, n, "%u B", (u32)b);
  else if (b < (1u << 20))
    mx_snprintf(o, n, "%u.%u KB", (u32)(b >> 10), (u32)((b & 1023) * 10 >> 10));
  else if (b < (1ull << 30))
    mx_snprintf(o, n, "%u.%u MB", (u32)(b >> 20), (u32)((b & 0xFFFFF) * 10 >> 20));
  else
    mx_snprintf(o, n, "%u.%u GB", (u32)(b >> 30), (u32)((b & 0x3FFFFFFF) * 10 >> 30));
}
static int is_dir(const char *path) {
  int fd = mx_fio_dopen(path);
  if (fd < 0)
    return 0;
  mx_fio_dclose(fd);
  return 1;
}
static int exists(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd >= 0) {
    mx_fio_close(fd);
    return 1;
  }
  return is_dir(path);
}

/* the entries of a folder: names, sizes and (size 0) whether a folder; the
   number, or -1 when it does not open. "." and ".." left out */
static int read_dir(const char *path, struct ent *e, int max) {
  int fd = mx_fio_dopen(path), n = 0;
  if (fd < 0)
    return -1;
  char name[NAME];
  u32 size;
  while (n < max && mx_fio_dread(fd, name, sizeof name, &size) > 0) {
    if (!strcmp(name, ".") || !strcmp(name, ".."))
      continue;
    strcpy(e[n].name, name);
    e[n].size = size, e[n].mark = 0, e[n].dir = 0;
    n++;
  }
  mx_fio_dclose(fd);
  /* the entry does not say what it is: what opens as a folder is one.
     Asked only of an entry without a size or without an extension (the
     host gives folders a size; a USB stick none) */
  for (int i = 0; i < n; i++)
    if (!e[i].size || !strchr(e[i].name + 1, '.')) {
      char p[256];
      join(p, sizeof p, path, e[i].name);
      e[i].dir = (u8)is_dir(p);
    }
  return n;
}

static int ent_cmp(const void *a, const void *b) {
  const struct ent *x = a, *y = b;
  if (x->dir != y->dir)
    return y->dir - x->dir;
  return name_cmp(x->name, y->name);
}

/* the folder again; at keep, the entry of that name chosen */
static void list(const char *keep) {
  nents = read_dir(cwd, ents, MAXE);
  listed_ok = nents >= 0;
  if (nents < 0)
    nents = 0;
  qsort(ents, (size_t)nents, sizeof *ents, ent_cmp);
  sel = scroll = 0;
  for (int i = 0; keep && i < nents; i++)
    if (!strcmp(ents[i].name, keep))
      sel = i;
}

static void open_dev(int d) {
  cdev = d;
  strcpy(cwd, devs[d].root);
  list(0);
  pane = P_LIST;
}

/* ---- the pad ---- */
static void read_pad(void) {
  pad_pressed = pad_held = 0;
  if (!pad_on)
    return;
  pad_pressed = mx_pad_pressed(0) | mx_pad_pressed(1);
  pad_held = mx_pad_buttons(0) | mx_pad_buttons(1);
  if (pad_pressed && toast_dismiss())
    pad_pressed = 0;
}

/* ---- a long job: frames go on drawing while it runs (20 a second, from
   a timer), circle stops it ---- */
static volatile u32 clock20 MX_IRQ_DATA;
MX_IRQ_FN static void clock_tick(void *arg) {
  (void)arg;
  clock20++;
}
static u32 drawn_at;
static int cancel;
static struct {
  const char *title;
  char item[NAME];
  u64 done, total;
  int files, nfiles, errors, skipped;
} job;
static u8 *buf;
#define BUF (64 << 10)
static void frame(void);
static void busy_frame(void) {
  if (clock20 == drawn_at)
    return;
  drawn_at = clock20;
  read_pad();
  if (pad_pressed & MX_PAD_CIRCLE)
    cancel = 1;
  frame();
}

/* how much there is: files and bytes under path */
static void count(const char *path, int dir, u32 size) {
  if (cancel)
    return;
  if (!dir) {
    job.nfiles++, job.total += size;
    return;
  }
  struct ent *e = malloc(MAXE * sizeof *e);
  if (!e)
    return;
  int n = read_dir(path, e, MAXE);
  busy_frame();
  for (int i = 0; i < n; i++) {
    char p[256];
    join(p, sizeof p, path, e[i].name);
    count(p, e[i].dir, e[i].size);
  }
  free(e);
}

static int copy_file(const char *src, const char *dst) {
  int in = mx_fio_open(src, MX_FIO_RDONLY);
  if (in < 0)
    return -1;
  int out = mx_fio_open(dst, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (out < 0) {
    mx_fio_close(in);
    return -1;
  }
  strncpy(job.item, base_name(src), NAME - 1);
  int r = 0;
  for (;;) {
    if (cancel) {
      r = -2;
      break;
    }
    int n = mx_fio_read(in, buf, BUF);
    if (n < 0) {
      r = -1;
      break;
    }
    if (n > 0 && mx_fio_write(out, buf, (u32)n)) {
      r = -1;
      break;
    }
    job.done += (u32)n;
    busy_frame();
    if (n < BUF)
      break;
  }
  mx_fio_close(in);
  mx_fio_close(out);
  if (r)
    mx_fio_remove(dst); /* half a file is no copy */
  job.files++;
  return r;
}

/* src (a folder: all in it) to dst; 0, or < 0 */
static int copy_tree(const char *src, const char *dst, int dir) {
  if (cancel)
    return -2;
  if (!dir) {
    int r = copy_file(src, dst);
    if (r == -1)
      job.errors++;
    return r;
  }
  if (!is_dir(dst) && mx_fio_mkdir(dst, 0777) < 0) {
    job.errors++;
    return -1;
  }
  struct ent *e = malloc(MAXE * sizeof *e);
  if (!e)
    return -1;
  int n = read_dir(src, e, MAXE), r = 0;
  for (int i = 0; i < n && !cancel; i++) {
    char s[256], d[256];
    join(s, sizeof s, src, e[i].name);
    join(d, sizeof d, dst, e[i].name);
    if (copy_tree(s, d, e[i].dir) < 0)
      r = -1;
  }
  free(e);
  return cancel ? -2 : r;
}

static int delete_tree(const char *path, int dir) {
  if (cancel)
    return -2;
  strncpy(job.item, base_name(path), NAME - 1);
  if (!dir) {
    int r = mx_fio_remove(path);
    job.files++;
    busy_frame();
    if (r < 0)
      job.errors++;
    return r < 0 ? -1 : 0;
  }
  struct ent *e = malloc(MAXE * sizeof *e);
  if (!e)
    return -1;
  int n = read_dir(path, e, MAXE), r = 0;
  for (int i = 0; i < n && !cancel; i++) {
    char p[256];
    join(p, sizeof p, path, e[i].name);
    if (delete_tree(p, e[i].dir) < 0)
      r = -1;
  }
  free(e);
  if (cancel)
    return -2;
  if (mx_fio_rmdir(path) < 0) {
    job.errors++;
    return -1;
  }
  return r;
}

static void job_begin(const char *title) {
  memset(&job, 0, sizeof job);
  job.title = title;
  cancel = 0;
  mode = M_BUSY;
  drawn_at = clock20 - 1;
}

static void job_end(const char *done_what, const char *keep) {
  mode = M_BROWSE;
  char m[64];
  if (cancel)
    toast(N_INFO, "Stopped", job.title, 0);
  else if (job.errors) {
    mx_snprintf(m, sizeof m, "%d could not be done", job.errors);
    toast(N_WARN, job.title, m, 1);
  } else if (job.skipped) {
    mx_snprintf(m, sizeof m, "%d already there, left as they were", job.skipped);
    toast(N_INFO, done_what, m, 0);
  } else
    toast(N_INFO, done_what, cwd, 0);
  list(keep);
}

/* what an action works on: the marked entries, else the chosen one */
static int targets(int *idx) {
  int n = 0;
  for (int i = 0; i < nents; i++)
    if (ents[i].mark)
      idx[n++] = i;
  if (!n && nents)
    idx[n++] = sel;
  return n;
}

static void do_copy(int move) {
  static int idx[MAXE];
  int n = targets(idx);
  nclip = 0, clip_move = move;
  for (int i = 0; i < n && nclip < MAXCLIP; i++, nclip++) {
    join(clip[nclip], sizeof clip[0], cwd, ents[idx[i]].name);
    clip_dir[nclip] = ents[idx[i]].dir;
  }
  for (int i = 0; i < nents; i++)
    ents[i].mark = 0;
  char m[48];
  if (nclip == 1)
    mx_snprintf(m, sizeof m, "%s", base_name(clip[0]));
  else
    mx_snprintf(m, sizeof m, "%d items", nclip);
  toast(N_INFO, move ? "Cut: paste it where it goes" : "Copied: paste it where it goes", m, 0);
}

static void do_paste(void) {
  job_begin(clip_move ? "Moving" : "Copying");
  for (int i = 0; i < nclip && !cancel; i++)
    count(clip[i], clip_dir[i], 0), busy_frame();
  /* sizes of single files: from the files themselves */
  for (int i = 0; i < nclip; i++)
    if (!clip_dir[i]) {
      int fd = mx_fio_open(clip[i], MX_FIO_RDONLY);
      if (fd >= 0) {
        job.total += (u32)mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
        mx_fio_close(fd);
      }
    }
  const char *keep = 0;
  for (int i = 0; i < nclip && !cancel; i++) {
    char dst[256];
    join(dst, sizeof dst, cwd, base_name(clip[i]));
    keep = base_name(clip[i]);
    int sl = (int)strlen(clip[i]);
    if (!strcmp(dst, clip[i]) || (clip_dir[i] && !strncmp(dst, clip[i], (size_t)sl) && dst[sl] == '/')) {
      job.errors++; /* onto itself, or into itself */
      continue;
    }
    if (exists(dst)) {
      job.skipped++;
      continue;
    }
    if (copy_tree(clip[i], dst, clip_dir[i]) == 0 && clip_move)
      delete_tree(clip[i], clip_dir[i]);
  }
  if (clip_move && !cancel)
    nclip = 0;
  job_end(clip_move ? "Moved" : "Copied", keep);
}

static void do_delete(void) {
  static int idx[MAXE];
  int n = targets(idx);
  job_begin("Deleting");
  for (int i = 0; i < n && !cancel; i++) {
    char p[256];
    join(p, sizeof p, cwd, ents[idx[i]].name);
    count(p, ents[idx[i]].dir, ents[idx[i]].size);
  }
  for (int i = 0; i < n && !cancel; i++) {
    char p[256];
    join(p, sizeof p, cwd, ents[idx[i]].name);
    delete_tree(p, ents[idx[i]].dir);
  }
  job_end("Deleted", 0);
}

/* rename and a new folder: the name from the keyboard */
enum { K_RENAME, K_MKDIR };
static int kfor;
static char kbuf[64];
static void name_done(void) {
  if (!kbuf[0] || !strcmp(kbuf, ".") || !strcmp(kbuf, ".."))
    return;
  char dst[256];
  join(dst, sizeof dst, cwd, kbuf);
  if (exists(dst)) {
    toast(N_WARN, "That name is taken", kbuf, 0);
    return;
  }
  if (kfor == K_MKDIR) {
    if (mx_fio_mkdir(dst, 0777) < 0)
      toast(N_WARN, "No new folder", "The device did not make it", 1);
    list(kbuf);
    return;
  }
  /* rename: a copy and a delete, until mxio has a rename */
  char src[256];
  join(src, sizeof src, cwd, ents[sel].name);
  int dir = ents[sel].dir;
  job_begin("Renaming");
  count(src, dir, ents[sel].size);
  if (copy_tree(src, dst, dir) == 0)
    delete_tree(src, dir);
  job_end("Renamed", kbuf);
}

/* an ELF: Files waits while it runs, its textures and libgl's memory
   given back; its end status as a notification when not 0 */
static void start_elf(const char *path) {
  static char args[256];
  mx_snprintf(args, sizeof args, "%s", path);
  u32 len = (u32)strlen(args) + 1;
  /* it starts in Files' working directory (kernel version 40): its own */
  char back[MX_FIO_CWD_MAX], dir[MX_FIO_CWD_MAX];
  u32 d = 0;
  for (u32 i = 0; args[i] && i < sizeof dir - 1; i++)
    if (args[i] == ':' || args[i] == '/')
      d = i + 1;
  memcpy(dir, args, d);
  dir[d] = 0;
  int moved = d && mx_fio_getcwd(back, sizeof back) >= 0 && mx_fio_chdir(dir) == 0;
  mxgl_suspend();
  int st = mx_loader_launch(1, args, len, 0);
  mxgl_resume();
  if (moved)
    mx_fio_chdir(back);
  for (int i = 0; mxgl_suspended() && i < 50; i++)
    mx_sleep_us(100000), mxgl_resume();
  video_apply();
  pad_on = mx_pad_init() >= 0;
  char m[48];
  if (st < 0)
    toast(N_WARN, "Could not start", base_name(path), 1);
  else if (st) {
    mx_snprintf(m, sizeof m, "Ended with status %d", st);
    toast(N_WARN, base_name(path), m, 1);
  }
  mx_printf("files: %s returned %d\n", path, st);
}

/* ---- the action menu ---- */
enum { A_COPY, A_MOVE, A_PASTE, A_RENAME, A_DELETE, A_MKDIR, A_MARKALL, NACTIONS };
static const char *const act_name[NACTIONS] = {"Copy", "Cut", "Paste here", "Rename", "Delete", "New folder",
                                               "Select all"};
static int acts[NACTIONS], nacts, msel;
static void menu_open(void) {
  nacts = 0;
  int marked = 0;
  for (int i = 0; i < nents; i++)
    marked += ents[i].mark;
  if (nents) {
    acts[nacts++] = A_COPY, acts[nacts++] = A_MOVE;
  }
  if (nclip)
    acts[nacts++] = A_PASTE;
  if (nents && marked <= 1)
    acts[nacts++] = A_RENAME;
  if (nents)
    acts[nacts++] = A_DELETE;
  acts[nacts++] = A_MKDIR;
  if (nents)
    acts[nacts++] = A_MARKALL;
  msel = 0, mode = M_MENU;
}

/* ---- the keyboard ---- */
static const char *const krows[2][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.!"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL(", "ZXCVBNM)+&"},
};
static const char *const kspecial[4] = {"Shift", "Space", "Delete", "Done"};
static int kx, ky, kshift;
static void keys_open(int what) {
  kfor = what;
  kbuf[0] = 0;
  if (what == K_RENAME)
    strncpy(kbuf, ents[sel].name, sizeof kbuf - 1), kbuf[sizeof kbuf - 1] = 0;
  kx = ky = kshift = 0;
  mode = M_KEYS;
}
static int name_max(void) { return devs[cdev].kind == MX_DEV_MC ? 31 : (int)sizeof kbuf - 1; } /* a card: 31 */
static void keys_input(void) {
  u32 p = pad_pressed;
  int len = (int)strlen(kbuf);
  if (p & MX_PAD_UP)
    ky = ky > 0 ? ky - 1 : 4;
  if (p & MX_PAD_DOWN)
    ky = ky < 4 ? ky + 1 : 0;
  int w = ky == 4 ? 4 : 10;
  if (ky == 4 && kx > 3)
    kx = 3;
  if (p & MX_PAD_LEFT)
    kx = kx > 0 ? kx - 1 : w - 1;
  if (p & MX_PAD_RIGHT)
    kx = kx < w - 1 ? kx + 1 : 0;
  if (p & MX_PAD_SELECT)
    kshift ^= 1;
  if (p & MX_PAD_SQUARE && len)
    kbuf[--len] = 0;
  if (p & MX_PAD_TRIANGLE && len < name_max())
    kbuf[len++] = ' ', kbuf[len] = 0;
  if (p & MX_PAD_CIRCLE)
    mode = M_BROWSE;
  int done = p & MX_PAD_START;
  if (p & MX_PAD_CROSS) {
    if (ky < 4) {
      if (len < name_max())
        kbuf[len++] = krows[kshift][ky][kx], kbuf[len] = 0;
    } else if (kx == 0)
      kshift ^= 1;
    else if (kx == 1 && len < name_max())
      kbuf[len++] = ' ', kbuf[len] = 0;
    else if (kx == 2 && len)
      kbuf[--len] = 0;
    else if (kx == 3)
      done = 1;
  }
  if (done) {
    mode = M_BROWSE;
    name_done();
  }
}

/* ---- drawing ---- */
#define TOPY 112.0f
#define BOTY 636.0f
#define ROW 36.0f
static float dev_w(void) { return SCR_W > 1100 ? 250.0f : 214.0f; }
static int wide(void) { return SCR_W > 1100; }
static float list_x0(void) { return MARGIN + dev_w() + 20; }
static float list_x1(void) { return wide() ? SCR_W - MARGIN - 320 : SCR_W - MARGIN; }
static int list_rows(void) { return (int)((BOTY - TOPY - 66) / ROW); }

/* lines 2 units wide: a pixel or more in every mode */
static void folder_icon(float x, float y, u32 c) {
  d_rect(x, y - 14, x + 8, y - 10, c, c);
  d_rect(x, y - 10, x + 20, y + 2, c, c);
}
static void file_icon(float x, float y, u32 c) {
  /* a page: upright, its lines of text */
  d_rect(x + 3, y - 16, x + 17, y + 2, c, c);
  u32 in = argb(C_GRAPHITE, 1);
  d_rect(x + 5, y - 14, x + 15, y, in, in);
  d_rect(x + 7, y - 10, x + 13, y - 8, c, c);
  d_rect(x + 7, y - 5, x + 13, y - 3, c, c);
}

static void draw_devices(void) {
  float x0 = MARGIN, x1 = MARGIN + dev_w();
  d_card(x0, TOPY, x1, BOTY, 18, C_GRAPHITE);
  d_label(x0 + 20, TOPY + 34, "Devices", argb(C_FAINT, 1));
  for (int d = 0; d < ndev; d++) {
    float y = TOPY + 84 + d * 66;
    int chosen = d == dsel, here = d == cdev;
    if (chosen) {
      float a = pane == P_DEVICES ? 0.09f : 0.04f;
      d_highlight(x0 + 8, y - 32, x1 - 8, y + 26, 10, a, a * 0.5f, pane == P_DEVICES ? 0.6f : 0);
    }
    d_text(FONT_BODY, x0 + 20, y - 4, devs[d].name, argb(here || chosen ? C_WHITE : C_DIM, 1));
    static const char *const st[] = {"Not looked at yet", "Looking", "", "Not found"};
    char s[32];
    mx_snprintf(s, sizeof s, "%s", devs[d].state == ST_READY ? devs[d].root : st[devs[d].state]);
    d_text(FONT_SMALL, x0 + 20, y + 18, s, argb(devs[d].state == ST_NONE ? C_WARN : C_FAINT, 1));
    if (here)
      d_rect(x0 + 8, y - 22, x0 + 11, y + 16, argb(C_LAMP, 1), argb(C_LAMP, 1));
  }
}

static void draw_list(void) {
  float x0 = list_x0(), x1 = list_x1();
  d_card(x0, TOPY, x1, BOTY, 18, C_GRAPHITE);
  if (cdev < 0) {
    d_text(FONT_BODY, x0 + 24, TOPY + 60, "Choose a device", argb(C_DIM, 1));
    d_text(FONT_SMALL, x0 + 24, TOPY + 90, "Up and down, then " SYM_CROSS, argb(C_FAINT, 1));
    return;
  }
  d_label(x0 + 24, TOPY + 34, "Name", argb(C_FAINT, 1));
  d_label(x1 - 24 - d_label_w("Size"), TOPY + 34, "Size", argb(C_FAINT, 1));
  if (!listed_ok) {
    d_text(FONT_BODY, x0 + 24, TOPY + 90, "This folder does not open", argb(C_WARN, 1));
    return;
  }
  if (!nents) {
    d_text(FONT_BODY, x0 + 24, TOPY + 90, "Empty", argb(C_DIM, 1));
    return;
  }
  int rows = list_rows();
  if (sel < scroll)
    scroll = sel;
  if (sel >= scroll + rows)
    scroll = sel - rows + 1;
  for (int r = 0; r < rows && scroll + r < nents; r++) {
    int i = scroll + r;
    const struct ent *e = &ents[i];
    float y = TOPY + 76 + r * ROW;
    if (i == sel) {
      float a = pane == P_LIST ? 0.09f : 0.04f;
      d_highlight(x0 + 8, y - 25, x1 - 8, y + 11, 8, a, a * 0.5f, pane == P_LIST ? 0.6f : 0);
    }
    u32 ic = e->mark ? C_LAMP : i == sel ? C_DIM : C_FAINT;
    if (e->mark) /* selected: the lamp's mark before it */
      d_rect(x0 + 12, y - 12, x0 + 18, y - 2, argb(C_LAMP, 1), argb(C_LAMP, 1));
    if (e->dir)
      folder_icon(x0 + 26, y - 1, argb(ic, 1));
    else
      file_icon(x0 + 24, y, argb(ic, 1));
    char sz[16] = "";
    if (!e->dir)
      fmt_size(sz, sizeof sz, e->size);
    float sw = d_text_w(FONT_SMALL, sz);
    d_text_fit(FONT_BODY, FONT_SMALL, x0 + 56, y, x1 - x0 - 100 - sw, e->name,
               argb(e->mark ? C_LAMP : i == sel ? C_WHITE : C_DIM, 1));
    d_text(FONT_SMALL, x1 - 24 - sw, y, sz, argb(C_FAINT, 1));
  }
  /* the count, and where in the list */
  char m[64];
  int marked = 0;
  for (int i = 0; i < nents; i++)
    marked += ents[i].mark;
  if (marked)
    mx_snprintf(m, sizeof m, "%d items" DOT "%d selected", nents, marked);
  else
    mx_snprintf(m, sizeof m, "%d items", nents);
  d_text(FONT_SMALL, x0 + 24, BOTY - 20, m, argb(C_FAINT, 1));
  if (nents > rows) {
    float h = BOTY - TOPY - 120, t = TOPY + 52 + h * scroll / nents, b = t + h * rows / nents;
    d_rect(x1 - 6, t, x1 - 4, b, argb(C_WHITE, 0.25f), argb(C_WHITE, 0.25f));
  }
}

/* the chosen entry: its name, kind, size and (text) its first lines */
static char peek_path[256], peek_text[640];
static void draw_preview(void) {
  if (!wide())
    return;
  float x0 = SCR_W - MARGIN - 300, x1 = SCR_W - MARGIN;
  d_card(x0, TOPY, x1, BOTY, 18, C_GRAPHITE);
  d_label(x0 + 20, TOPY + 34, "Selected", argb(C_FAINT, 1));
  if (cdev < 0 || pane != P_LIST || !nents) {
    if (cdev >= 0 || pane == P_DEVICES) {
      const char *s = pane == P_DEVICES ? devs[dsel].root : cwd;
      d_text_fit(FONT_BODY, FONT_SMALL, x0 + 20, TOPY + 80, x1 - x0 - 40, s, argb(C_WHITE, 1));
      d_text(FONT_SMALL, x0 + 20, TOPY + 108, "Free space: when mxio can say", argb(C_FAINT, 1));
    }
    return;
  }
  const struct ent *e = &ents[sel];
  char path[256];
  join(path, sizeof path, cwd, e->name);
  d_text_fit(FONT_TITLE, FONT_SMALL, x0 + 20, TOPY + 84, x1 - x0 - 40, e->name, argb(C_WHITE, 1));
  const char *kind = e->dir ? "Folder"
                     : ends_with(e->name, ".elf") ? "Program (ELF)"
                     : ends_with(e->name, ".png") || ends_with(e->name, ".jpg") || ends_with(e->name, ".tga") ? "Picture"
                     : ends_with(e->name, ".mpg") || ends_with(e->name, ".m2v") || ends_with(e->name, ".pss") ? "Video"
                     : ends_with(e->name, ".mp3") || ends_with(e->name, ".wav") || ends_with(e->name, ".ogg") ? "Sound"
                     : ends_with(e->name, ".irx") ? "IOP module"
                                                  : "File";
  char sz[24] = "";
  if (!e->dir)
    fmt_size(sz, sizeof sz, e->size);
  float y = TOPY + 124;
  d_text(FONT_SMALL, x0 + 20, y, "Kind", argb(C_FAINT, 1));
  d_text(FONT_SMALL, x0 + 110, y, kind, argb(C_WHITE, 1));
  if (!e->dir) {
    y += 28;
    d_text(FONT_SMALL, x0 + 20, y, "Size", argb(C_FAINT, 1));
    d_text(FONT_SMALL, x0 + 110, y, sz, argb(C_WHITE, 1));
  }
  y += 28;
  d_text(FONT_SMALL, x0 + 20, y, "Where", argb(C_FAINT, 1));
  d_text_fit(FONT_SMALL, FONT_LABEL, x0 + 110, y, x1 - x0 - 130, cwd, argb(C_WHITE, 1));
  /* a text file: its first lines, read once per entry */
  int text = !e->dir && (ends_with(e->name, ".txt") || ends_with(e->name, ".ini") || ends_with(e->name, ".cfg") ||
                         ends_with(e->name, ".md") || ends_with(e->name, ".log") || ends_with(e->name, ".dat"));
  if (!text)
    return;
  if (strcmp(path, peek_path)) {
    strcpy(peek_path, path);
    peek_text[0] = 0;
    int fd = mx_fio_open(path, MX_FIO_RDONLY);
    if (fd >= 0) {
      int n = mx_fio_read(fd, peek_text, sizeof peek_text - 1);
      mx_fio_close(fd);
      peek_text[n > 0 ? n : 0] = 0;
      for (char *s = peek_text; *s; s++)
        if ((u8)*s < 32 && *s != '\n')
          *s = ' ';
        else if ((u8)*s > 126)
          *s = '?';
    }
  }
  y += 44;
  d_rect(x0 + 20, y - 22, x1 - 20, y - 21, argb(C_WHITE, 0.08f), argb(C_WHITE, 0.08f));
  const char *s = peek_text;
  for (int l = 0; l < 12 && *s && y < BOTY - 20; l++, y += 26) {
    char line[64];
    int n = 0;
    while (*s && *s != '\n' && n < 63)
      line[n++] = *s++;
    line[n] = 0;
    while (*s && *s != '\n')
      s++;
    if (*s == '\n')
      s++;
    d_text_fit(FONT_SMALL, FONT_LABEL, x0 + 20, y, x1 - x0 - 40, line, argb(C_DIM, 1));
  }
}

static void scrim(void) { d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.62f), argb(C_INK, 0.62f)); }
static void panel(float x, float y, float w, float h) {
  d_shadow(x + w / 2, y + h / 2, w * 0.7f, h * 0.9f, 0.5f);
  d_card_lit(x, y, x + w, y + h, 20, 0x17191E, 1, 0.55f);
}

static void draw_menu(void) {
  scrim();
  float w = 360, h = 40 + nacts * 50 + 20, x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
  panel(x, y, w, h);
  for (int i = 0; i < nacts; i++) {
    float yy = y + 60 + i * 50;
    if (i == msel) {
      d_highlight(x + 12, yy - 32, x + w - 12, yy + 14, 10, 0.09f, 0.04f, 0.6f);
    }
    char l[48];
    if (acts[i] == A_PASTE)
      mx_snprintf(l, sizeof l, "%s (%d)", act_name[A_PASTE], nclip);
    else
      mx_snprintf(l, sizeof l, "%s", act_name[acts[i]]);
    d_text(FONT_BODY, x + 32, yy, l, argb(acts[i] == A_DELETE ? C_WARN : i == msel ? C_WHITE : C_DIM, 1));
  }
}

static int confirm_n;
static void draw_confirm(void) {
  scrim();
  float w = 540, h = 200, x = (SCR_W - w) / 2, y = 250;
  panel(x, y, w, h);
  char m[64];
  if (confirm_n == 1)
    mx_snprintf(m, sizeof m, "Delete %s?", ents[sel].name);
  else
    mx_snprintf(m, sizeof m, "Delete %d items?", confirm_n);
  d_text_fit(FONT_TITLE, FONT_BODY, x + 32, y + 62, w - 64, m, argb(C_WHITE, 1));
  d_text(FONT_BODY, x + 32, y + 96, "Folders go with all that is in them.", argb(C_DIM, 1));
  aa_under(0x17191E);
  float bx = x + 32;
  bx += d_button(bx, y + 126, SYM_CROSS, "Delete", 1) + 12;
  d_button(bx, y + 126, SYM_CIRCLE, "Keep", 0);
  aa_under_room();
}

static void draw_keys(void) {
  scrim();
  float kw = 58, kh = 50, g = 8, w = 10 * kw + 9 * g + 64, h = 420, x = (SCR_W - w) / 2, y = 150;
  if (w > SCR_W - 40) {
    kw = (SCR_W - 40 - 64 - 9 * g) / 10, w = 10 * kw + 9 * g + 64, x = (SCR_W - w) / 2;
  }
  panel(x, y, w, h);
  d_label(x + 32, y + 40, kfor == K_MKDIR ? "New folder" : "Rename", argb(C_LAMP, 1));
  /* the name, with a caret */
  d_highlight(x + 32, y + 56, x + w - 32, y + 104, 10, 0.06f, 0.06f, 0);
  float tx = d_text_fit(FONT_BODY, FONT_SMALL, x + 48, y + 88, w - 120, kbuf, argb(C_WHITE, 1));
  if ((ticks / 20) & 1)
    d_rect(tx + 2, y + 66, tx + 4, y + 94, argb(C_LAMP, 1), argb(C_LAMP, 1));
  float gy = y + 128;
  aa_under(0x17191E);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 10; c++) {
      float kx0 = x + 32 + c * (kw + g), ky0 = gy + r * (kh + g);
      int on = ky == r && kx == c;
      if (on)
        d_pill(kx0, ky0, kx0 + kw, ky0 + kh, C_WHITE);
      else
        d_highlight(kx0, ky0, kx0 + kw, ky0 + kh, 8, 0.05f, 0.03f, 0);
      char ch[2] = {krows[kshift][r][c], 0};
      float cw = d_text_w(FONT_BODY, ch);
      d_text(FONT_BODY, kx0 + (kw - cw) / 2, ky0 + kh / 2 + text_mid(FONT_BODY), ch, argb(on ? C_INK : C_WHITE, 1));
    }
  float sw = (10 * kw + 9 * g - 3 * g) / 4, sy = gy + 4 * (kh + g);
  for (int i = 0; i < 4; i++) {
    float kx0 = x + 32 + i * (sw + g);
    int on = ky == 4 && kx == i;
    if (on)
      d_pill(kx0, sy, kx0 + sw, sy + kh, C_WHITE);
    else
      d_highlight(kx0, sy, kx0 + sw, sy + kh, 8, 0.05f, 0.03f, 0);
    const char *l = i == 0 && kshift ? "shift" : kspecial[i];
    float lw = d_text_w(FONT_SMALL, l);
    d_text(FONT_SMALL, kx0 + (sw - lw) / 2, sy + kh / 2 + text_mid(FONT_SMALL), l, argb(on ? C_INK : C_DIM, 1));
  }
  aa_under_room();
}

static void draw_busy(void) {
  scrim();
  float w = 600, h = 220, x = (SCR_W - w) / 2, y = 240;
  panel(x, y, w, h);
  d_label(x + 32, y + 42, job.title, argb(C_LAMP, 1));
  d_text_fit(FONT_TITLE, FONT_BODY, x + 32, y + 92, w - 64, job.item[0] ? job.item : "Looking", argb(C_WHITE, 1));
  float k = job.total ? (float)job.done / (float)job.total : job.nfiles ? (float)job.files / (float)job.nfiles : 0;
  k = clampf(k, 0, 1);
  float bx0 = x + 32, bx1 = x + w - 32, by = y + 126;
  d_bar(bx0, by, bx1, by + 4, k, C_LAMP, 1);
  char a[24], b[24], m[80];
  fmt_size(a, sizeof a, job.done), fmt_size(b, sizeof b, job.total);
  mx_snprintf(m, sizeof m, "%d of %d files" DOT "%s of %s", job.files, job.nfiles, a, b);
  d_text(FONT_SMALL, x + 32, y + 162, m, argb(C_DIM, 1));
  d_text(FONT_SMALL, x + 32, y + 192, cancel ? "Stopping" : SYM_CIRCLE " Stop", argb(C_FAINT, 1));
}

static void frame(void) {
  glClearColor((C_INK >> 16 & 255) / 255.0f, (C_INK >> 8 & 255) / 255.0f, (C_INK & 255) / 255.0f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  room_begin(-1);
  room_light(MARGIN + 160, 140, 520, 320, 0.05f);
  d_begin_2d();
  room_draw();
  /* the title and where */
  float x = d_text(FONT_TITLE, MARGIN - 2, 80, "Files", argb(C_WHITE, 1));
  if (cdev >= 0)
    d_text_fit(FONT_BODY, FONT_SMALL, x + 24, 80, SCR_W - MARGIN - x - 24, cwd, argb(C_DIM, 1));
  draw_devices();
  draw_list();
  draw_preview();
  static const char *const h_dev[] = {SYM_CROSS, "Open", SYM_CIRCLE, "Close Files", 0};
  static const char *const h_list[] = {SYM_CROSS, "Open", SYM_SQUARE, "Select", SYM_TRIANGLE, "Actions",
                                       SYM_CIRCLE, "Up", 0};
  static const char *const h_menu[] = {SYM_CROSS, "Choose", SYM_CIRCLE, "Back", 0};
  static const char *const h_keys[] = {SYM_CROSS, "Type", SYM_SQUARE, "Delete", SYM_TRIANGLE, "Space",
                                       "START", "Done", SYM_CIRCLE, "Cancel", 0};
  switch (mode) {
  case M_MENU: draw_menu(), hints_draw(h_menu); break;
  case M_CONFIRM: draw_confirm(); break;
  case M_KEYS: draw_keys(), hints_draw(h_keys); break;
  case M_BUSY: draw_busy(); break;
  default: hints_draw(pane == P_DEVICES ? h_dev : h_list); break;
  }
  toast_draw();
  mxgl_end_frame();
  mxgl_frame_done();
}

/* ---- the devices ---- */
/* the list again when the kernel's changed: the device open kept, or, if it
   went, the list of devices in front */
static u32 devs_gen = ~0u;
static void devs_refresh(void) {
  if (pdevs_gen() == devs_gen && ndev)
    return;
  devs_gen = pdevs_gen();
  char was[16] = "";
  int host_state = ST_UNKNOWN;
  if (cdev >= 0)
    strcpy(was, devs[cdev].root);
  for (int d = 0; d < ndev; d++)
    if (devs[d].kind == MX_DEV_HOST)
      host_state = devs[d].state;
  struct pdev p[PDEV_MAX];
  int n = pdevs_read(p, PDEV_MAX - 1, 0);
  ndev = 0;
  for (int i = 0; i < n; i++, ndev++) {
    mx_snprintf(devs[ndev].root, sizeof devs[ndev].root, "%s/", p[i].root);
    mx_snprintf(devs[ndev].name, sizeof devs[ndev].name, "%s", pdev_name(&p[i]));
    devs[ndev].state = ST_READY, devs[ndev].kind = p[i].kind;
  }
  strcpy(devs[ndev].root, "host:/"), strcpy(devs[ndev].name, "PC (host)");
  devs[ndev].state = host_state, devs[ndev++].kind = MX_DEV_HOST;
  cdev = -1;
  for (int d = 0; d < ndev; d++)
    if (!strcmp(devs[d].root, was))
      cdev = d;
  if (was[0] && cdev < 0) {
    toast(N_DEVICE, "Removed", was, 0);
    pane = P_DEVICES, nents = 0, cwd[0] = 0;
  }
  if (dsel >= ndev)
    dsel = ndev - 1;
}

/* the PC: looked at when chosen (ps2client may not be listening) */
static void look_step(void) {
  for (int d = 0; d < ndev; d++)
    if (devs[d].state == ST_LOOKING) {
      int fd = mx_fio_dopen(devs[d].root);
      if (fd >= 0)
        mx_fio_dclose(fd);
      devs[d].state = fd >= 0 ? ST_READY : ST_NONE;
    }
}

static void browse_input(void) {
  u32 p = pad_pressed;
  if (pane == P_DEVICES) {
    if (p & MX_PAD_UP && dsel > 0)
      dsel--;
    if (p & MX_PAD_DOWN && dsel + 1 < ndev)
      dsel++;
    if (p & (MX_PAD_CROSS | MX_PAD_RIGHT)) {
      if (devs[dsel].state == ST_READY)
        open_dev(dsel);
      else if (devs[dsel].state != ST_LOOKING) {
        devs[dsel].state = ST_LOOKING;
        job_begin("Looking");
        strcpy(job.item, devs[dsel].name);
        while (devs[dsel].state == ST_LOOKING) {
          look_step();
          busy_frame();
          if (devs[dsel].state == ST_LOOKING)
            mx_sleep_us(250000);
        }
        mode = M_BROWSE;
        if (devs[dsel].state == ST_READY)
          open_dev(dsel);
        else
          toast(N_WARN, devs[dsel].name, "Not found", 0);
      }
    }
    if (p & MX_PAD_CIRCLE)
      mx_exit(0);
    return;
  }
  int rows = list_rows();
  if (p & MX_PAD_UP && sel > 0)
    sel--;
  if (p & MX_PAD_DOWN && sel + 1 < nents)
    sel++;
  if (p & MX_PAD_L1)
    sel = sel > rows ? sel - rows : 0;
  if (p & MX_PAD_R1)
    sel = sel + rows < nents ? sel + rows : nents ? nents - 1 : 0;
  if (p & MX_PAD_LEFT)
    pane = P_DEVICES, dsel = cdev;
  if (p & MX_PAD_SQUARE && nents) {
    ents[sel].mark ^= 1;
    if (sel + 1 < nents)
      sel++;
  }
  if (p & MX_PAD_TRIANGLE)
    menu_open();
  if (p & MX_PAD_CROSS && nents && ents[sel].dir) {
    char p2[256];
    join(p2, sizeof p2, cwd, ents[sel].name);
    if (strlen(p2) < sizeof cwd - 2) {
      strcpy(cwd, p2);
      list(0);
    }
  } else if (p & MX_PAD_CROSS && nents && ends_with(ents[sel].name, ".elf")) {
    char p2[256];
    join(p2, sizeof p2, cwd, ents[sel].name);
    start_elf(p2);
  }
  if (p & MX_PAD_CIRCLE) {
    /* up a folder, the one we were in chosen; at the top: the devices */
    if (!strcmp(cwd, devs[cdev].root)) {
      pane = P_DEVICES, dsel = cdev;
      return;
    }
    char was[NAME];
    int l = (int)strlen(cwd);
    while (l > 0 && cwd[l - 1] == '/')
      cwd[--l] = 0;
    strncpy(was, base_name(cwd), NAME - 1), was[NAME - 1] = 0;
    char *s = cwd + strlen(cwd);
    while (s > cwd && s[-1] != '/')
      s--;
    *s = 0;
    if (!strchr(cwd, '/')) /* "usb0:" */
      strcpy(cwd, devs[cdev].root);
    list(was);
  }
}

static void menu_input(void) {
  u32 p = pad_pressed;
  if (p & MX_PAD_UP && msel > 0)
    msel--;
  if (p & MX_PAD_DOWN && msel + 1 < nacts)
    msel++;
  if (p & (MX_PAD_CIRCLE | MX_PAD_TRIANGLE)) {
    mode = M_BROWSE;
    return;
  }
  if (!(p & MX_PAD_CROSS))
    return;
  mode = M_BROWSE;
  static int idx[MAXE];
  switch (acts[msel]) {
  case A_COPY: do_copy(0); break;
  case A_MOVE: do_copy(1); break;
  case A_PASTE: do_paste(); break;
  case A_RENAME: keys_open(K_RENAME); break;
  case A_DELETE:
    confirm_n = targets(idx);
    mode = M_CONFIRM;
    break;
  case A_MKDIR: keys_open(K_MKDIR); break;
  case A_MARKALL:
    for (int i = 0; i < nents; i++)
      ents[i].mark = 1;
    break;
  }
}

/* tests: every action once, in the folder open (ops) */
static int find(const char *name) {
  for (int i = 0; i < nents; i++)
    if (!strcmp(ents[i].name, name))
      return i;
  return -1;
}
static int run_ops(void) {
  int bad = 0;
  char top[256];
  strcpy(top, cwd);
  /* a new folder */
  kfor = K_MKDIR, strcpy(kbuf, "filestest"), name_done();
  bad += find("filestest") < 0;
  /* a folder copied into it: all that is in it */
  sel = find("quake1");
  bad += sel < 0;
  do_copy(0);
  join(cwd, sizeof cwd, top, "filestest"), list(0);
  do_paste();
  bad += find("quake1") < 0;
  join(cwd, sizeof cwd, top, "filestest/quake1"), list(0);
  bad += nents != 3;
  mx_printf("files: copied folder: %d entries\n", nents);
  /* renamed */
  sel = find("game.ini");
  kfor = K_RENAME, strcpy(kbuf, "renamed.ini"), name_done();
  bad += find("renamed.ini") < 0 || find("game.ini") >= 0;
  /* two files cut and pasted a folder up */
  ents[find("cover.t8")].mark = 1, ents[find("renamed.ini")].mark = 1;
  do_copy(1);
  join(cwd, sizeof cwd, top, "filestest"), list(0);
  do_paste();
  bad += find("cover.t8") < 0 || find("renamed.ini") < 0;
  join(cwd, sizeof cwd, top, "filestest/quake1"), list(0);
  bad += nents != 1;
  mx_printf("files: after the move: %d left\n", nents);
  /* all of it deleted */
  strcpy(cwd, top), list(0);
  sel = find("filestest");
  do_delete();
  bad += find("filestest") >= 0;
  mx_printf("files: ops %s (%d)\n", bad ? "FAILED" : "passed", bad);
  return bad;
}

#ifdef MX_BIOS_CALLS
/* the standalone build on PCSX2: arguments from host:files.txt */
static char *file_args[8], file_buf[160];
static int args_from_host(char ***argv) {
  int fd = mx_fio_open("host:files.txt", MX_FIO_RDONLY), n = 0, argc = 1;
  if (fd < 0)
    return 1;
  n = mx_fio_read(fd, file_buf, sizeof file_buf - 1);
  mx_fio_close(fd);
  file_buf[n > 0 ? n : 0] = 0;
  file_args[0] = "files";
  for (char *s = file_buf; *s && argc < 8;) {
    while (*s == ' ' || *s == '\n')
      *s++ = 0;
    if (!*s)
      break;
    file_args[argc++] = s;
    while (*s && *s != ' ' && *s != '\n')
      s++;
  }
  *argv = file_args;
  return argc;
}
#endif

/* arguments (tests): dev=<n> open device n at the start, path=<dir> that
   folder, sel=<n> its entry n, mark=<n> entries 0..n-1 selected,
   menu (the actions), keys (the keyboard), ops (every action once),
   run=<elf> start it, shot=<frames> the picture to
   host:files.tga, mode=<video mode> */
int main(int argc, char **argv) {
  cfg_load();
#ifdef MX_BIOS_CALLS
  if (argc <= 1)
    argc = args_from_host(&argv);
#endif
  int shot = 0, odev = -1, osel = 0, omark = 0, omenu = 0, okeys = 0, oops = 0;
  const char *orun = 0, *odev_s = 0;
  const char *opath = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strncmp(a, "shot=", 5))
      shot = atoi(a + 5);
    else if (!strncmp(a, "dev=", 4))
      odev_s = a + 4;
    else if (!strncmp(a, "path=", 5))
      opath = a + 5;
    else if (!strncmp(a, "sel=", 4))
      osel = atoi(a + 4);
    else if (!strncmp(a, "mark=", 5))
      omark = atoi(a + 5);
    else if (!strcmp(a, "menu"))
      omenu = 1;
    else if (!strcmp(a, "keys"))
      okeys = 1;
    else if (!strcmp(a, "ops"))
      oops = 1;
    else if (!strncmp(a, "run=", 4))
      orun = a + 4;
    else if (!strncmp(a, "mode=", 5))
      cfg_arg("video.mode", a + 5);
  }
  ents = malloc(MAXE * sizeof *ents);
  buf = malloc(BUF);
  if (!ents || !buf)
    return 1;
  mxgl_init();
  pad_on = mx_pad_init() >= 0;
  video_apply();
  mx_timer_add(50000, 50000, clock_tick, 0);
  devs_refresh();
  /* tests: dev=<n> the n-th device, or dev=<name> the one whose name starts so (dev=host) */
  if (odev_s)
    for (int d = 0; d < ndev; d++)
      if ((odev_s[0] >= '0' && odev_s[0] <= '9') ? d == atoi(odev_s) : !strncmp(devs[d].root, odev_s, strlen(odev_s)))
        odev = d;

  for (;;) {
    ticks++;
    read_pad();
    if (ticks % 15 == 0)
      devs_refresh(), look_step();
    if (odev >= 0 && devs[odev].state != ST_LOOKING) {
      if (devs[odev].state == ST_UNKNOWN)
        devs[odev].state = ST_LOOKING;
      else {
        if (devs[odev].state == ST_READY) {
          open_dev(odev);
          if (opath) {
            join(cwd, sizeof cwd, devs[odev].root, opath);
            list(0);
          }
          sel = osel < nents ? osel : 0;
          for (int i = 0; i < omark && i < nents; i++)
            ents[i].mark = 1;
          if (omenu)
            menu_open();
          if (okeys)
            keys_open(K_MKDIR);
          if (orun) {
            char p2[256];
            join(p2, sizeof p2, cwd, orun);
            start_elf(p2);
          }
          if (oops) {
            int r = run_ops();
            MX_TEST_END(r ? 1 : 0);
            return r;
          }
        }
        odev = -1;
      }
    }
    toast_step();
    switch (mode) {
    case M_BROWSE: browse_input(); break;
    case M_MENU: menu_input(); break;
    case M_CONFIRM:
      if (pad_pressed & MX_PAD_CROSS)
        do_delete();
      else if (pad_pressed & MX_PAD_CIRCLE)
        mode = M_BROWSE;
      break;
    case M_KEYS: keys_input(); break;
    }
    frame();
    if (shot && ticks == (u32)shot) {
      int r = mxgl_screenshot("host:files.tga");
      mx_printf("files: shot %d\n", r);
      MX_TEST_END(r ? 1 : 0);
      return 0;
    }
  }
}
