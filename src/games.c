/* MXSHELL2: the games, from <device>/PS2MAX/<game>/game.ini (the format
   mxshell and its package-games.sh use: title, year, type, elf, args,
   description, cover; and saves). The storage drivers come from mc0:/MXOS,
   each loaded once. A game starts with mx_loader_launch: the shell waits
   meanwhile, and its textures are given back first. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <mx/iop.h>
#include <mx/mem.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/debug.h>
#include <string.h>
#include <stdlib.h>

#include "pn.h"

#define MXOS "mc0:/MXOS/"
#define TRIES 40 /* a device is looked for this many times, a quarter second apart */

struct game games[MAX_GAMES];
int ngames;
const char *const dev_root[NDEVS] = {"usb0:", "ata0:", "host:"};
const char *const dev_name[NDEVS] = {"USB", "HDD", "Host"};
int dev_found[NDEVS];

static int game_dev[MAX_GAMES];

void game_path(char *out, int len, int i, const char *file) {
  mx_snprintf(out, len, "%s/PS2MAX/%s/%s", dev_root[game_dev[i]], games[i].dir, file);
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
  char path[96], buf[768];
  mx_snprintf(path, sizeof path, "%s/PS2MAX/%s/game.ini", dev_root[dev], g->dir);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    /* a directory with <directory>.elf is a game too */
    mx_snprintf(g->elf, sizeof g->elf, "%s.elf", g->dir);
    mx_snprintf(path, sizeof path, "%s/PS2MAX/%s/%s", dev_root[dev], g->dir, g->elf);
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
      else if (!strcmp(s, "section")) {
        char sec[16];
        set(sec, sizeof sec, v);
        g->sec = !strcmp(sec, "media") ? SEC_MEDIA : !strcmp(sec, "emulation") ? SEC_EMU : !strcmp(sec, "apps") ? SEC_APPS : SEC_GAMES;
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
    /* sorted by title */
    int j = ngames++;
    for (; j > 0 && strcmp(games[j - 1].title, g.title) > 0; j--)
      games[j] = games[j - 1], game_dev[j] = game_dev[j - 1];
    games[j] = g, game_dev[j] = dev;
  }
}

static int step_dev, step_tries;
int games_searching;

void games_search(void) {
  games_restart();
  games_searching = 1;
}

void games_restart(void) {
  games_textures_free();
  ngames = 0;
  step_dev = step_tries = 0;
  for (int d = 0; d < NDEVS; d++)
    dev_found[d] = 0;
}

static int dev_wanted(int d) {
  static const char *const keys[NDEVS] = {"dev.usb", "dev.hdd", "dev.host"};
  return cfg_get(keys[d], d == DEV_HOST ? "0" : "1")[0] == '1';
}

int games_step(void) {
  while (step_dev < NDEVS) {
    int d = step_dev;
    if (!dev_wanted(d)) {
      step_dev++, step_tries = 0;
      continue;
    }
    if (step_tries == 0)
      if (dev_drivers(d) < 0) {
        /* without its drivers the device cannot come */
        mx_printf("mxshell: %s: no drivers\n", dev_root[d]);
        step_tries = TRIES;
      }
    if (step_tries >= TRIES) {
      step_dev++, step_tries = 0;
      continue;
    }
    char path[24];
    mx_snprintf(path, sizeof path, "%s/PS2MAX", dev_root[d]);
    int dir = mx_fio_dopen(path);
    if (dir < 0) {
      /* USB and the HDD come up a little after their drivers */
      if (d != DEV_HOST && ++step_tries < TRIES)
        return 0;
      step_dev++, step_tries = 0;
      continue;
    }
    dev_found[d] = 1;
    scan(d, dir);
    mx_fio_dclose(dir);
    step_dev++, step_tries = 0;
  }
  return 1;
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

int game_launch(int i) {
  static char args[256], swap[48];
  game_path(args, 112, i, games[i].elf);
  char *e = args + strlen(args) + 1;
  int argc = 1;
  for (const char *s = games[i].args; *s && e < args + sizeof args - 2;) {
    while (*s == ' ')
      s++;
    if (!*s)
      break;
    while (*s && *s != ' ' && e < args + sizeof args - 2)
      *e++ = *s++;
    *e++ = 0;
    argc++;
  }
  mx_snprintf(swap, sizeof swap, "%s/PS2MAX/swap.bin", dev_root[game_dev[i]]);
  int fd = mx_fio_open(swap, MX_FIO_RDONLY);
  if (fd >= 0)
    mx_fio_close(fd);
  return mx_loader_launch(argc, args, (u32)(e - args), fd >= 0 ? swap : 0);
}
