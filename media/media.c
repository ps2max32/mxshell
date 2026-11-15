/* Media: an app of MXSHELL2 (its Media section) for photos, videos and
   music. Penumbra as the shell: the shell's drawing, font, video mode and
   notifications (src/), its own main loop.

   Three kinds, L1 and R1 between them: Photos (JPEG, PNG: <mx/pic.h>),
   Videos (MPEG-1 and -2, alone or in a program stream with LPCM or MP2
   sound: <mx/movie.h>, on the IPU), Music (WAV, MP2: <mx/mp2.h>). Each
   browses the devices' folders, showing the folders and the files of its
   kind; the chosen file is opened full screen:
     a photo   fitted to the screen, left and right to the folder's others
     a video   with its sound, a bar of where it is at a button; cross
               pauses, left and right jump 10 s
     music     plays in a thread of its own (it goes on while you browse and
               look at photos, the folder's next track after it), its
               screen "Now playing" from any of them with START.
   Pictures and video frames are libgl textures (CT16): a photo is scaled
   down to fit 1024 x 1024 first, a video's picture is copied into its
   texture each frame (mx_movie_raster).

   What the OS cannot give yet: music beside a game or the shell (it ends
   with Media); MP3 (layer III) is listed but not played. */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/ipu.h>
#include <mx/loader.h>
#include <mx/movie.h>
#include <mx/mp2.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/pic.h>
#include <mx/sif.h>
#include <mx/snd.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <stdlib.h>
#include <string.h>

#include "mxgl.h"
#include "pn.h"

MX_CORE_HEAP(512 << 10);

u32 pad_pressed, pad_held;
int screen;
static int pad_on;

/* ---- the kinds ---- */
enum { K_PHOTO, K_VIDEO, K_MUSIC, NKINDS };
static const char *const kind_name[NKINDS] = {"Photos", "Videos", "Music"};
static const char *const kind_key[NKINDS] = {"photo", "video", "music"};
static const char *const kind_exts[NKINDS][6] = {
    {".jpg", ".jpeg", ".png", 0},
    {".mpg", ".mpeg", ".m2v", ".m1v", ".vob", 0},
    {".mp2", ".mpa", ".wav", ".mp3", 0},
};
static int kind = K_PHOTO;

/* ---- the devices (as Files): the kernel's list, then the PC (host:) ---- */
enum { ST_UNKNOWN, ST_LOOKING, ST_READY, ST_NONE };
static struct {
  char root[16], name[24];
  int state, kind;
} devs[PDEV_MAX];
static int ndev;

/* ---- the folder: its folders and the files of the kind ---- */
#define MAXE 512
#define NAME 96
struct ent {
  char name[NAME];
  u32 size;
  u8 dir;
};
static struct ent *ents;
static int nents, sel, scroll, cdev = -1, listed_ok;
static char cwd[256];
enum { P_DEVICES, P_LIST };
static int pane = P_DEVICES, dsel = 2;

/* what is on the screen */
enum { V_BROWSE, V_PHOTO, V_VIDEO, V_MUSIC };
static int view = V_BROWSE;

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
static int of_kind(const char *name, int k) {
  for (int i = 0; kind_exts[k][i]; i++)
    if (ends_with(name, kind_exts[k][i]))
      return 1;
  return 0;
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
/* the name without its extension, for titles */
static void title_of(char *out, int n, const char *path) {
  mx_snprintf(out, n, "%s", base_name(path));
  char *dot = strrchr(out, '.');
  if (dot && dot != out)
    *dot = 0;
}
static void fmt_size(char *o, int n, u32 b) {
  if (b < 1024)
    mx_snprintf(o, n, "%u B", b);
  else if (b < (1u << 20))
    mx_snprintf(o, n, "%u.%u KB", b >> 10, (b & 1023) * 10 >> 10);
  else
    mx_snprintf(o, n, "%u.%u MB", b >> 20, (b & 0xFFFFF) * 10 >> 20);
}
static void fmt_time(char *o, int n, u32 ms) {
  u32 s = ms / 1000;
  if (s >= 3600)
    mx_snprintf(o, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
  else
    mx_snprintf(o, n, "%u:%02u", s / 60, s % 60);
}
static int is_dir(const char *path) {
  int fd = mx_fio_dopen(path);
  if (fd < 0)
    return 0;
  mx_fio_dclose(fd);
  return 1;
}

/* the entries of the folder: folders, and the files of the kind */
static int read_dir(const char *path, struct ent *e, int max, int k) {
  int fd = mx_fio_dopen(path), n = 0;
  if (fd < 0)
    return -1;
  char name[NAME];
  u32 size;
  while (n < max && mx_fio_dread(fd, name, sizeof name, &size) > 0) {
    if (name[0] == '.')
      continue;
    strcpy(e[n].name, name);
    e[n].size = size, e[n].dir = 0;
    n++;
  }
  mx_fio_dclose(fd);
  /* no type in a directory entry (mxio): what opens as a folder is one;
     asked only of an entry without a size or an extension */
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (!e[i].size || !strchr(e[i].name + 1, '.')) {
      char p[256];
      join(p, sizeof p, path, e[i].name);
      e[i].dir = (u8)is_dir(p);
    }
    if (e[i].dir || of_kind(e[i].name, k))
      e[m++] = e[i];
  }
  return m;
}

static int ent_cmp(const void *a, const void *b) {
  const struct ent *x = a, *y = b;
  if (x->dir != y->dir)
    return y->dir - x->dir;
  return name_cmp(x->name, y->name);
}

static void preview_drop(void);
static void list(const char *keep) {
  nents = read_dir(cwd, ents, MAXE, kind);
  listed_ok = nents >= 0;
  if (nents < 0)
    nents = 0;
  qsort(ents, (size_t)nents, sizeof *ents, ent_cmp);
  sel = scroll = 0;
  for (int i = 0; keep && i < nents; i++)
    if (!strcmp(ents[i].name, keep))
      sel = i;
  preview_drop();
}

static void open_dev(int d) {
  cdev = d;
  strcpy(cwd, devs[d].root);
  /* the device's Media folder, or one of the kind's name in it */
  char p[256];
  join(p, sizeof p, devs[d].root, "Media");
  if (is_dir(p)) {
    strcpy(cwd, p);
    join(p, sizeof p, cwd, kind_name[kind]);
    if (is_dir(p))
      strcpy(cwd, p);
  }
  list(0);
  pane = P_LIST;
}

/* the folder of the kind next to this one (Media/Photos -> Media/Music) */
static void kind_go(int k) {
  if (k == kind)
    return;
  if (cdev >= 0) {
    const char *b = base_name(cwd);
    if (!name_cmp(b, kind_name[kind])) {
      char p[256];
      mx_snprintf(p, sizeof p, "%.*s%s", (int)(b - cwd), cwd, kind_name[k]);
      if (is_dir(p))
        strcpy(cwd, p);
    }
  }
  kind = k;
  if (cdev >= 0)
    list(0);
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

/* ---- a picture as textures ----
   In tiles of 256 x 256 (CT16, 128 KB each): libgl's cache of GS memory
   takes them one at a time, where a picture in one texture (up to 1024 x
   1024, 2 MB) does not fit beside 1080i's frame buffer. */
#define TILE 256
#define MAXT 16 /* 1024 x 1024 */
/* the tiles drawn in a frame must all be in GS memory at once: at 1080i
   (its 16-bit buffer 1920 x 540) about 11 fit beside the font */
#define MAXT_DRAWN 9
struct image {
  int tex[MAXT]; /* GL names, by row; 0 none */
  u16 w, h;      /* the picture, pixels */
  u16 tw, th;    /* in the tiles, texels */
  u8 nx, ny;
};
static u32 tile_buf[TILE * TILE] MX_ALIGNED(64);
static void image_free(struct image *im) {
  for (int i = 0; i < MAXT; i++)
    if (im->tex[i]) {
      GLuint t = (GLuint)im->tex[i];
      glDeleteTextures(1, &t);
    }
  memset(im, 0, sizeof *im);
}
/* tw x th texels from px (a line every stride texels) into the tiles: made
   at the first call (make), else changed. Each tile's texels past the
   picture repeat its edge (no dark seam where it is filtered) */
static void image_fill(struct image *im, const u32 *px, u32 stride, int make) {
  for (int j = 0; j < im->ny; j++)
    for (int i = 0; i < im->nx; i++) {
      u32 x0 = (u32)i * TILE, y0 = (u32)j * TILE;
      u32 cw = im->tw - x0 < TILE ? im->tw - x0 : TILE, ch = im->th - y0 < TILE ? im->th - y0 : TILE;
      u32 ew = cw < TILE ? cw + 1 : TILE, eh = ch < TILE ? ch + 1 : TILE;
      for (u32 y = 0; y < eh; y++) {
        const u32 *s = px + (y0 + (y < ch ? y : ch - 1)) * stride + x0;
        u32 *d = tile_buf + y * ew;
        memcpy(d, s, cw * 4);
        if (ew > cw)
          d[cw] = s[cw - 1];
      }
      int k = j * im->nx + i;
      if (make) {
        GLuint name;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* the texture's size: the tile's, a power of two */
        u32 W = 8, H = 8;
        while (W < ew)
          W <<= 1;
        while (H < eh)
          H <<= 1;
        /* made with a texel that is not grey: libgl keeps a texture of
           grey texels as brightness alone (a lightmap), and the colour of
           the texels that come later would be lost */
        static u32 blank[TILE * TILE] MX_ALIGNED(64);
        blank[0] = 0xFF000100u;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)W, (GLsizei)H, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank);
        im->tex[k] = (int)name;
      } else
        glBindTexture(GL_TEXTURE_2D, (GLuint)im->tex[k]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)ew, (GLsizei)eh, GL_RGBA, GL_UNSIGNED_BYTE, tile_buf);
    }
}
/* tiles for tw x th texels: 0, or -1 when it is too large */
static int image_make(struct image *im, u32 tw, u32 th) {
  im->tw = (u16)tw, im->th = (u16)th;
  im->nx = (u8)((tw + TILE - 1) / TILE), im->ny = (u8)((th + TILE - 1) / TILE);
  return im->nx * im->ny <= MAXT_DRAWN ? 0 : -1;
}
/* the picture on the 2D layer at (x0, y0)-(x1, y1) */
static void d_image(const struct image *im, float x0, float y0, float x1, float y1, float a) {
  glEnable(GL_TEXTURE_2D);
  glColor4f(1, 1, 1, a * ui_fade);
  float kx = (x1 - x0) / im->tw, ky = (y1 - y0) / im->th;
  for (int j = 0; j < im->ny; j++)
    for (int i = 0; i < im->nx; i++) {
      u32 tx0 = (u32)i * TILE, ty0 = (u32)j * TILE;
      u32 cw = im->tw - tx0 < TILE ? im->tw - tx0 : TILE, ch = im->th - ty0 < TILE ? im->th - ty0 : TILE;
      u32 ew = cw < TILE ? cw + 1 : TILE, eh = ch < TILE ? ch + 1 : TILE, W = 8, H = 8;
      while (W < ew)
        W <<= 1;
      while (H < eh)
        H <<= 1;
      float u = (float)cw / W, v = (float)ch / H;
      float qx0 = x0 + tx0 * kx, qy0 = y0 + ty0 * ky, qx1 = qx0 + cw * kx, qy1 = qy0 + ch * ky;
      glBindTexture(GL_TEXTURE_2D, (GLuint)im->tex[j * im->nx + i]);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0), glVertex2f(qx0, qy0);
      glTexCoord2f(u, 0), glVertex2f(qx1, qy0);
      glTexCoord2f(u, v), glVertex2f(qx1, qy1);
      glTexCoord2f(0, v), glVertex2f(qx0, qy1);
      glEnd();
    }
  glDisable(GL_TEXTURE_2D);
}
/* largest pictures decoded: their pixels are pinned (4 bytes each) */
#define PIC_MAX_PIXELS (2600u * 1700u)
#define PIC_MAX_BYTES (12u << 20)

