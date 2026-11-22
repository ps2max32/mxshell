/* MXSHELL2: the games, from <device>/PS2MAX/<game>/game.ini (the format
   mxshell and its package-games.sh use: title, year, type, elf, args,
   description, cover; and saves, data), on the devices the kernel lists
   (devlist.c) but the memory cards. A game starts beside the shell
   (mx_proc_spawn); the shell's textures are given back first. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <mx/dev.h>
#include <mx/mem.h>
#include <mx/proc.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/debug.h>
#include <string.h>
#include <stdlib.h>

#include "pn.h"

struct game games[MAX_GAMES];
int ngames;
struct pdev gdevs[PDEV_MAX];
int ngdevs;
int gdev_games[PDEV_MAX];
#define dev_root(d) gdevs[d].root

static int game_dev[MAX_GAMES];

/* file in the game's directory; "../<dir>/<file>": in another one of
   PS2MAX */
void game_path(char *out, int len, int i, const char *file) {
  if (!strncmp(file, "../", 3))
    mx_snprintf(out, len, "%s/PS2MAX/%s", dev_root(game_dev[i]), file + 3);
  else
    mx_snprintf(out, len, "%s/PS2MAX/%s/%s", dev_root(game_dev[i]), games[i].dir, file);
}

static void set(char *d, u32 n, const char *s) {
  u32 i = 0;
  for (; s[i] && s[i] != '\r' && s[i] != '\n' && i + 1 < n; i++)
    d[i] = s[i];
  d[i] = 0;
}

static u32 title_colour(const char *t) {
  /* a dark, muted colour from the title: the tile without a cover */
  u32 h = 2166136261u;
  for (; *t; t++)
    h = (h ^ (u8)*t) * 16777619u;
  static const u32 tones[8] = {0x33271F, 0x351C19, 0x2A281C, 0x1B232E, 0x23202B, 0x1C2723, 0x29222C, 0x2C251C};
  return tones[h & 7];
}

static int read_ini(int dev, struct game *g) {
  /* the store's game.ini has its keys too (apps/mxstore docs/index-format.md) */
  static char buf[2048]; /* the shelf is read by one thread */
  char path[96];
  mx_snprintf(path, sizeof path, "%s/PS2MAX/%s/game.ini", dev_root(dev), g->dir);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    /* a directory with <directory>.elf is a game too */
    mx_snprintf(g->elf, sizeof g->elf, "%s.elf", g->dir);
    mx_snprintf(path, sizeof path, "%s/PS2MAX/%s/%s", dev_root(dev), g->dir, g->elf);
    fd = mx_fio_open(path, MX_FIO_RDONLY);
    if (fd < 0)
      return -1;
    mx_fio_close(fd);
    set(g->title, sizeof g->title, g->dir);
    return 0;
  }
  int n = mx_fio_read(fd, buf, sizeof buf - 1);
  mx_fio_close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  for (char *s = buf; *s;) {
    char *e = s;
    while (*e && *e != '\n')
      e++;
    char *eq = s;
    while (eq < e && *eq != '=')
      eq++;
    if (eq < e) {
      *eq = 0;
      const char *v = eq + 1;
      if (!strcmp(s, "title")) set(g->title, sizeof g->title, v);
      else if (!strcmp(s, "year")) set(g->year, sizeof g->year, v);
      else if (!strcmp(s, "type")) set(g->type, sizeof g->type, v);
      else if (!strcmp(s, "elf")) set(g->elf, sizeof g->elf, v);
      else if (!strcmp(s, "args")) set(g->args, sizeof g->args, v);
      else if (!strcmp(s, "description")) set(g->desc, sizeof g->desc, v);
      else if (!strcmp(s, "cover")) set(g->cover, sizeof g->cover, v);
      else if (!strcmp(s, "cover_hd")) set(g->cover_hd, sizeof g->cover_hd, v);
      else if (!strcmp(s, "saves")) set(g->saves, sizeof g->saves, v);
      else if (!strcmp(s, "data")) set(g->data, sizeof g->data, v);
      else if (!strcmp(s, "setup")) set(g->setup, sizeof g->setup, v);
      else if (!strcmp(s, "setup_args")) set(g->setup_args, sizeof g->setup_args, v);
      else if (!strcmp(s, "versions")) set(g->versions, sizeof g->versions, v);
      else if (!strcmp(s, "order")) g->order = (u8)atoi(v);
      else if (!strcmp(s, "section")) {
        char sec[16];
        set(sec, sizeof sec, v);
        if (!strcmp(sec, "system"))
          return -1; /* MXSHELL itself: not on the shelf */
        /* emulation: in Apps until it has its own section (pn.h) */
        g->sec = !strcmp(sec, "apps") || !strcmp(sec, "emulation") ? SEC_APPS : SEC_GAMES;
      }
    }
    s = *e ? e + 1 : e;
  }
  return g->title[0] && g->elf[0] ? 0 : -1;
}