/* path decoded and scaled to fit max x max (averaging): 0, or a message */
static const char *image_load(struct image *im, const char *path, u32 max) {
  memset(im, 0, sizeof *im);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return "Cannot open it";
  int bytes = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  if (bytes <= 0 || (u32)bytes > PIC_MAX_BYTES) {
    mx_fio_close(fd);
    return bytes <= 0 ? "Empty" : "Too large a file";
  }
  u8 *data = malloc((u32)bytes);
  if (!data) {
    mx_fio_close(fd);
    return "No memory for it";
  }
  int got = mx_fio_read(fd, data, (u32)bytes);
  mx_fio_close(fd);
  u16 w = 0, h = 0;
  if (got != bytes || mx_pic_size(data, (u32)bytes, &w, &h) < 0) {
    free(data);
    return got != bytes                                             ? "Cannot read it"
           : ends_with(path, ".jpg") || ends_with(path, ".jpeg") ? "A JPEG this cannot show (progressive?)"
                                                                     : "Not a picture this can show";
  }
  if ((u32)w * h > PIC_MAX_PIXELS) {
    free(data);
    return "Too many pixels (at most about 4 million)";
  }
  struct mx_pic p;
  int r = mx_pic_decode(data, (u32)bytes, &p);
  free(data);
  if (r)
    return r == MX_PIC_FORMAT ? "A kind this cannot show (progressive JPEG?)"
           : r == MX_PIC_DATA ? "Damaged"
                              : "No memory for it";
  /* the size in the texture: the picture, smaller to fit max x max */
  u32 tw = p.width, th = p.height;
  if (tw > max || th > max) {
    if (tw * max >= th * max && tw >= th)
      th = (th * max + tw / 2) / tw, tw = max;
    else
      tw = (tw * max + th / 2) / th, th = max;
  }
  /* and smaller until its tiles fit GS memory beside 1080i's buffer */
  while (((tw + TILE - 1) / TILE) * ((th + TILE - 1) / TILE) > MAXT_DRAWN)
    tw = tw * 15 / 16, th = th * 15 / 16;
  if (!tw)
    tw = 1;
  if (!th)
    th = 1;
  u32 *t = malloc(tw * th * 4);
  if (!t) {
    mx_pic_free(&p);
    return "No memory for it";
  }
  /* each texel the mean of the pixels it covers; opaque (the GS's alpha
     0x80 to GL's 255) */
  for (u32 y = 0; y < th; y++) {
    u32 sy0 = y * p.height / th, sy1 = (y + 1) * p.height / th;
    if (sy1 <= sy0)
      sy1 = sy0 + 1;
    for (u32 x = 0; x < tw; x++) {
      u32 sx0 = x * p.width / tw, sx1 = (x + 1) * p.width / tw;
      if (sx1 <= sx0)
        sx1 = sx0 + 1;
      u32 r0 = 0, g0 = 0, b0 = 0, n = 0;
      for (u32 sy = sy0; sy < sy1; sy++) {
        const u32 *s = p.pixels + sy * p.width + sx0;
        for (u32 sx = sx0; sx < sx1; sx++, s++, n++)
          r0 += *s & 255, g0 += *s >> 8 & 255, b0 += *s >> 16 & 255;
      }
      t[y * tw + x] = (r0 / n) | (g0 / n) << 8 | (b0 / n) << 16 | 0xFFu << 24;
    }
  }
  im->w = p.width, im->h = p.height;
  mx_pic_free(&p);
  image_make(im, tw, th);
  image_fill(im, t, tw, 1);
  free(t);
  return 0;
}

/* the box (units) w x h of a picture of pw x ph pixels fitted in bw x bh:
   the screen's pixels are not square in every mode, the units are */
static void fit(u32 pw, u32 ph, float bw, float bh, float *w, float *h) {
  float k = bw / pw < bh / ph ? bw / pw : bh / ph;
  *w = pw * k, *h = ph * k;
}

/* ---- music: a thread of its own feeds the sound's stream ---- */
enum { MU_NONE, MU_PLAY, MU_STOP, MU_PAUSE };
static struct {
  volatile int cmd, state; /* state: 0 stopped, 1 playing, 2 paused */
  volatile int ended;      /* a track came to its end (the main loop plays the next) */
  volatile u32 pos_ms, len_ms;
  volatile int error;
  volatile int hold; /* tests: no file reads while the picture is written */
  char path[256];
  int sema; /* a command done */
} mu;
/* the tracks: the folder's music when one was chosen */
#define MAXQ 64
static char queue[MAXQ][NAME];
static char queue_dir[256];
static int nqueue, qpos;

/* the track playing (the thread's own) */
static u8 mu_stack[16384] MX_ALIGNED(16) MX_CORE_BSS;
static u8 mu_in[8192] MX_ALIGNED(64) MX_CORE_BSS;
static s16 mu_pcm[MX_MP2_SAMPLES * 2] MX_ALIGNED(64) MX_CORE_BSS;
static struct mx_mp2 mu_dec MX_CORE_BSS;
static struct {
  int fd, mp2, rate, bits, ch;
  u32 in, have;         /* mu_in: taken, there */
  u32 left;             /* WAV: data bytes still to read */
  u32 pend, pend_off;   /* bytes of mu_pcm (MP2) or mu_in (WAV) still to queue */
  u64 queued;           /* bytes queued since the start */
  u32 bps;              /* bytes a second */
  int eof;
} tr;

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static void track_close(void) {
  if (tr.fd >= 0)
    mx_fio_close(tr.fd);
  tr.fd = -1;
  mx_snd_stream_stop();
}