static void scan(int dev, int dir) {
  char name[64];
  u32 size;
  while (ngames < MAX_GAMES && mx_fio_dread(dir, name, sizeof name, &size) > 0) {
    if (name[0] == '.')
      continue;
    struct game g;
    memset(&g, 0, sizeof g);
    set(g.dir, sizeof g.dir, name);
    if (read_ini(dev, &g) < 0)
      continue;
    g.base = title_colour(g.title);
    /* sorted by order=, then by title */
    int j = ngames++;
    for (; j > 0 && (games[j - 1].order > g.order ||
                     (games[j - 1].order == g.order && strcmp(games[j - 1].title, g.title) > 0));
         j--)
      games[j] = games[j - 1], game_dev[j] = game_dev[j - 1];
    games[j] = g, game_dev[j] = dev;
  }
}

static int step_dev;
int games_searching, host_only;

const char *game_root(int i) { return dev_root(game_dev[i]); }
int games_find(const char *root, const char *dir) {
  for (int i = 0; i < ngames; i++)
    if (!strcmp(game_root(i), root) && !strcmp(games[i].dir, dir))
      return i;
  return -1;
}

void games_search(void) {
  games_restart();
  games_searching = 1;
}

/* the devices again (the memory cards hold no games), the games forgotten */
void games_restart(void) {
  games_textures_free();
  ngames = 0;
  step_dev = 0;
  struct pdev all[PDEV_MAX];
  int n = host_only ? 0 : pdevs_read(all, PDEV_MAX - 1, 0);
  if (cfg_get("dev.host", "0")[0] == '1') {
    memset(&all[n], 0, sizeof all[n]);
    strcpy(all[n].root, "host:");
    all[n++].kind = MX_DEV_HOST;
  }
  ngdevs = 0;
  for (int i = 0; i < n; i++)
    if (all[i].kind != MX_DEV_MC)
      gdevs[ngdevs] = all[i], gdev_games[ngdevs++] = 0;
}

/* one device a step, so the frames go on */
int games_step(void) {
  if (step_dev >= ngdevs)
    return 1;
  int d = step_dev++;
  char path[24];
  mx_snprintf(path, sizeof path, "%s/PS2MAX", dev_root(d));
  int dir = mx_fio_dopen(path);
  if (dir >= 0) {
    int before = ngames;
    scan(d, dir);
    mx_fio_dclose(dir);
    gdev_games[d] = ngames - before;
  }
  return step_dev >= ngdevs;
}

/* A cover: a CLUT of 256 RGBA entries in the GS's CSM1 order (alpha 0x80),
   then the indices (mxshell's tools/mk-cover.py, MXSHELL2's
   tools/mk-covers.py): cover.t8 128 x 176 (mxshell's), cover_hd 256 x 361
   (a DVD case, 1 : 1.41).
   As a GL texture: the CLUT in its plain order, alpha 255, the rows in a
   texture of the next power of two high. */
static int cover_file(int i, const char *file, int cw, int ch) {
  char path[112];
  game_path(path, sizeof path, i, file);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("mxshell: %s: no cover (%d)\n", path, fd);
    return -1;
  }
  int th = 8;
  while (th < ch)
    th *= 2;
  u8 *buf = malloc(1024 + (u32)(cw * th));
  if (!buf) {
    mx_fio_close(fd);
    return -1;
  }
  u8 clut[1024];
  int n = mx_fio_read(fd, clut, sizeof clut);
  n += mx_fio_read(fd, buf + 1024, (u32)(cw * ch));
  mx_fio_close(fd);
  if (n != 1024 + cw * ch) {
    mx_printf("mxshell: %s: %d bytes, not a %d x %d cover\n", path, n, cw, ch);
    free(buf);
    return -1;
  }
  for (int k = 0; k < 256; k++) {
    int at = (k & ~0x18) | (k & 8) << 1 | (k & 0x10) >> 1;
    buf[4 * k] = clut[4 * at], buf[4 * k + 1] = clut[4 * at + 1], buf[4 * k + 2] = clut[4 * at + 2];
    buf[4 * k + 3] = 255;
  }
  memset(buf + 1024 + cw * ch, buf[1024 + cw * (ch - 1)], (u32)(cw * (th - ch)));
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, cw, th, 0, 1024 + cw * th, buf);
  free(buf);
  games[i].tex = (int)t;
  games[i].tex_v = (float)ch / (float)th;
  return 0;
}

static int cover_load(int i) {
  mx_printf("mxshell: %s: cover '%s', cover_hd '%s'\n", games[i].dir, games[i].cover, games[i].cover_hd);
  if (games[i].cover_hd[0] && cover_file(i, games[i].cover_hd, 256, 361) == 0)
    return 0;
  return games[i].cover[0] ? cover_file(i, games[i].cover, 128, 176) : -1;
}

void games_textures(void) {
  for (int i = 0; i < ngames; i++)
    if (!games[i].tex && (games[i].cover[0] || games[i].cover_hd[0]))
      cover_load(i);
}

void games_textures_free(void) {
  for (int i = 0; i < ngames; i++)
    if (games[i].tex) {
      GLuint t = (GLuint)games[i].tex;
      glDeleteTextures(1, &t);
      games[i].tex = 0;
    }
}

/* ---- the data's versions ---- */

/* the data directory's file f (data= relative to the game's directory) */
static void data_path(char *out, int len, int i, const char *f) {
  char rel[112];
  if (games[i].data[0])
    mx_snprintf(rel, sizeof rel, "%s/%s", games[i].data, f);
  else
    mx_snprintf(rel, sizeof rel, "%s", f);
  game_path(out, len, i, rel);
}

/* up to the next c or the end of s, into d (n bytes); s after it */
static const char *field(const char *s, char c, char *d, u32 n) {
  u32 k = 0;
  for (; *s && *s != c && *s != ','; s++)
    if (k + 1 < n)
      d[k++] = *s;
  d[k] = 0;
  return *s == c ? s + 1 : s;
}

int game_vers(int i, struct gver *v) {
  int n = 0;
  char path[160];
  for (const char *s = games[i].versions; *s && n < MAX_VERS;) {
    struct gver *g = &v[n];
    s = field(s, ':', g->name, sizeof g->name);
    s = field(s, ':', g->file, sizeof g->file);
    s = field(s, ',', g->word, sizeof g->word); /* and past its "," */
    if (!g->name[0] || !g->word[0])
      continue;
    g->there = 0;
    if (g->file[0]) {
      data_path(path, sizeof path, i, g->file);
      int fd = mx_fio_open(path, MX_FIO_RDONLY);
      if (fd >= 0)
        mx_fio_close(fd), g->there = 1;
    }
    n++;
  }
  return n;
}