/* open mu.path: WAV (PCM 8 or 16 bits) or MP2. 0, or -1 */
static int track_open(void) {
  memset(&tr, 0, sizeof tr);
  tr.fd = mx_fio_open(mu.path, MX_FIO_RDONLY);
  if (tr.fd < 0)
    return -1;
  int size = mx_fio_lseek(tr.fd, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(tr.fd, 0, MX_FIO_SEEK_SET);
  int n = mx_fio_read(tr.fd, mu_in, sizeof mu_in);
  if (n < 12)
    return -1;
  if (!memcmp(mu_in, "RIFF", 4) && !memcmp(mu_in + 8, "WAVE", 4)) {
    /* the chunks up to "data" (in the first 8 KB) */
    u32 at = 12;
    while (at + 8 <= (u32)n) {
      u32 len = rd32(mu_in + at + 4);
      if (!memcmp(mu_in + at, "fmt ", 4) && at + 24 <= (u32)n) {
        if ((mu_in[at + 8] | mu_in[at + 9] << 8) != 1)
          return -1; /* not PCM */
        tr.ch = mu_in[at + 10], tr.rate = (int)rd32(mu_in + at + 12), tr.bits = mu_in[at + 22];
      } else if (!memcmp(mu_in + at, "data", 4)) {
        tr.in = at + 8, tr.have = (u32)n;
        tr.left = len - ((u32)n - tr.in < len ? (u32)n - tr.in : len);
        if (tr.have - tr.in > len)
          tr.have = tr.in + len;
        break;
      }
      at += 8 + len + (len & 1);
    }
    if (!tr.rate || !tr.in || (tr.bits != 8 && tr.bits != 16) || tr.ch < 1 || tr.ch > 2)
      return -1;
    tr.bps = (u32)tr.rate * (u32)tr.ch * (u32)tr.bits / 8;
    mu.len_ms = (u32)((u64)(tr.have - tr.in + tr.left) * 1000 / tr.bps);
    return mx_snd_stream_format((u32)tr.rate, tr.bits, tr.ch);
  }
  /* MP2: its length from the first frame's bit rate */
  tr.mp2 = 1, tr.have = (u32)n;
  mx_mp2_init(&mu_dec);
  static const u16 kbps[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
  for (int i = 0; i + 3 < n; i++)
    if (mu_in[i] == 0xFF && (mu_in[i + 1] & 0xFE) == 0xFC) { /* MPEG-1 layer II */
      u32 k = kbps[mu_in[i + 2] >> 4];
      if (k)
        mu.len_ms = (u32)((u64)(size - i) * 8 / k);
      break;
    }
  return 0;
}

/* the stream's room filled, at most 150 ms ahead and a few frames at a
   time (the thread runs above the main one): 1 while there is more */
static int track_feed(void) {
  for (int steps = 0;; steps++) {
    if (steps >= 16 || (tr.bps && mx_snd_stream_queued() >= tr.bps * 3 / 20))
      return 1;
    if (tr.pend) {
      const u8 *src = tr.mp2 ? (const u8 *)mu_pcm : mu_in;
      u32 q = mx_snd_stream_queue(src + tr.pend_off, tr.pend);
      tr.pend -= q, tr.pend_off += q, tr.queued += q;
      if (tr.pend)
        return 1; /* full */
      if (!tr.mp2)
        tr.in = tr.have; /* all of mu_in queued */
    }
    if (!tr.mp2) {
      if (tr.in < tr.have) {
        tr.pend = tr.have - tr.in, tr.pend_off = tr.in;
        tr.pend -= tr.pend % (u32)(tr.ch * tr.bits / 8);
        if (!tr.pend)
          return 0;
        continue;
      }
      if (!tr.left)
        return 0;
      u32 want = tr.left < sizeof mu_in ? tr.left : sizeof mu_in;
      int n = mx_fio_read(tr.fd, mu_in, want);
      if (n <= 0)
        return 0;
      tr.in = 0, tr.have = (u32)n, tr.left -= (u32)n;
      continue;
    }
    /* MP2: the next frame */
    u32 frames = 0, rate = 0;
    u32 used = mx_mp2_decode(&mu_dec, mu_in + tr.in, tr.have - tr.in, mu_pcm, &frames, &rate);
    if (!used) {
      /* the frame is not all here: more of the file */
      if (tr.eof)
        return 0;
      memmove(mu_in, mu_in + tr.in, tr.have - tr.in);
      tr.have -= tr.in, tr.in = 0;
      int n = mx_fio_read(tr.fd, mu_in + tr.have, sizeof mu_in - tr.have);
      if (n <= 0)
        tr.eof = 1;
      else
        tr.have += (u32)n;
      continue;
    }
    tr.in += used;
    if (!frames)
      continue;
    if ((int)rate != tr.rate) {
      tr.rate = (int)rate, tr.ch = 2, tr.bits = 16, tr.bps = rate * 4;
      mx_snd_stream_format(rate, 16, 2);
    }
    tr.pend = frames * 4, tr.pend_off = 0;
  }
}

static void music_thread(void *arg) {
  (void)arg;
  int more = 0;
  for (;;) {
    int c = mu.cmd;
    if (c) {
      if (c == MU_PLAY) {
        if (mu.state)
          track_close();
        mu.error = 0, mu.ended = 0, mu.pos_ms = 0, mu.len_ms = 0;
        if (track_open() < 0) {
          track_close();
          mu.state = 0, mu.error = 1;
        } else
          mu.state = 1, more = 1;
      } else if (c == MU_STOP) {
        if (mu.state)
          track_close();
        mu.state = 0;
      } else if (c == MU_PAUSE && mu.state) {
        mu.state = mu.state == 1 ? 2 : 1;
        if (mu.state == 2)
          mx_snd_stream_volume(0);
        else
          mx_snd_stream_volume(MX_SND_VOL_MAX);
      }
      mu.cmd = 0;
      mx_sema_signal(mu.sema);
    }
    if (mu.state == 1 && !mu.hold) {
      if (more)
        more = track_feed();
      u32 q = mx_snd_stream_queued();
      if (tr.bps)
        mu.pos_ms = (u32)((tr.queued > q ? tr.queued - q : 0) * 1000 / tr.bps);
      if (!more && !q) {
        track_close();
        mu.state = 0, mu.ended = 1;
      }
      mx_snd_commit();
    }
    mx_sleep_us(20000);
  }
}

/* a command to the thread, waited for */
static void music_cmd(int c) {
  mu.cmd = c;
  mx_sema_wait(mu.sema);
}
static int music_started;
/* the sound's modules loaded here, in the main thread (loading them from
   the music's thread did not finish) */
static int music_start(void) {
  if (music_started)
    return 0;
  if (mx_snd_init() < 0)
    return -1;
  mu.sema = mx_sema_create(0, 1);
  mx_thread_start(music_thread, 0, mu_stack, sizeof mu_stack, mx_thread_prio(mx_thread_self()) - 1);
  music_started = 1;
  return 0;
}
static void music_play_at(int i) {
  if (i < 0 || i >= nqueue)
    return;
  qpos = i;
  join(mu.path, sizeof mu.path, queue_dir, queue[i]);
  if (music_start() < 0) {
    toast(N_WARN, "No sound", "The sound's modules did not load", 0);
    return;
  }
  music_cmd(MU_PLAY);
  if (mu.error) {
    char t[NAME];
    title_of(t, sizeof t, queue[i]);
    toast(N_WARN, ends_with(queue[i], ".mp3") ? "MP3 does not play yet" : "Cannot play it", t, 0);
  }
}
/* the folder's music, from entry e on */
static void music_play_folder(int e) {
  nqueue = 0;
  strcpy(queue_dir, cwd);
  int at = 0;
  for (int i = 0; i < nents && nqueue < MAXQ; i++)
    if (!ents[i].dir) {
      if (i == e)
        at = nqueue;
      strcpy(queue[nqueue++], ents[i].name);
    }
  music_play_at(at);
}
static void music_stop(void) {
  if (music_started && mu.state)
    music_cmd(MU_STOP);
}
/* at a track's end: the next (MP3s passed over) */
static void music_step(void) {
  if (!mu.ended)
    return;
  mu.ended = 0;
  for (int i = qpos + 1; i < nqueue; i++)
    if (!ends_with(queue[i], ".mp3")) {
      music_play_at(i);
      return;
    }
}

static int ipu_ready;

/* ---- the preview of the chosen entry (16:9) ---- */
static struct image thumb;
static char thumb_path[256], thumb_err[64], info[96];
static u32 chosen_at;
static void preview_drop(void) {
  image_free(&thumb);
  thumb_path[0] = thumb_err[0] = info[0] = 0;
  chosen_at = ticks;
}
static u32 mp2_len_ms(const char *path, u32 *kbps_out);

/* once the choice has stayed a moment: its picture or what it is */
static void preview_step(void) {
  if (cdev < 0 || pane != P_LIST || !nents || ents[sel].dir || ticks - chosen_at < 12)
    return;
  char p[256];
  join(p, sizeof p, cwd, ents[sel].name);
  if (!strcmp(p, thumb_path))
    return;
  preview_drop();
  strcpy(thumb_path, p);
  if (kind == K_PHOTO) {
    const char *e = image_load(&thumb, p, 256);
    if (e)
      mx_snprintf(thumb_err, sizeof thumb_err, "%s", e);
    else
      mx_snprintf(info, sizeof info, "%u x %u", thumb.w, thumb.h);
  } else if (kind == K_VIDEO) {
    if (!ipu_ready)
      ipu_ready = mx_ipu_init() >= 0;
    struct mx_movie *mv = ipu_ready ? mx_movie_open(p, MX_MOVIE_NO_SOUND) : 0;
    if (!mv)
      mx_snprintf(thumb_err, sizeof thumb_err, "Not a video this can play");
    else {
      const struct mx_movie_info *in = mx_movie_info(mv);
      char t[16] = "";
      if (in->duration_ms)
        fmt_time(t, sizeof t, in->duration_ms);
      mx_snprintf(info, sizeof info, "%u x %u" DOT "%u fps%s%s%s", in->width, in->height,
                  (in->rate_num + in->rate_den / 2) / in->rate_den, t[0] ? DOT : "", t,
                  in->predicted ? DOT "IPB" : "");
      mx_movie_close(mv);
    }
  } else {
    if (ends_with(p, ".mp3"))
      mx_snprintf(thumb_err, sizeof thumb_err, "MP3 does not play yet");
    else if (ends_with(p, ".wav")) {
      int fd = mx_fio_open(p, MX_FIO_RDONLY);
      u8 h[44];
      if (fd >= 0 && mx_fio_read(fd, h, 44) == 44 && !memcmp(h + 8, "WAVE", 4)) {
        u32 rate = rd32(h + 24), bps = rd32(h + 28);
        char t[16];
        fmt_time(t, sizeof t, bps ? (u32)((u64)rd32(h + 40) * 1000 / bps) : 0);
        mx_snprintf(info, sizeof info, "WAV" DOT "%u Hz" DOT "%u-bit" DOT "%s", rate, h[34], t);
      }
      if (fd >= 0)
        mx_fio_close(fd);
    } else {
      u32 k = 0, ms = mp2_len_ms(p, &k);
      char t[16];
      fmt_time(t, sizeof t, ms);
      mx_snprintf(info, sizeof info, "MP2" DOT "%u kbit/s" DOT "%s", k, t);
    }
  }
}
static u32 mp2_len_ms(const char *path, u32 *kbps_out) {
  static const u16 kbps[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  u8 h[512];
  int n = mx_fio_read(fd, h, sizeof h);
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  mx_fio_close(fd);
  for (int i = 0; i + 3 < n; i++)
    if (h[i] == 0xFF && (h[i + 1] & 0xFE) == 0xFC && kbps[h[i + 2] >> 4]) {
      *kbps_out = kbps[h[i + 2] >> 4];
      return (u32)((u64)(size - i) * 8 / *kbps_out);
    }
  return 0;
}

/* ---- the photo viewer ---- */
static struct image photo;
static char photo_err[64];
static int photo_at; /* its entry */
static u32 shown_at;
static void photo_open(int i) {
  image_free(&photo);
  photo_err[0] = 0;
  photo_at = i;
  char p[256];
  join(p, sizeof p, cwd, ents[i].name);
  const char *e = image_load(&photo, p, 1024);
  if (e)
    mx_snprintf(photo_err, sizeof photo_err, "%s", e);
  shown_at = ticks;
  view = V_PHOTO;
}
static void photo_next(int d) {
  for (int i = photo_at + d; i >= 0 && i < nents; i += d)
    if (!ents[i].dir) {
      sel = i;
      photo_open(i);
      return;
    }
}

/* ---- the video player ---- */
static struct mx_movie *movie;
static struct image mv_im;
static u32 *mv_px;
static u32 mv_w, mv_paused;
static char mv_title[NAME];
static void video_close(void) {
  if (movie)
    mx_movie_close(movie);
  movie = 0;
  image_free(&mv_im);
  free(mv_px);
  mv_px = 0;
  view = V_BROWSE;
}
static void video_open(int i) {
  char p[256];
  join(p, sizeof p, cwd, ents[i].name);
  title_of(mv_title, sizeof mv_title, ents[i].name);
  /* the sound's stream is the movie's now */
  music_stop();
  if (!ipu_ready)
    ipu_ready = mx_ipu_init() >= 0;
  movie = ipu_ready ? mx_movie_open(p, 0) : 0;
  if (!movie) {
    toast(N_WARN, "Cannot play it", mv_title, 0);
    return;
  }
  const struct mx_movie_info *in = mx_movie_info(movie);
  /* the picture as lines of pixels (whole macroblocks), then into tiles */
  mv_w = in->mbw * 16u;
  mv_px = calloc(mv_w * in->mbh * 16u, 4);
  if (!mv_px || image_make(&mv_im, in->width, in->height) < 0) {
    toast(N_WARN, mv_px ? "Too large a picture (768 x 768 at most)" : "No memory for it", mv_title, 0);
    video_close();
    return;
  }
  image_fill(&mv_im, mv_px, mv_w, 1);
  mv_paused = 0;
  shown_at = ticks;
  view = V_VIDEO;
  mx_printf("media: video %s: %u x %u, %u/%u fps, %s\n", p, in->width, in->height, in->rate_num, in->rate_den,
            in->sound ? "sound" : "no sound");
}
/* a frame's step: the next picture into the tiles when it is due */
static void video_step(void) {
  int r = mx_movie_update(movie);
  if (r < 0) {
    mx_printf("media: video ended: %u pictures, decoding %u us, %u errors\n", mx_movie_info(movie)->pictures,
              mx_movie_info(movie)->decode_us, mx_movie_info(movie)->errors);
    video_close();
    return;
  }
  if (r > 0) {
    mx_movie_raster(movie, mv_px, mv_w * 4);
    image_fill(&mv_im, mv_px, mv_w, 0);
  }
}

/* ---- drawing ---- */
#define TOPY 112.0f
#define BOTY 636.0f
#define ROW 36.0f
static int wide(void) { return SCR_W > 1100; }
static float dev_w(void) { return wide() ? 250.0f : 214.0f; }
static float list_x0(void) { return MARGIN + dev_w() + 20; }
static float list_x1(void) { return wide() ? SCR_W - MARGIN - 320 : SCR_W - MARGIN; }
static int list_rows(void) { return (int)((BOTY - TOPY - 66) / ROW); }

static void folder_icon(float x, float y, u32 c) {
  d_rect(x, y - 14, x + 8, y - 10, c, c);
  d_rect(x, y - 10, x + 20, y + 2, c, c);
}
/* the kinds' icons, lines 2 units wide */
static void kind_icon(int k, float x, float y, u32 c) {
  if (k == K_PHOTO) { /* a frame with a hill */
    d_rect(x, y - 16, x + 20, y - 14, c, c), d_rect(x, y, x + 20, y + 2, c, c);
    d_rect(x, y - 16, x + 2, y + 2, c, c), d_rect(x + 18, y - 16, x + 20, y + 2, c, c);
    d_rect(x + 5, y - 5, x + 9, y - 1, c, c), d_rect(x + 9, y - 9, x + 15, y - 1, c, c);
  } else if (k == K_VIDEO) { /* a play triangle in steps */
    for (int i = 0; i < 9; i++) {
      float h = 9 - i;
      d_rect(x + 4 + i * 1.6f, y - 7 - h, x + 5.6f + i * 1.6f, y - 7 + h, c, c);
    }
  } else { /* a note */
    d_rect(x + 12, y - 17, x + 14, y - 2, c, c);
    d_rect(x + 12, y - 17, x + 19, y - 14, c, c);
    d_rect(x + 5, y - 5, x + 14, y + 1, c, c);
  }
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
    d_text(FONT_SMALL, x0 + 20, y + 18, devs[d].state == ST_READY ? devs[d].root : st[devs[d].state],
           argb(devs[d].state == ST_NONE ? C_WARN : C_FAINT, 1));
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
  d_label(x0 + 24, TOPY + 34, kind_name[kind], argb(C_FAINT, 1));
  if (!listed_ok) {
    d_text(FONT_BODY, x0 + 24, TOPY + 90, "This folder does not open", argb(C_WARN, 1));
    return;
  }
  if (!nents) {
    char m[64];
    mx_snprintf(m, sizeof m, "No %s here", kind == K_MUSIC ? "music" : kind == K_PHOTO ? "photos" : "videos");
    d_text(FONT_BODY, x0 + 24, TOPY + 90, m, argb(C_DIM, 1));
    return;
  }
  int rows = list_rows();
  if (sel < scroll)
    scroll = sel;
  if (sel >= scroll + rows)
    scroll = sel - rows + 1;
  int files = 0;
  for (int i = 0; i < nents; i++)
    files += !ents[i].dir;
  for (int r = 0; r < rows && scroll + r < nents; r++) {
    int i = scroll + r;
    const struct ent *e = &ents[i];
    float y = TOPY + 76 + r * ROW;
    if (i == sel) {
      float a = pane == P_LIST ? 0.09f : 0.04f;
      d_highlight(x0 + 8, y - 25, x1 - 8, y + 11, 8, a, a * 0.5f, pane == P_LIST ? 0.6f : 0);
    }
    char p[256];
    join(p, sizeof p, cwd, e->name);
    int playing = kind == K_MUSIC && mu.state && !strcmp(p, mu.path);
    u32 ic = playing ? C_LAMP : i == sel ? C_DIM : C_FAINT;
    if (e->dir)
      folder_icon(x0 + 26, y - 1, argb(ic, 1));
    else
      kind_icon(kind, x0 + 24, y, argb(ic, 1));
    char sz[16] = "", t[NAME];
    if (!e->dir)
      fmt_size(sz, sizeof sz, e->size);
    float sw = d_text_w(FONT_SMALL, sz);
    if (e->dir)
      mx_snprintf(t, sizeof t, "%s", e->name);
    else
      title_of(t, sizeof t, e->name);
    int mute = !e->dir && ends_with(e->name, ".mp3");
    d_text_fit(FONT_BODY, FONT_SMALL, x0 + 56, y, x1 - x0 - 100 - sw, t,
               argb(playing ? C_LAMP : mute ? C_FAINT : i == sel ? C_WHITE : C_DIM, 1));
    d_text(FONT_SMALL, x1 - 24 - sw, y, sz, argb(C_FAINT, 1));
  }
  char m[48];
  mx_snprintf(m, sizeof m, "%d %s", files, kind == K_MUSIC ? (files == 1 ? "track" : "tracks")
                                           : kind == K_PHOTO ? (files == 1 ? "photo" : "photos")
                                                             : (files == 1 ? "video" : "videos"));
  d_text(FONT_SMALL, x0 + 24, BOTY - 20, m, argb(C_FAINT, 1));
  if (nents > rows) {
    float h = BOTY - TOPY - 120, t = TOPY + 52 + h * scroll / nents, b = t + h * rows / nents;
    d_rect(x1 - 6, t, x1 - 4, b, argb(C_WHITE, 0.25f), argb(C_WHITE, 0.25f));
  }
}

static void draw_preview(void) {
  if (!wide())
    return;
  float x0 = SCR_W - MARGIN - 300, x1 = SCR_W - MARGIN;
  d_card(x0, TOPY, x1, BOTY, 18, C_GRAPHITE);
  d_label(x0 + 20, TOPY + 34, "Selected", argb(C_FAINT, 1));
  if (cdev < 0 || pane != P_LIST || !nents)
    return;
  const struct ent *e = &ents[sel];
  char t[NAME];
  if (e->dir)
    mx_snprintf(t, sizeof t, "%s", e->name);
  else
    title_of(t, sizeof t, e->name);
  float y = TOPY + 70;
  /* the picture: a photo's own, else the kind's mark on a slab */
  float bw = x1 - x0 - 40, bh = 200;
  if (thumb.nx) {
    float w, h;
    fit(thumb.w, thumb.h, bw, bh, &w, &h);
    float px = x0 + 20 + (bw - w) / 2, py = y + (bh - h) / 2;
    d_image(&thumb, px, py, px + w, py + h, 1);
  } else {
    surface_draw(&(struct surface){.x0 = x0 + 20, .y0 = y, .x1 = x1 - 20, .y1 = y + bh, .r = 10, .fill = C_SLAB,
                                   .glass = -1, .a = 1, .grad = 1, .fill_b = C_DEEP, .fa = 1, .fa_b = 1});
    float cx = (x0 + x1) / 2;
    if (e->dir)
      folder_icon(cx - 10, y + bh / 2 + 6, argb(C_FAINT, 1));
    else
      kind_icon(kind, cx - 10, y + bh / 2 + 8, argb(C_FAINT, 1));
  }
  y += bh + 40;
  d_text_fit(FONT_BODY, FONT_SMALL, x0 + 20, y, x1 - x0 - 40, t, argb(C_WHITE, 1));
  y += 28;
  if (e->dir)
    d_text(FONT_SMALL, x0 + 20, y, "Folder", argb(C_FAINT, 1));
  else if (thumb_err[0])
    d_text_fit(FONT_SMALL, FONT_SMALL, x0 + 20, y, x1 - x0 - 40, thumb_err, argb(C_WARN, 1));
  else if (info[0])
    d_text_fit(FONT_SMALL, FONT_SMALL, x0 + 20, y, x1 - x0 - 40, info, argb(C_DIM, 1));
  else
    d_text(FONT_SMALL, x0 + 20, y, "Looking", argb(C_FAINT, 1));
}

/* the line of the track playing, along the bottom left (when one plays) */
static void draw_now_line(void) {
  if (!mu.state || view == V_MUSIC)
    return;
  char t[NAME], a[16], b[16], m[NAME + 40];
  title_of(t, sizeof t, mu.path);
  fmt_time(a, sizeof a, mu.pos_ms), fmt_time(b, sizeof b, mu.len_ms);
  mx_snprintf(m, sizeof m, "%s%s" DOT "%s / %s", mu.state == 2 ? "Paused: " : "", t, a, b);
  float x = MARGIN, y = 680;
  kind_icon(K_MUSIC, x, y - 2, argb(C_LAMP, 1));
  d_text_fit(FONT_SMALL, FONT_SMALL, x + 30, y, SCR_W * 0.42f, m, argb(C_DIM, 1));
}

static void kind_tabs(float x, float y) {
  for (int k = 0; k < NKINDS; k++) {
    int on = k == kind;
    float w = d_text_w(FONT_BUTTON, kind_name[k]);
    d_text(FONT_BUTTON, x, y, kind_name[k], argb(on ? C_WHITE : C_FAINT, 1));
    if (on)
      d_glow(x + w / 2, y + 16, 5, 3, argb(C_LAMP, 1));
    x += w + 30;
  }
}

static void draw_browse(void) {
  room_begin(-1);
  room_light(MARGIN + 160, 140, 520, 320, 0.05f);
  d_begin_2d();
  room_draw();
  float x = d_text(FONT_TITLE, MARGIN - 2, 80, "Media", argb(C_WHITE, 1));
  kind_tabs(x + 34, 78);
  if (cdev >= 0 && wide())
    d_text_fit(FONT_SMALL, FONT_SMALL, SCR_W - MARGIN - 300, 78, 300, cwd, argb(C_FAINT, 1));
  draw_devices();
  draw_list();
  draw_preview();
  draw_now_line();
  static const char *const h_dev[] = {SYM_CROSS, "Open", "L1", "Kind", "R1", "Kind", SYM_CIRCLE, "Close", 0};
  const char *act = nents && ents[sel].dir ? "Open" : kind == K_PHOTO ? "View" : "Play";
  const char *h_list[] = {SYM_CROSS, act, "L1", "Kind", "R1", "Kind", SYM_CIRCLE, "Up", 0};
  const char *h_listm[] = {SYM_CROSS, act, "START", "Now playing", "L1", "Kind", "R1", "Kind", SYM_CIRCLE, "Up", 0};
  hints_draw(pane == P_DEVICES ? h_dev : mu.state ? h_listm : h_list);
}

/* full screen: the ink and a soft light, no room */
static void dark_begin(void) {
  d_begin_2d();
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 1), argb(C_INK, 1));
}
/* the caption of a full-screen view: shown a while after a button */
static float caption_alpha(void) {
  float t = (float)(ticks - shown_at) * frame_dt;
  return t < 3 ? 1 : t < 3.5f ? 1 - (t - 3) / 0.5f : 0;
}

static void draw_photo(void) {
  dark_begin();
  if (photo.nx) {
    float w, h;
    fit(photo.tw, photo.th, SCR_W, SCR_H, &w, &h);
    d_image(&photo, (SCR_W - w) / 2, (SCR_H - h) / 2, (SCR_W + w) / 2, (SCR_H + h) / 2, 1);
  } else
    d_text(FONT_BODY, MARGIN, SCR_H / 2, photo_err, argb(C_WARN, 1));
  float a = caption_alpha();
  if (a <= 0)
    return;
  /* the caption on a shade at the bottom (over the photo only: on the ink
     a shade shows as a band in 16-bit colour) */
  if (photo.nx) {
    float w, h;
    fit(photo.tw, photo.th, SCR_W, SCR_H, &w, &h);
    float y0 = SCR_H - 150 > (SCR_H - h) / 2 ? SCR_H - 150 : (SCR_H - h) / 2;
    d_rect((SCR_W - w) / 2, y0, (SCR_W + w) / 2, (SCR_H + h) / 2, argb(C_INK, 0), argb(C_INK, 0.8f * a));
  }
  char t[NAME], m[64];
  title_of(t, sizeof t, ents[photo_at].name);
  int n = 0, at = 0;
  for (int i = 0; i < nents; i++)
    if (!ents[i].dir)
      n++, at += i <= photo_at;
  if (photo.nx)
    mx_snprintf(m, sizeof m, "%d of %d" DOT "%u x %u", at, n, photo.w, photo.h);
  else
    mx_snprintf(m, sizeof m, "%d of %d", at, n);
  d_text_fit(FONT_BUTTON, FONT_BODY, MARGIN, SCR_H - 74, SCR_W - 2 * MARGIN, t, argb(C_WHITE, a));
  d_text(FONT_SMALL, MARGIN, SCR_H - 46, m, argb(C_DIM, a));
  float keep = ui_fade;
  ui_fade = a;
  static const char *const hint[] = {"L1", "Previous", "R1", "Next", SYM_CIRCLE, "Back", 0};
  hints_draw(hint);
  ui_fade = keep;
}

static void draw_video(void) {
  dark_begin();
  const struct mx_movie_info *in = mx_movie_info(movie);
  float w, h;
  /* the shape on the screen (16:9 for a wide 720 x 576), not the pixels' */
  if (in->aspect_w && in->aspect_h)
    fit(in->aspect_w, in->aspect_h, SCR_W, SCR_H, &w, &h);
  else
    fit(in->width, in->height, SCR_W, SCR_H, &w, &h);
  d_image(&mv_im, (SCR_W - w) / 2, (SCR_H - h) / 2, (SCR_W + w) / 2, (SCR_H + h) / 2, 1);
  float a = mv_paused ? 1 : caption_alpha();
  if (a <= 0)
    return;
  {
    float y0 = SCR_H - 170 > (SCR_H - h) / 2 ? SCR_H - 170 : (SCR_H - h) / 2;
    d_rect((SCR_W - w) / 2, y0, (SCR_W + w) / 2, (SCR_H + h) / 2, argb(C_INK, 0), argb(C_INK, 0.85f * a));
  }
  d_text_fit(FONT_BUTTON, FONT_BODY, MARGIN, SCR_H - 96, SCR_W - 2 * MARGIN, mv_title, argb(C_WHITE, a));
  /* where it is: a line of the lamp along the length */
  u32 now = mx_movie_time_ms(movie), len = in->duration_ms;
  float lx0 = MARGIN, lx1 = SCR_W - MARGIN, ly = SCR_H - 66;
  float k = len ? clampf((float)now / len, 0, 1) : 0;
  d_bar(lx0, ly, lx1, ly + 3, k, C_LAMP, a);
  if (len) {
    d_glow(lx0 + (lx1 - lx0) * k, ly + 1.5f, 12, 8, argb(C_LAMP, 0.5f * a));
  }
  char t0[16], t1[16], m[40];
  fmt_time(t0, sizeof t0, now), fmt_time(t1, sizeof t1, len);
  mx_snprintf(m, sizeof m, len ? "%s / %s" : "%s", t0, t1);
  d_text(FONT_SMALL, MARGIN, SCR_H - 32, m, argb(C_DIM, a));
  if (mv_paused)
    d_label(MARGIN + d_text_w(FONT_SMALL, m) + 24, SCR_H - 32, "Paused", argb(C_LAMP, a));
  float keep = ui_fade;
  ui_fade = a;
  static const char *const hint[] = {SYM_CROSS, "Pause", "L1", "-10 s", "R1", "+10 s", SYM_CIRCLE, "Stop", 0};
  static const char *const hint_p[] = {SYM_CROSS, "Play", "L1", "-10 s", "R1", "+10 s", SYM_CIRCLE, "Stop", 0};
  hints_draw(mv_paused ? hint_p : hint);
  ui_fade = keep;
}

static void draw_music(void) {
  room_begin(-1);
  room_light(SCR_W * 0.3f, 300, 600, 380, 0.07f);
  d_begin_2d();
  room_draw();
  float x = d_text(FONT_TITLE, MARGIN - 2, 80, "Media", argb(C_WHITE, 1));
  kind_tabs(x + 34, 78);
  /* the slab of the record: the kind's mark, lit */
  float sx = MARGIN, sy = 150, ss = wide() ? 300 : 240;
  d_card(sx, sy, sx + ss, sy + ss, 18, C_SLAB);
  d_glow(sx + ss / 2, sy + ss / 2, ss * 0.45f, ss * 0.45f, argb(C_LAMP, 0.08f));
  /* a note: its head, stem and flag */
  {
    u32 c = mu.state == 1 ? C_LAMP : C_DIM;
    float k = ss / 300, cx = sx + ss / 2, cy = sy + ss / 2;
    aa_under(C_SLAB);
    d_pill(cx - 46 * k, cy + 26 * k, cx + 6 * k, cy + 62 * k, c);
    d_rect(cx - 4 * k, cy - 70 * k, cx + 6 * k, cy + 44 * k, argb(c, 1), argb(c, 1));
    for (int i = 0; i < 6; i++) /* the flag, in steps down to the right */
      d_rect(cx + 6 * k, cy - (70 - i * 9) * k, cx + (18 + i * 6) * k, cy - (58 - i * 9) * k, argb(c, 1),
             argb(c, 1));
    aa_under_room();
  }
  float tx = sx + ss + 56, tx1 = SCR_W - MARGIN, ty = 196;
  char t[NAME];
  title_of(t, sizeof t, mu.path);
  d_label(tx, ty, mu.state == 1 ? "Now playing" : mu.state == 2 ? "Paused" : "Stopped", argb(C_LAMP, 1));
  d_text_fit(FONT_DISP, FONT_TITLE, tx - 2, ty + 58, tx1 - tx, mu.path[0] ? t : "Nothing", argb(C_WHITE, 1));
  char where[96];
  mx_snprintf(where, sizeof where, "%s" DOT "track %d of %d", base_name(queue_dir), qpos + 1, nqueue);
  if (!base_name(queue_dir)[0])
    mx_snprintf(where, sizeof where, "track %d of %d", qpos + 1, nqueue);
  d_text(FONT_BODY, tx, ty + 92, where, argb(C_DIM, 1));
  /* where it is */
  float ly = ty + 140;
  float p = mu.len_ms ? clampf((float)mu.pos_ms / mu.len_ms, 0, 1) : 0;
  d_bar(tx, ly, tx1, ly + 3, p, C_LAMP, 1);
  if (mu.len_ms) {
    d_glow(tx + (tx1 - tx) * p, ly + 1.5f, 12, 8, argb(C_LAMP, 0.5f));
  }
  char a[16], b[16];
  fmt_time(a, sizeof a, mu.pos_ms), fmt_time(b, sizeof b, mu.len_ms);
  d_text(FONT_SMALL, tx, ly + 30, a, argb(C_DIM, 1));
  d_text(FONT_SMALL, tx1 - d_text_w(FONT_SMALL, b), ly + 30, b, argb(C_DIM, 1));
  /* the next ones */
  float y = ly + 92;
  d_label(tx, y, "Up next", argb(C_FAINT, 1));
  int shown = 0;
  for (int i = qpos + 1; i < nqueue && shown < 4; i++, shown++) {
    title_of(t, sizeof t, queue[i]);
    d_text_fit(FONT_BODY, FONT_SMALL, tx, y + 36 + 32 * shown, tx1 - tx, t,
               argb(ends_with(queue[i], ".mp3") ? C_FAINT : C_DIM, 1));
  }
  if (!shown)
    d_text(FONT_BODY, tx, y + 36, "The end of the folder", argb(C_FAINT, 1));
  static const char *const hint[] = {SYM_CROSS, "Pause", "L1", "Previous", "R1", "Next", SYM_SQUARE, "Stop",
                                  SYM_CIRCLE, "Back", 0};
  static const char *const hint_p[] = {SYM_CROSS, "Play", "L1", "Previous", "R1", "Next", SYM_SQUARE, "Stop",
                                   SYM_CIRCLE, "Back", 0};
  hints_draw(mu.state == 1 ? hint : hint_p);
}

static void frame(void) {
  glClearColor((C_INK >> 16 & 255) / 255.0f, (C_INK >> 8 & 255) / 255.0f, (C_INK & 255) / 255.0f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  switch (view) {
  case V_PHOTO: draw_photo(); break;
  case V_VIDEO: draw_video(); break;
  case V_MUSIC: draw_music(); break;
  default: draw_browse(); break;
  }
  d_begin_2d();
  toast_draw();
  mxgl_end_frame();
  mxgl_frame_done();
}

/* ---- the devices (as Files) ---- */
static void video_close(void);
static void music_stop(void);
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
  /* the device open went: what played from it stops */
  if (was[0] && cdev < 0) {
    if (view == V_VIDEO)
      video_close();
    if (mu.state && !strncmp(mu.path, was, strlen(was)))
      music_stop();
    view = V_BROWSE;
    toast(N_DEVICE, "Removed", was, 0);
    pane = P_DEVICES, nents = 0, cwd[0] = 0;
  }
  if (dsel >= ndev)
    dsel = ndev - 1;
}

/* the PC: looked at when chosen */
static void look_step(void) {
  for (int d = 0; d < ndev; d++)
    if (devs[d].state == ST_LOOKING) {
      int fd = mx_fio_dopen(devs[d].root);
      if (fd >= 0)
        mx_fio_dclose(fd);
      devs[d].state = fd >= 0 ? ST_READY : ST_NONE;
    }
}

/* the chosen entry opened: a folder, or the file in its view */
static void open_entry(void) {
  if (!nents)
    return;
  if (ents[sel].dir) {
    char p[256];
    join(p, sizeof p, cwd, ents[sel].name);
    if (strlen(p) < sizeof cwd - 2) {
      strcpy(cwd, p);
      list(0);
    }
    return;
  }
  if (kind == K_PHOTO)
    photo_open(sel);
  else if (kind == K_VIDEO)
    video_open(sel);
  else {
    if (ends_with(ents[sel].name, ".mp3")) {
      char t[NAME];
      title_of(t, sizeof t, ents[sel].name);
      toast(N_WARN, "MP3 does not play yet", t, 0);
      return;
    }
    music_play_folder(sel);
    if (mu.state)
      view = V_MUSIC;
  }
}

static void up_folder(void) {
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
  if (!strchr(cwd, '/'))
    strcpy(cwd, devs[cdev].root);
  list(was);
}

static void browse_input(void) {
  u32 p = pad_pressed;
  if (p & MX_PAD_L1)
    kind_go((kind + NKINDS - 1) % NKINDS);
  if (p & MX_PAD_R1)
    kind_go((kind + 1) % NKINDS);
  if (p & MX_PAD_START && mu.state)
    view = V_MUSIC;
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
        while (devs[dsel].state == ST_LOOKING) {
          look_step();
          if (devs[dsel].state == ST_LOOKING)
            mx_sleep_us(250000);
        }
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
  int rows = list_rows(), was = sel;
  if (p & MX_PAD_UP && sel > 0)
    sel--;
  if (p & MX_PAD_DOWN && sel + 1 < nents)
    sel++;
  if (p & MX_PAD_LEFT)
    pane = P_DEVICES, dsel = cdev;
  (void)rows;
  if (sel != was)
    chosen_at = ticks;
  if (p & MX_PAD_CROSS)
    open_entry();
  else if (p & MX_PAD_CIRCLE)
    up_folder();
}

static void photo_input(void) {
  if (pad_pressed)
    shown_at = ticks;
  if (pad_pressed & (MX_PAD_RIGHT | MX_PAD_R1))
    photo_next(1);
  if (pad_pressed & (MX_PAD_LEFT | MX_PAD_L1))
    photo_next(-1);
  if (pad_pressed & MX_PAD_START && mu.state)
    view = V_MUSIC;
  if (pad_pressed & MX_PAD_CIRCLE) {
    image_free(&photo);
    view = V_BROWSE;
  }
}

static void video_input(void) {
  if (pad_pressed)
    shown_at = ticks;
  if (pad_pressed & MX_PAD_CROSS) {
    mv_paused = !mv_paused;
    mx_movie_pause(movie, (int)mv_paused);
  }
  if (pad_pressed & (MX_PAD_LEFT | MX_PAD_RIGHT | MX_PAD_L1 | MX_PAD_R1)) {
    s32 to = (s32)mx_movie_time_ms(movie) + (pad_pressed & (MX_PAD_RIGHT | MX_PAD_R1) ? 10000 : -10000);
    mx_movie_seek(movie, to < 0 ? 0 : (u32)to);
  }
  if (pad_pressed & MX_PAD_CIRCLE)
    video_close();
}

static void music_input(void) {
  if (pad_pressed & MX_PAD_CROSS && mu.path[0]) {
    if (mu.state)
      music_cmd(MU_PAUSE);
    else
      music_play_at(qpos);
  }
  if (pad_pressed & (MX_PAD_RIGHT | MX_PAD_R1))
    for (int i = qpos + 1; i < nqueue; i++)
      if (!ends_with(queue[i], ".mp3")) {
        music_play_at(i);
        break;
      }
  if (pad_pressed & (MX_PAD_LEFT | MX_PAD_L1)) {
    /* the start of this one after a few seconds, else the one before */
    int to = qpos;
    if (mu.pos_ms < 3000)
      for (int i = qpos - 1; i >= 0; i--)
        if (!ends_with(queue[i], ".mp3")) {
          to = i;
          break;
        }
    music_play_at(to);
  }
  if (pad_pressed & MX_PAD_SQUARE)
    music_stop();
  if (pad_pressed & MX_PAD_CIRCLE)
    view = V_BROWSE;
}

#ifdef MX_BIOS_CALLS
/* the standalone build on PCSX2: arguments from host:media.txt */
static char *file_args[12], file_buf[256];
static int args_from_host(char ***argv) {
  int fd = mx_fio_open("host:media.txt", MX_FIO_RDONLY), n = 0, argc = 1;
  if (fd < 0)
    return 1;
  n = mx_fio_read(fd, file_buf, sizeof file_buf - 1);
  mx_fio_close(fd);
  file_buf[n > 0 ? n : 0] = 0;
  file_args[0] = "media";
  for (char *s = file_buf; *s && argc < 12;) {
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

/* arguments: kind=photo|video|music the kind to start with; (tests)
   dev=<n> open device n, path=<dir> that folder on it, sel=<n> its entry
   n, open (open it), keys=<buttons> pressed one a second after that
   (r l x o s t: right, left, cross, circle, start, triangle; p: a pause of a
   second), shot=<frames> the picture to host:media.tga after that many
   frames, mode=<video mode> */
int main(int argc, char **argv) {
  cfg_load();
#ifdef MX_BIOS_CALLS
  if (argc <= 1)
    argc = args_from_host(&argv);
#endif
  int shot = 0, odev = -1, osel = 0, oopen = 0;
  const char *opath = 0, *okeys = 0, *odev_s = 0;
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
    else if (!strcmp(a, "open"))
      oopen = 1;
    else if (!strncmp(a, "keys=", 5))
      okeys = a + 5;
    else if (!strncmp(a, "mode=", 5))
      cfg_arg("video.mode", a + 5);
    else if (!strncmp(a, "kind=", 5))
      for (int k = 0; k < NKINDS; k++)
        if (!strcmp(a + 5, kind_key[k]))
          kind = k;
  }
  /* room above this thread for the movie's reader and the music (each
     starts at this one's priority less one): a standalone program starts
     at 0 */
  if (mx_thread_prio(mx_thread_self()) < 8)
    mx_thread_set_prio(mx_thread_self(), 64);
  ents = malloc(MAXE * sizeof *ents);
  if (!ents)
    return 1;
  mxgl_init();
  pad_on = mx_pad_init() >= 0;
  video_apply();
  devs_refresh();
  /* tests: dev=<n> the n-th device, or dev=<name> the one whose name starts so (dev=host) */
  if (odev_s)
    for (int d = 0; d < ndev; d++)
      if ((odev_s[0] >= '0' && odev_s[0] <= '9') ? d == atoi(odev_s) : !strncmp(devs[d].root, odev_s, strlen(odev_s)))
        odev = d;
  u32 keys_from = 0;

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
          chosen_at = ticks - 100;
          if (oopen)
            open_entry();
          keys_from = ticks;
        }
        odev = -1;
      }
    }
    /* tests: the buttons of keys=, one a second */
    if (okeys && *okeys && keys_from && ticks - keys_from >= 60) {
      static const struct {
        char c;
        u32 b;
      } km[] = {{'r', MX_PAD_RIGHT}, {'l', MX_PAD_LEFT}, {'x', MX_PAD_CROSS}, {'o', MX_PAD_CIRCLE},
                {'s', MX_PAD_START}, {'t', MX_PAD_TRIANGLE}, {'d', MX_PAD_DOWN}, {'u', MX_PAD_UP}};
      for (u32 i = 0; i < sizeof km / sizeof km[0]; i++)
        if (km[i].c == *okeys)
          pad_pressed = km[i].b;
      mx_printf("media: key %c\n", *okeys);
      okeys++;
      keys_from = ticks;
    }
    music_step();
    toast_step();
    switch (view) {
    case V_BROWSE: browse_input(), preview_step(); break;
    case V_PHOTO: photo_input(); break;
    case V_VIDEO: video_input(); break;
    case V_MUSIC: music_input(); break;
    }
    if (view == V_VIDEO)
      video_step();
    frame();
    if (shot && ticks == (u32)shot) {
      mu.hold = 1;
      mx_sleep_us(40000);
      int r = mxgl_screenshot("host:media.tga");
      mx_printf("media: shot %d; music %d at %u of %u ms\n", r, mu.state, mu.pos_ms, mu.len_ms);
      MX_TEST_END(r ? 1 : 0);
      return 0;
    }
  }
}