int game_ver_there(const struct gver *v, int n) {
  int k = 0;
  for (int j = 0; j < n; j++)
    k += v[j].there;
  return k;
}

int game_ver_pick(int i, const struct gver *v, int n) {
  if (!n)
    return -1;
  /* the one chosen before: <game>/.version, its word */
  char path[112], w[20];
  game_path(path, sizeof path, i, ".version");
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd >= 0) {
    int r = mx_fio_read(fd, w, sizeof w - 1);
    mx_fio_close(fd);
    w[r > 0 ? r : 0] = 0;
    for (int k = 0; w[k]; k++)
      if (w[k] == '\r' || w[k] == '\n' || w[k] == ' ')
        w[k] = 0;
    for (int j = 0; j < n; j++)
      if (!strcmp(v[j].word, w) && (v[j].there || !game_ver_there(v, n)))
        return j;
  }
  int there = game_ver_there(v, n);
  if (there >= 2)
    return -1;
  for (int j = 0; j < n; j++)
    if (v[j].there)
      return j;
  return 0; /* none there: the first, and the game says what is missing */
}

int game_ver_next(int i, const struct gver *v, int n, int cur) {
  (void)i;
  for (int k = 1; k <= n; k++) {
    int j = (cur + k) % n;
    if (v[j].there)
      return j;
  }
  return cur;
}

void game_ver_save(int i, const char *word) {
  char path[112];
  game_path(path, sizeof path, i, ".version");
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("mxshell: %s not written (%d)\n", path, fd);
    return;
  }
  char line[20];
  int l = mx_snprintf(line, sizeof line, "%s\n", word);
  mx_fio_write(fd, line, l);
  mx_fio_close(fd);
}

/* the word {version} stands for: the chosen version's, else (not asked: a
   test's launch) the first that is there */
static const char *ver_word(int i) {
  static struct gver v[MAX_VERS];
  int n = game_vers(i, v);
  if (!n)
    return "";
  int j = game_ver_pick(i, v, n);
  if (j < 0)
    j = game_ver_next(i, v, n, n - 1);
  return v[j].word;
}

/* a program of the game's directory with its arguments ({data} and
   {version} replaced) */
static int spawn(int i, const char *elf, const char *argline) {
  static char args[256];
  game_path(args, 112, i, elf);
  char *e = args + strlen(args) + 1;
  int argc = 1;
  /* {data} in args=: the game's data directory as data= names it, relative
     to the game's directory, which it runs in ("../DATA/quake": one that
     games share); without data= its own, "." */
  const char *data = games[i].data[0] ? games[i].data : ".";
  u32 dl = (u32)strlen(data);
  const char *ver = strstr(argline, "{version}") ? ver_word(i) : "";
  u32 vl = (u32)strlen(ver);
  for (const char *s = argline; *s && e < args + sizeof args - 2;) {
    while (*s == ' ')
      s++;
    if (!*s)
      break;
    while (*s && *s != ' ' && e < args + sizeof args - 2) {
      if (!strncmp(s, "{data}", 6) && e + dl < args + sizeof args - 2) {
        memcpy(e, data, dl), e += dl, s += 6;
        continue;
      }
      if (!strncmp(s, "{version}", 9) && e + vl < args + sizeof args - 2) {
        memcpy(e, ver, vl), e += vl, s += 9;
        continue;
      }
      *e++ = *s++;
    }
    *e++ = 0;
    argc++;
  }
  /* in the game's directory (kernel version 40); the swap file is MXOS's own
     (the Storage page) */
  static char dir[112];
  game_path(dir, sizeof dir, i, "");
  return mx_proc_spawn_in(dir, argc, args, (u32)(e - args), 0);
}

int game_launch(int i) { return spawn(i, games[i].elf, games[i].args); }
int game_setup(int i) { return spawn(i, games[i].setup, games[i].setup_args); }
