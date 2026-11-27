/* MXSTORE: the store of mx OS, an app of MXSHELL2 (its Apps section). The
   programs made with ps2max from the signed index on GitHub Pages
   (apps/mxstore docs/index-format.md): a shelf per section as the shell's
   Home, a page per program (its hero, its cover as a slab, the facts, the
   screenshots on the floor), and installing, updating and removing them on
   a USB drive or the hard disk. Penumbra as the shell: the shell's drawing,
   font, video mode and notifications (../src), a main loop of its own; all
   network and file work on the worker thread (fetch.c).

   The cover on its shelf is the file an install writes as the program's
   coverhd.t8, and the text its game.ini: the store and the shell's shelf
   show the same.

   Arguments (tests): base=<url or dir/> the site (host:site/ reads a
   directory: packages and pictures with relative URLs come from there
   too), dev=host the PC as a device to install to (and use first),
   dev=hostonly the PC alone (tests that write: no USB drive is touched),
   sec=<n> the section at the start, open=<id> its page, install=<id>
   install it and end (status 0 when it went in), data=<id> the same for
   an installed game's data (its data entry), remove=<id> the same for
   removing, shot=<frames> the picture to host:store.tga, mode=<video mode> */
#include <GL/gl.h>
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/setting.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <stdlib.h>
#include <string.h>

#include "https.h"
#include "mxgl.h"
#include "pn.h"
#include "store.h"

MX_CORE_HEAP(512 << 10);

u32 pad_pressed, pad_held;
int screen;
int pad_on; /* pn.h */

/* ---- the sections ---- */
/* as the shell's (src/pn.h): emulation is in Apps until the shell has a
   section for it */
enum { SS_GAMES, SS_APPS, SS_SYSTEM, SS_UPDATES, NSS };
static const char *const ss_name[NSS] = {"Games", "Apps", "System", "Updates"};
static int sec, focus_of[NSS], focus;
static struct spring fpos, shift;

static int has_update(const struct entry *e) {
  return e->on_dev >= 0 && e->on_version[0] && (e->partial || strcmp(e->on_version, e->version));
}
static int in_section(const struct entry *e, int s) {
  if (e->kind == K_DATA)
    return 0; /* on its game's page */
  switch (s) {
  case SS_SYSTEM: return e->kind == K_SYSTEM;
  case SS_UPDATES: return has_update(e);
  case SS_APPS:
    return e->kind != K_SYSTEM &&
           (!strcmp(e->section, "apps") || !strcmp(e->section, "emulation") || !strcmp(e->section, "media"));
  default:
    return e->kind != K_SYSTEM && strcmp(e->section, "emulation") && strcmp(e->section, "apps") &&
           strcmp(e->section, "media");
  }
}
static int row[MAX_ENTRIES], nrow;
static void row_make(void) {
  nrow = 0;
  if (index_state == IX_READY)
    for (int i = 0; i < nentries; i++)
      if (in_section(&entries[i], sec))
        row[nrow++] = i;
  if (focus >= nrow)
    focus = nrow ? nrow - 1 : 0;
}
static int nupdates(void) {
  int n = 0;
  for (int i = 0; index_state == IX_READY && i < nentries; i++)
    n += has_update(&entries[i]);
  return n;
}

/* ---- small things ---- */
static void fmt_size(char *o, int n, u64 b) {
  if (b < 1024)
    mx_snprintf(o, n, "%u B", (u32)b);
  else if (b < (1u << 20))
    mx_snprintf(o, n, "%u KB", (u32)(b >> 10));
  else if (b < (1ull << 30))
    mx_snprintf(o, n, "%u.%u MB", (u32)(b >> 20), (u32)((b & 0xFFFFF) * 10 >> 20));
  else
    mx_snprintf(o, n, "%u.%u GB", (u32)(b >> 30), (u32)((b & 0x3FFFFFFF) * 10 >> 30));
}
static const char *kind_name(const struct entry *e) {
  static const char *const k[] = {"Game", "Emulator", "App", "System"};
  return k[e->kind];
}
/* text in lines of w, at most max lines: the y after it */
static float para(int size, float x, float y, float w, const char *s, u32 c, int max) {
  char line[256];
  float lh = text_line(size) * 1.25f;
  int lines = 0;
  while (*s && lines < max) {
    int n = 0, cut = 0;
    while (s[n] && n < (int)sizeof line - 1) {
      line[n] = s[n];
      line[n + 1] = 0;
      if (d_text_w(size, line) > w)
        break;
      if (s[n] == ' ')
        cut = n;
      n++;
    }
    if (!s[n])
      cut = n;
    else if (!cut)
      cut = n;
    memcpy(line, s, (u32)cut);
    line[cut] = 0;
    if (lines == max - 1 && s[cut]) {
      /* the last line: cut with an ellipsis */
      int l = cut;
      while (l > 0 && d_text_w(size, line) + d_text_w(size, "...") > w)
        line[--l] = 0;
      strcat(line, "...");
    }
    d_text(size, x, y, line, c);
    y += lh, lines++;
    s += cut;
    while (*s == ' ')
      s++;
  }
  return y;
}
/* ---- the devices: the kernel's, without the memory cards (programs go
   to a USB drive or the HDD). With a system folder (the setting
   system.root, kernel 47: "usb0:/PS2MAX") programs go under it, on its
   device alone ---- */
static void dev_base(struct sdev *d, const char *sysroot) {
  u32 l = (u32)strlen(d->root);
  if (sysroot[0] && !strncmp(sysroot, d->root, l) && sysroot[l] == '/')
    mx_snprintf(d->base, sizeof d->base, "%s", sysroot);
  else
    mx_snprintf(d->base, sizeof d->base, "%s/PS2MAX", d->root);
  u32 n = (u32)strlen(d->base);
  while (n > 1 && d->base[n - 1] == '/')
    d->base[--n] = 0;
}
static int with_host, store_host_only;
static u32 devs_gen = ~0u;
static void devs_refresh(void) {
  if (pdevs_gen() == devs_gen && nsdevs)
    return;
  devs_gen = pdevs_gen();
  char keep[12];
  mx_snprintf(keep, sizeof keep, "%s", cur_dev >= 0 ? sdevs[cur_dev].root : "");
  struct pdev all[PDEV_MAX];
  int n = pdevs_read(all, PDEV_MAX, with_host), k = 0;
  char sysroot[48];
  if (mx_setting_get("system.root", sysroot, sizeof sysroot) <= 0)
    sysroot[0] = 0;
  if (!strchr(sysroot, ':'))
    sysroot[0] = 0; /* not a path with a device: none */
  u32 rl = (u32)strcspn(sysroot, ":") + 1; /* "usb0:" */
  /* host: first when asked for (tests) */
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n && k < SDEV_MAX; i++) {
      if (all[i].kind == MX_DEV_MC || (all[i].kind == MX_DEV_HOST) != (pass == 0) ||
          (store_host_only && all[i].kind != MX_DEV_HOST))
        continue;
      /* a system folder: only its device (host: is for tests) */
      if (sysroot[0] && all[i].kind != MX_DEV_HOST && (strlen(all[i].root) != rl || strncmp(all[i].root, sysroot, rl)))
        continue;
      mx_snprintf(sdevs[k].root, sizeof sdevs[k].root, "%s", all[i].root);
      dev_base(&sdevs[k], sysroot);
      mx_snprintf(sdevs[k].name, sizeof sdevs[k].name, "%s", pdev_name(&all[i]));
      sdevs[k].free = all[i].free;
      sdevs[k].writable = !(all[i].flags & MX_DEV_READONLY);
      k++;
    }
  nsdevs = k;
  cur_dev = nsdevs ? 0 : -1;
  for (int i = 0; i < nsdevs; i++)
    if (!strcmp(sdevs[i].root, keep))
      cur_dev = i;
  if (index_state == IX_READY && !worker_busy())
    worker_job(J_SCAN, 0, 0, 0);
}

/* ---- pictures as textures ---- */
#define TILE 256
static u32 tile_buf[TILE * TILE] MX_ALIGNED(64);
static void tex_free(struct picture *p) {
  for (int i = 0; i < 9; i++)
    if (p->tex[i]) {
      GLuint t = (GLuint)p->tex[i];
      glDeleteTextures(1, &t);
      p->tex[i] = 0;
    }
}
static void cover_tex(struct picture *p) {
  /* as the shell's games.c: the CLUT in its plain order, the rows in a
     texture of 512 */
  const int cw = 256, ch = 361, th = 512;
  u8 *buf = malloc(1024 + (u32)(cw * th));
  if (buf) {
    const u8 *clut = p->data;
    for (int k = 0; k < 256; k++) {
      int at = (k & ~0x18) | (k & 8) << 1 | (k & 0x10) >> 1;
      buf[4 * k] = clut[4 * at], buf[4 * k + 1] = clut[4 * at + 1], buf[4 * k + 2] = clut[4 * at + 2];
      buf[4 * k + 3] = 255;
    }
    memcpy(buf + 1024, p->data + 1024, (u32)(cw * ch));
    memset(buf + 1024 + cw * ch, buf[1024 + cw * (ch - 1)], (u32)(cw * (th - ch)));
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, cw, th, 0, 1024 + cw * th, buf);
    free(buf);
    p->tex[0] = (int)t;
  }
}
/* pixels in tiles of 256 (Media's way: libgl's cache takes them one at a
   time), each tile's texels past the picture its edge again */
static void image_tex(struct picture *p) {
  const u32 *px = (const u32 *)p->data;
  p->tw = p->w, p->th = p->h;
  p->nx = (u8)((p->w + TILE - 1) / TILE), p->ny = (u8)((p->h + TILE - 1) / TILE);
  for (int j = 0; j < p->ny; j++)
    for (int i = 0; i < p->nx && j * p->nx + i < 9; i++) {
      u32 x0 = (u32)i * TILE, y0 = (u32)j * TILE;
      u32 cw = p->tw - x0 < TILE ? p->tw - x0 : TILE, ch = p->th - y0 < TILE ? p->th - y0 : TILE;
      u32 ew = cw < TILE ? cw + 1 : TILE, eh = ch < TILE ? ch + 1 : TILE;
      for (u32 y = 0; y < eh; y++) {
        const u32 *s = px + (y0 + (y < ch ? y : ch - 1)) * p->tw + x0;
        u32 *d = tile_buf + y * ew;
        memcpy(d, s, cw * 4);
        if (ew > cw)
          d[cw] = s[cw - 1];
      }
      u32 W = 8, H = 8;
      while (W < ew)
        W <<= 1;
      while (H < eh)
        H <<= 1;
      GLuint name;
      glGenTextures(1, &name);
      glBindTexture(GL_TEXTURE_2D, name);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      /* a texel that is not grey first: libgl keeps a grey texture as
         brightness alone (media.c) */
      static u32 blank[TILE * TILE] MX_ALIGNED(64);
      blank[0] = 0xFF000100u;
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)W, (GLsizei)H, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)ew, (GLsizei)eh, GL_RGBA, GL_UNSIGNED_BYTE, tile_buf);
      p->tex[j * p->nx + i] = (int)name;
    }
}
/* a picture the worker made ready: into textures, its bytes given back */
static void pic_take(struct picture *p) {
  if (p->state != PIC_READY)
    return;
  if (p->cover)
    cover_tex(p);
  else
    image_tex(p);
  free(p->data);
  p->data = 0;
  p->state = PIC_DONE;
}
static void d_image(const struct picture *p, float x0, float y0, float x1, float y1, float a) {
  if (p->state != PIC_DONE || p->cover)
    return;
  glEnable(GL_TEXTURE_2D);
  glColor4f(1, 1, 1, a * ui_fade);
  float kx = (x1 - x0) / p->tw, ky = (y1 - y0) / p->th;
  for (int j = 0; j < p->ny; j++)
    for (int i = 0; i < p->nx; i++) {
      u32 tx0 = (u32)i * TILE, ty0 = (u32)j * TILE;
      u32 cw = p->tw - tx0 < TILE ? p->tw - tx0 : TILE, ch = p->th - ty0 < TILE ? p->th - ty0 : TILE;
      u32 ew = cw < TILE ? cw + 1 : TILE, eh = ch < TILE ? ch + 1 : TILE, W = 8, H = 8;
      while (W < ew)
        W <<= 1;
      while (H < eh)
        H <<= 1;
      float u = (float)cw / W, v = (float)ch / H;
      float qx0 = x0 + tx0 * kx, qy0 = y0 + ty0 * ky, qx1 = qx0 + cw * kx, qy1 = qy0 + ch * ky;
      glBindTexture(GL_TEXTURE_2D, (GLuint)p->tex[j * p->nx + i]);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0), glVertex2f(qx0, qy0);
      glTexCoord2f(u, 0), glVertex2f(qx1, qy0);
      glTexCoord2f(u, v), glVertex2f(qx1, qy1);
      glTexCoord2f(0, v), glVertex2f(qx0, qy1);
      glEnd();
    }
  glDisable(GL_TEXTURE_2D);
}
static void pictures_step(void) {
  for (int i = 0; i < nentries && index_state == IX_READY; i++) {
    pic_take(&covers[i]);
    for (int k = 0; k < PAGE_PICS; k++)
      pic_take(&pages[i][k]);
  }
  pic_take(&view_pic);
}

/* ---- the screens ---- */
enum { S_SHELF, S_PAGE, S_VIEW, S_INSTALL, S_CONFIRM };
#define J_DATA (-1) /* start_job: a game's data entry */
static int page = -1, shot_sel, job_entry = -1, job_kind, confirm_replace;
static int job_show = -1; /* whose pictures the install shows: a data entry's game */
static int need_missing; /* the page's program: needs= files not there */
/* the page's game's data versions: there on its device (or the one it
   installs to), and the one chosen with up and down (-1: the program's
   own button) */
static u8 ver_there[MAX_VERS];
static int ver_sel = -1;
static void versions_look(const struct entry *en) {
  int d = en->on_dev >= 0 ? en->on_dev : cur_dev;
  for (int i = 0; i < en->nvers; i++)
    ver_there[i] = d >= 0 && data_there(en, d, en->vers[i].file);
}
static struct spring page_in;

/* hero 512 x 288 (four tiles), the shots 240 x 135 (one each): nine tiles a
   frame at most (media.c: what fits beside 1080i's frame buffer) */
static void want(struct picture *p, const struct pic_ref *ref, u16 w, u16 h) {
  if (p->state != PIC_NONE && p->state != PIC_FAILED)
    return;
  if (!ref->path)
    return;
  p->ref = ref, p->cover = 0, p->w = w, p->h = h;
  p->state = PIC_WANT;
}
static void page_open(int e) {
  /* the last page's textures back; one the worker is making stays */
  if (page >= 0 && page != e)
    for (int k = 0; k < PAGE_PICS; k++)
      if (pages[page][k].state == PIC_DONE) {
        tex_free(&pages[page][k]);
        pages[page][k].state = PIC_NONE;
      }
  page = e, shot_sel = 0;
  struct entry *en = &entries[e];
  want(&pages[e][0], &en->hero, 512, 288);
  for (int k = 0; k < en->nshots; k++)
    want(&pages[e][1 + k], &en->shots[k], 240, 135);
  worker_poke();
  int d = en->on_dev >= 0 ? en->on_dev : cur_dev;
  need_missing = d >= 0 && en->needs[0] && !needs_there(en, d);
  versions_look(en);
  if (ver_sel >= en->nvers)
    ver_sel = -1;
  page_in.x = 0, page_in.v = 0;
  screen = S_PAGE;
}

/* what the page's lit button does */
enum { A_NONE, A_INSTALL, A_UPDATE, A_REPAIR, A_REPLACE };
static const char *why_not; /* the reason there is no button */
static int page_action(const struct entry *e) {
  why_not = 0;
  if (e->update != U_NONE) {
    why_not = "MXBOOT and MXKERNEL are installed from the PC (mxboot-install) for now";
    return A_NONE;
  }
  if (e->kernel > kernel_version()) {
    static char m[64];
    mx_snprintf(m, sizeof m, "Needs a newer system: kernel %u (this one is %u)", e->kernel, kernel_version());
    why_not = m;
    return A_NONE;
  }
  if (e->on_dev >= 0) {
    if (e->partial)
      return A_REPAIR;
    if (!e->on_version[0])
      return A_REPLACE; /* put there by hand: only after asking */
    if (strcmp(e->on_version, e->version))
      return A_UPDATE;
    return A_NONE;
  }
  if (cur_dev < 0) {
    why_not = "No USB drive or hard disk to install to";
    return A_NONE;
  }
  if (!sdevs[cur_dev].writable) {
    why_not = "The device cannot be written";
    return A_NONE;
  }
  u64 need = (u64)(e->installed ? e->installed : e->size) + e->size;
  if (sdevs[cur_dev].free && sdevs[cur_dev].free < need) {
    why_not = "Not enough free space on the device";
    return A_NONE;
  }
  return A_INSTALL;
}

/* can data version v of the page's game be got now: 1, else 0 with why in
   *why (0: no reason to show) */
static int version_get(const struct entry *e, int v, const char **why) {
  *why = 0;
  const struct version *ver = &e->vers[v];
  if (ver_there[v] || ver->data < 0)
    return 0;
  if (e->on_dev < 0) {
    *why = "After the install";
    return 0;
  }
  const struct entry *de = &entries[ver->data];
  if (!sdevs[e->on_dev].writable) {
    *why = "The device cannot be written";
    return 0;
  }
  if (sdevs[e->on_dev].free && sdevs[e->on_dev].free < (u64)de->installed + de->size) {
    *why = "Not enough free space on the device for it";
    return 0;
  }
  return 1;
}

static int job_data = -1; /* J_DATA: the data entry */
static void start_job(int kind, int e, int replace) {
  if (worker_busy())
    return;
  int dev = kind == J_INSTALL && entries[e].on_dev >= 0 ? entries[e].on_dev : cur_dev;
  job_show = e;
  if (kind == J_DATA) {
    /* a version of the game's data: its data entry, to the game's device */
    dev = entries[e].on_dev;
    e = job_data;
    kind = J_INSTALL;
  }
  job_entry = e, job_kind = kind;
  worker_job(kind, e, dev, replace);
  screen = S_INSTALL;
}

/* ---- the shelf (as the shell's Home) ---- */
#define BIG_W 236.0f
#define BIG_H 333.0f
#define SMALL_W 162.0f
#define SMALL_H 228.0f
#define GAP 28.0f
static float lit_of(int i) {
  float d = fpos.x - (float)i;
  return clampf(1.0f - (d < 0 ? -d : d), 0, 1);
}
static void layout(int i, struct tile_look *t) {
  float x = MARGIN - shift.x;
  for (int k = 0; k < i; k++)
    x += SMALL_W + (BIG_W - SMALL_W) * lit_of(k) + GAP;
  float l = lit_of(i);
  memset(t, 0, sizeof *t);
  t->w = SMALL_W + (BIG_W - SMALL_W) * l;
  t->h = SMALL_H + (BIG_H - SMALL_H) * l;
  t->x = x, t->y = FLOOR_Y - t->h;
  t->turn = ((float)i < fpos.x ? 16.0f : -16.0f) * (1 - l);
  t->lit = l;
  t->fade = clampf(1.0f - (x + t->w - (SCR_W - MARGIN * 0.5f)) / 120.0f, 0, 1);
  if (i < nrow) {
    int e = row[i];
    t->base = entries[e].base;
    if (covers[e].state == PIC_DONE)
      t->tex = covers[e].tex[0], t->tex_u = 1, t->tex_v = 361.0f / 512.0f;
  } else
    t->base = 0x23262C;
}
static void tile_text(int i, const struct tile_look *t) {
  if (t->fade < 0.05f || t->tex || i >= nrow)
    return;
  const struct entry *e = &entries[row[i]];
  float a = t->fade * (0.45f + 0.55f * t->lit);
  int size = t->lit > 0.5f ? FONT_TITLE : FONT_TILE;
  float pad = 14 + 2 * t->lit;
  d_text_fit(size, FONT_BODY, t->x + pad, t->y + pad + (t->lit > 0.5f ? 26 : 20), t->w - 2 * pad, e->title,
             argb(C_WHITE, a));
  d_label(t->x + 16, t->y + t->h - 16, e->type[0] ? e->type : kind_name(e), argb(C_WHITE, 0.7f * a));
}
static const char *state_of(const struct entry *e, char *buf, int n) {
  if (e->on_dev >= 0) {
    if (e->partial)
      return "Not finished";
    if (has_update(e)) {
      mx_snprintf(buf, n, "Update %s", e->version);
      return buf;
    }
    mx_snprintf(buf, n, "%s" DOT "%.*s", e->on_version[0] ? "Installed" : "There by hand",
                (int)strlen(sdevs[e->on_dev].root) - 1, sdevs[e->on_dev].root);
    return buf;
  }
  return e->kind == K_SYSTEM ? "mx OS" : "In the store";
}
static void caption(float x) {
  float y = 488;
  if (x > SCR_W - MARGIN - 440)
    x = SCR_W - MARGIN - 440;
  if (focus >= nrow) {
    const char *t = index_state == IX_READY ? (sec == SS_UPDATES ? "Everything is up to date" : "Nothing here yet")
                                            : index_state == IX_FAILED ? index_title : "Looking";
    d_label(MARGIN, y, ss_name[sec], argb(C_LAMP, 1));
    d_text_fit(FONT_DISP, FONT_TITLE, MARGIN - 2, y + 54, SCR_W - 2 * MARGIN, t, argb(C_WHITE, 1));
    d_text_fit(FONT_BODY, FONT_SMALL, MARGIN, y + 86, SCR_W - 2 * MARGIN,
               index_state == IX_READY ? "" : index_message, argb(index_state == IX_FAILED ? C_WARN : C_DIM, 1));
    if (index_state == IX_FAILED)
      d_button(MARGIN, y + 104, SYM_CROSS, "Try again", 1);
    return;
  }
  const struct entry *e = &entries[row[focus]];
  char st[48], meta[128], sz[16];
  fmt_size(sz, sizeof sz, e->size);
  d_label(x, y, state_of(e, st, sizeof st), argb(has_update(e) ? C_LAMP : C_LAMP, 1));
  d_text_fit(FONT_DISP, FONT_TITLE, x - 2, y + 54, SCR_W - MARGIN - x, e->title, argb(C_WHITE, 1));
  mx_snprintf(meta, sizeof meta, "%s%s%s%s%s", e->type, e->type[0] && e->year[0] ? DOT : "", e->year,
              DOT, sz);
  d_text_fit(FONT_BODY, FONT_SMALL, x, y + 86, SCR_W - MARGIN - x, meta, argb(C_DIM, 1));
  d_button(x, y + 104, SYM_CROSS, "Details", 1);
}
static void header(void) {
  float x = d_text(FONT_TITLE, MARGIN - 2, 80, "Store", argb(C_WHITE, 1)) + 34;
  for (int s = 0; s < NSS; s++) {
    char n[24];
    int u = s == SS_UPDATES ? nupdates() : 0;
    if (u)
      mx_snprintf(n, sizeof n, "%s %d", ss_name[s], u);
    else
      mx_snprintf(n, sizeof n, "%s", ss_name[s]);
    u32 c = s == sec ? C_LAMP : C_DIM;
    float w = d_label_w(n);
    d_label(x, 76, n, argb(c, s == sec ? 1 : 0.8f));
    if (s == sec)
      d_rect(x, 86, x + w, 88, argb(C_LAMP, 0.9f), argb(C_LAMP, 0.9f));
    x += w + 26;
  }
  /* where it installs to, and the index */
  char r[96], fr[16];
  if (cur_dev >= 0) {
    fmt_size(fr, sizeof fr, sdevs[cur_dev].free);
    mx_snprintf(r, sizeof r, "%s%s%s", sdevs[cur_dev].name, sdevs[cur_dev].free ? DOT : "",
                sdevs[cur_dev].free ? fr : "");
  } else
    mx_snprintf(r, sizeof r, "No USB drive");
  if (strlen(r) < 40 && SCR_W > 1100 && index_state == IX_READY)
    mx_snprintf(r + strlen(r), sizeof r - strlen(r), DOT "index %.10s", index_generated);
  float w = d_text_w(FONT_SMALL, r);
  d_text(FONT_SMALL, SCR_W - MARGIN - w, 80, r, argb(C_FAINT, 1));
}
/* the job going on beside, top right under the header */
static void job_strip(void) {
  if (!worker_busy() || job_entry < 0 || screen == S_INSTALL || job.kind == J_INDEX || job.kind == J_SCAN)
    return;
  float x0 = SCR_W - MARGIN - 300, y0 = 100;
  struct surface s = {x0, y0, x0 + 300, y0 + 52, 8, C_SLAB, -1, argb(C_LAMP, 0.3f), 0.4f, 0.96f, 0, 0, 0, 0, 0};
  surface_draw(&s);
  char t[80];
  mx_snprintf(t, sizeof t, "%s %s", job.phase == PH_DOWNLOAD ? "Loading" : "Installing", entries[job_entry].title);
  d_text_fit(FONT_SMALL, FONT_LABEL, x0 + 16, y0 + 22, 268, t, argb(C_WHITE, 1));
  float k = job.phase == PH_DOWNLOAD ? (job.total ? (float)job.done / job.total : 0)
                                     : job.nfiles ? (float)job.files / job.nfiles : 0;
  d_bar(x0 + 16, y0 + 36, x0 + 284, y0 + 38, k, C_LAMP, 1);
}

static void shelf_draw(void) {
  row_make();
  spring_step(&fpos, (float)focus, W_FOCUS, frame_dt);
  int visible = (int)((SCR_W - 2 * MARGIN - BIG_W) / (SMALL_W + GAP)) + 1;
  int over = focus - (visible - 2);
  spring_step(&shift, over > 0 ? (float)over * (SMALL_W + GAP) : 0.0f, W_FOCUS, frame_dt);
  struct tile_look t;
  float lxp = 0, wsum = 0, fx = MARGIN;
  for (int i = 0; i < nrow; i++) {
    layout(i, &t);
    lxp += (t.x + t.w * 0.5f) * t.lit, wsum += t.lit;
    if (t.lit > 0.5f)
      fx = t.x;
  }
  lxp = wsum > 0 ? lxp / wsum : MARGIN + BIG_W / 2;
  room_begin(FLOOR_Y);
  room_light(lxp, 0, 340, FLOOR_Y + 50, 0.28f);
  room_light(lxp, FLOOR_Y + 2, 300, 44, 0.34f);
  for (int i = 0; i < nrow; i++) {
    layout(i, &t);
    if (t.fade > 0.06f)
      room_shadow(t.x + t.w * 0.5f, FLOOR_Y + 4, t.w * 0.62f, 20 + 12 * t.lit, (0.35f + 0.3f * t.lit) * t.fade);
  }
  d_begin_2d();
  room_draw();
  d_begin_3d();
  light_at(lxp, 0);
  for (int i = 0; i < nrow; i++) {
    layout(i, &t);
    if (t.fade > 0.06f)
      d_tile_reflection(&t);
  }
  for (int i = 0; i < nrow; i++)
    if (i != focus) {
      layout(i, &t);
      if (t.fade > 0.06f)
        d_tile(&t);
    }
  if (focus < nrow) {
    layout(focus, &t);
    d_tile(&t);
  }
  d_begin_2d();
  for (int i = 0; i < nrow; i++) {
    layout(i, &t);
    tile_text(i, &t);
  }
  header();
  caption(fx);
  static const char *const h[] = {SYM_CROSS, "Details", "L1", "Section", "R1", "Section", SYM_CIRCLE, "Close",
                                  0};
  static const char *const h_fail[] = {SYM_CROSS, "Try again", SYM_CIRCLE, "Close", 0};
  hints_draw(index_state == IX_FAILED ? h_fail : h);
}

static void shelf_input(void) {
  row_make();
  if (pad_pressed & MX_PAD_LEFT && focus > 0)
    focus--;
  if (pad_pressed & MX_PAD_RIGHT && focus + 1 < nrow)
    focus++;
  int ds = (pad_pressed & MX_PAD_R1 ? 1 : 0) - (pad_pressed & MX_PAD_L1 ? 1 : 0);
  if (ds && sec + ds >= 0 && sec + ds < NSS) {
    focus_of[sec] = focus;
    sec += ds, focus = focus_of[sec];
    row_make();
    fpos.x = (float)focus, fpos.v = 0, shift.x = 0, shift.v = 0;
  }
  if (pad_pressed & MX_PAD_CROSS) {
    if (focus < nrow)
      page_open(row[focus]);
    else if (index_state == IX_FAILED && !worker_busy())
      worker_job(J_INDEX, 0, 0, 0);
  }
}

/* ---- a program's page (the mockup's "Page") ---- */
static float facts_x(void) { return SCR_W > 1100 ? SCR_W - MARGIN - 206 : -1; }
static void page_draw(void) {
  const struct entry *e = &entries[page];
  spring_step(&page_in, 1, W_LAYER, frame_dt);
  float k = page_in.x;
  room_begin(-1);
  room_light(260, 220, 520, 360, 0.08f);
  d_begin_2d();
  room_draw();
  /* the hero behind it all, in half-shadow */
  /* across the whole width (16:9; at 4:3 its sides cut), fading into the
     floor from 300 down */
  float hh = 520, iw = SCR_W, ih = SCR_W * 9 / 16;
  d_image(&pages[page][0], 0, 0, iw, ih, 0.9f * k);
  d_rect_h(0, 0, SCR_W * 0.5f, hh, argb(C_INK, 0.92f), argb(C_INK, 0.55f));
  d_rect_h(SCR_W * 0.5f, 0, SCR_W, hh, argb(C_INK, 0.55f), argb(C_INK, 0.2f));
  d_rect(0, 300, SCR_W, hh, argb(C_INK, 0), argb(C_INK, 1));
  d_rect(0, hh, SCR_W, SCR_H, argb(C_INK, 1), argb(C_INK, 1));

  /* the cover as a slab, its edge lit */
  struct tile_look slab;
  memset(&slab, 0, sizeof slab);
  slab.x = MARGIN, slab.y = 96, slab.w = 204, slab.h = 288, slab.turn = 10 * (1 - k), slab.lit = 1, slab.fade = k;
  slab.base = e->base;
  if (covers[page].state == PIC_DONE)
    slab.tex = covers[page].tex[0], slab.tex_u = 1, slab.tex_v = 361.0f / 512.0f;
  d_begin_3d();
  light_at(slab.x + 100, 0);
  d_tile(&slab);
  d_begin_2d();
  if (!slab.tex) {
    d_label(slab.x + 16, slab.y + 24, kind_name(e), argb(C_WHITE, 0.55f));
    d_text_fit(FONT_TITLE, FONT_BODY, slab.x + 16, slab.y + slab.h - 22, slab.w - 32, e->title, argb(C_WHITE, 1));
  }

  /* what it is */
  float fxr = facts_x();
  float x = MARGIN + 256, w = (fxr > 0 ? fxr - 40 : SCR_W - MARGIN) - x, y = 116;
  char crumb[48];
  mx_snprintf(crumb, sizeof crumb, "Store" DOT "%s", ss_name[sec]);
  d_label(x, y, crumb, argb(C_DIM, 1));
  d_text_fit(FONT_DISP, FONT_TITLE, x - 2, y + 52, w, e->title, argb(C_WHITE, 1));
  char facts[128];
  mx_snprintf(facts, sizeof facts, "%s", e->type);
  if (e->year[0])
    mx_snprintf(facts + strlen(facts), sizeof facts - strlen(facts), "%s%s", facts[0] ? DOT : "", e->year);
  if (e->developer[0])
    mx_snprintf(facts + strlen(facts), sizeof facts - strlen(facts), "%s%s", facts[0] ? DOT : "", e->developer);
  if (e->players[0])
    mx_snprintf(facts + strlen(facts), sizeof facts - strlen(facts), "%s%s players", facts[0] ? DOT : "",
                e->players);
  d_text_fit(FONT_BODY, FONT_SMALL, x, y + 84, w, facts, argb(C_DIM, 1));
  y = para(FONT_BODY, x, y + 124, w, e->description, argb(C_WHITE, 0.88f), fxr > 0 ? 5 : 4);
  if (e->needs[0] && !e->nvers && (need_missing || e->on_dev < 0)) {
    char m[240];
    mx_snprintf(m, sizeof m, "%s%s", e->needs_text[0] ? e->needs_text : "Bring your own game data.",
                need_missing && e->on_dev >= 0 ? " Not there yet." : "");
    /* one or two lines */
    int two = d_text_w(FONT_SMALL, m) > w - 32;
    float bh = two ? 72 : 46;
    struct surface s = {x, y, x + w, y + bh, 6, 0, 8, argb(C_WHITE, 0.1f), 0, 1, 0, 0, 0, 0, 0};
    surface_draw(&s);
    if (two)
      para(FONT_SMALL, x + 16, y + 29, w - 32, m, argb(C_WHITE, 0.85f), 2);
    else
      d_text(FONT_SMALL, x + 16, y + 29, m, argb(C_WHITE, 0.85f));
    y += bh + 12;
  }

  /* its data's versions: one a line, up and down choose one */
  int busy = worker_busy() && job_entry >= 0;
  if (e->nvers) {
    d_label(x, y + 6, "Its data", argb(C_FAINT, 1));
    y += 22;
    for (int i = 0; i < e->nvers; i++) {
      const struct version *ver = &e->vers[i];
      int lit = i == ver_sel;
      struct surface s = {x, y, x + w, y + 44, 6, 0, 8, argb(lit ? C_LAMP : C_WHITE, lit ? 0.35f : 0.08f), 0, 1, 0, 0, 0,
                          0, 0};
      surface_draw(&s);
      d_text_fit(FONT_BODY, FONT_SMALL, x + 16, y + 30, w * 0.4f, ver->name, argb(C_WHITE, 1));
      char m[128], where[96];
      const char *why;
      int can = version_get(e, i, &why);
      if (ver_there[i])
        mx_snprintf(m, sizeof m, "There");
      else if (ver->data >= 0) {
        char dsz[16];
        fmt_size(dsz, sizeof dsz, entries[ver->data].size);
        mx_snprintf(m, sizeof m, "In the store" DOT "%s%s%s", dsz, why ? DOT : "", why ? why : "");
      } else {
        /* the user's own copy: where its files go */
        const char *dd = kv_get(e, "data");
        const char *sl = strrchr(ver->file, '/');
        if (dd && !strncmp(dd, "../", 3))
          mx_snprintf(where, sizeof where, "%s/%.*s", dd + 3, sl ? (int)(sl - ver->file + 1) : 0, ver->file);
        else
          mx_snprintf(where, sizeof where, "%s/%s%s%.*s", e->id, dd ? dd : "", dd ? "/" : "",
                      sl ? (int)(sl - ver->file + 1) : 0, ver->file);
        mx_snprintf(m, sizeof m, "Your own copy: %s", where);
      }
      float tx = x + 16 + w * 0.4f;
      if (lit && can && !busy) {
        char g[32], dsz[16];
        fmt_size(dsz, sizeof dsz, entries[ver->data].size);
        mx_snprintf(g, sizeof g, "Get" DOT "%s", dsz);
        d_button(x + w - 200, y + 2, SYM_CROSS, g, 1);
        d_text_fit(FONT_SMALL, FONT_LABEL, tx, y + 29, w - 220 - w * 0.4f, m, argb(C_DIM, 1));
      } else
        d_text_fit(FONT_SMALL, FONT_LABEL, tx, y + 29, w * 0.6f - 32, m,
                   argb(ver_there[i] ? C_LAMP : C_DIM, 1));
      y += 52;
    }
  }

  /* the actions: one lit */
  int act = page_action(e);
  float by = 420 > y + 8 ? 420 : y + 8, bx = x;
  char lab[48], sz[16];
  fmt_size(sz, sizeof sz, e->size);
  if (busy && (job_entry == page || job_show == page)) {
    bx += d_button(bx, by, SYM_CROSS, job.kind == J_REMOVE ? "Removing" : "Installing", 1) + 14;
  } else if (act != A_NONE) {
    mx_snprintf(lab, sizeof lab, "%s" DOT "%s",
                act == A_INSTALL ? "Install" : act == A_UPDATE ? "Update" : act == A_REPLACE ? "Replace" : "Repair", sz);
    bx += d_button(bx, by, SYM_CROSS, lab, !busy && ver_sel < 0) + 14;
  } else if (e->on_dev >= 0) {
    bx += d_button(bx, by, SYM_CROSS, "Installed", 0) + 14;
  }
  if (e->on_dev >= 0 && e->on_version[0] && !busy)
    bx += d_button(bx, by, SYM_TRIANGLE, "Remove", 0) + 14;
  if (e->on_dev < 0 && nsdevs > 1 && act == A_INSTALL) {
    mx_snprintf(lab, sizeof lab, "To %s", sdevs[cur_dev].root);
    bx += d_button(bx, by, SYM_SQUARE, lab, 0) + 14;
  }
  if (why_not && !busy)
    d_text_fit(FONT_SMALL, FONT_LABEL, x, by + 70, w, why_not, argb(C_WARN, 1));
  else if (busy && job_entry != page && job_show != page)
    d_text_fit(FONT_SMALL, FONT_LABEL, x, by + 70, w, "Waits for the install going on", argb(C_FAINT, 1));

  /* the facts */
  if (fxr > 0) {
    float fy = 120;
    char v[64];
#define FACT(label, value)                                                                                 \
  do {                                                                                                     \
    d_label(fxr, fy, label, argb(C_FAINT, 1));                                                             \
    d_text_fit(FONT_SMALL, FONT_LABEL, fxr, fy + 24, 206, value, argb(C_WHITE, 0.9f));                    \
    fy += 62;                                                                                              \
  } while (0)
    mx_snprintf(v, sizeof v, "%s" DOT "%s", e->version, e->date);
    FACT("Version", v);
    if (e->on_dev >= 0) {
      mx_snprintf(v, sizeof v, "%.*s%s%s", (int)strlen(sdevs[e->on_dev].root) - 1, sdevs[e->on_dev].root,
                  e->on_version[0] ? DOT : DOT "by hand", e->on_version);
      FACT("On the device", v);
    } else {
      char fr[16];
      fmt_size(fr, sizeof fr, cur_dev >= 0 ? sdevs[cur_dev].free : 0);
      mx_snprintf(v, sizeof v, "%s%s%s", cur_dev >= 0 ? sdevs[cur_dev].root : "none",
                  cur_dev >= 0 && sdevs[cur_dev].free ? DOT : "", cur_dev >= 0 && sdevs[cur_dev].free ? fr : "");
      FACT("Installs to", v);
    }
    if (e->kernel) {
      mx_snprintf(v, sizeof v, "kernel %u or newer", e->kernel);
      FACT("System", v);
    }
    if (ver_sel >= 0 && e->vers[ver_sel].data >= 0) {
      const struct entry *de = &entries[e->vers[ver_sel].data];
      FACT("Its data", de->title);
      if (de->license[0])
        d_text_fit(FONT_SMALL, FONT_LABEL, fxr, fy - 14, 206, de->license, argb(C_DIM, 1));
      fy += 20;
    }
    if (e->source[0]) {
      const char *s = strstr(e->source, "github.com/");
      FACT("Source", s ? s + 11 : e->source);
      if (e->license[0])
        d_text(FONT_SMALL, fxr, fy - 14, e->license, argb(C_DIM, 1));
    }
#undef FACT
  }

  /* the screenshots on the floor, the chosen one lit */
  if (e->nshots) {
    float sy = 600, sx = MARGIN;
    d_rect_h(0, sy, SCR_W, sy + 1, argb(C_WHITE, 0.12f), argb(C_WHITE, 0.03f));
    for (int i = 0; i < e->nshots && sx < SCR_W - MARGIN - 100; i++) {
      int lit = i == shot_sel;
      float sw = lit ? 216 : 176, sh = sw * 9 / 16;
      const struct picture *p = &pages[page][1 + i];
      if (lit)
        d_glow(sx + sw * 0.5f, sy - sh * 0.5f, sw, 90, argb(C_LAMP, 0.16f));
      struct surface s = {sx, sy - sh - 6, sx + sw, sy - 6, 4, 0x15171Bu, -1, lit ? argb(C_LAMP, 0.45f)
                                                                                    : argb(C_WHITE, 0.08f),
                          lit ? 1.0f : 0.2f, 1, 0, 0, 0, 0, 0};
      surface_draw(&s);
      d_image(p, sx, sy - sh - 6, sx + sw, sy - 6, lit ? 1 : 0.62f);
      sx += sw + 20;
    }
  }
  static const char *const h_shots[] = {"L1", "Shot", "R1", "Shot", "R2", "View", SYM_CIRCLE, "Back", 0};
  static const char *const h_shots_d[] = {"Up/Down", "Its data", "L1",  "Shot",     "R1", "Shot",
                                          "R2",      "View",     SYM_CIRCLE, "Back", 0};
  static const char *const h[] = {SYM_CIRCLE, "Back", 0};
  static const char *const h_d[] = {"Up/Down", "Its data", SYM_CIRCLE, "Back", 0};
  hints_draw(e->nshots ? (e->nvers ? h_shots_d : h_shots) : e->nvers ? h_d : h);
  job_strip();
}

static void page_input(void) {
  struct entry *e = &entries[page];
  if (pad_pressed & MX_PAD_CIRCLE) {
    screen = S_SHELF;
    return;
  }
  if (e->nshots) {
    if (pad_pressed & (MX_PAD_L1 | MX_PAD_LEFT) && shot_sel > 0)
      shot_sel--;
    if (pad_pressed & (MX_PAD_R1 | MX_PAD_RIGHT) && shot_sel + 1 < e->nshots)
      shot_sel++;
    if (pad_pressed & MX_PAD_R2 && view_pic.state != PIC_BUSY && view_pic.state != PIC_WANT) {
      if (view_pic.state == PIC_DONE)
        tex_free(&view_pic);
      view_pic.state = PIC_NONE;
      want(&view_pic, &e->shots[shot_sel], 640, 360);
      worker_poke();
      screen = S_VIEW;
    }
  }
  /* up and down: the data's versions, above them the program's button */
  if (pad_pressed & MX_PAD_DOWN && ver_sel + 1 < e->nvers)
    ver_sel++;
  if (pad_pressed & MX_PAD_UP && ver_sel >= 0)
    ver_sel--;
  if (worker_busy())
    return;
  if (ver_sel >= 0) {
    const char *why;
    if (pad_pressed & MX_PAD_CROSS && version_get(e, ver_sel, &why)) {
      job_data = e->vers[ver_sel].data;
      start_job(J_DATA, page, 0);
    }
    return;
  }
  int act = page_action(e);
  if (pad_pressed & MX_PAD_CROSS && act == A_REPLACE)
    confirm_replace = 1, screen = S_CONFIRM;
  else if (pad_pressed & MX_PAD_CROSS && act != A_NONE)
    start_job(J_INSTALL, page, 0);
  if (pad_pressed & MX_PAD_TRIANGLE && e->on_dev >= 0 && e->on_version[0])
    confirm_replace = 0, screen = S_CONFIRM;
  if (pad_pressed & MX_PAD_SQUARE && e->on_dev < 0 && nsdevs > 1) {
    cur_dev = (cur_dev + 1) % nsdevs;
    page_open(page);
  }
}

static void view_draw(void) {
  room_begin(-1);
  d_begin_2d();
  room_draw();
  float w = SCR_W - 2 * 64, h = w * 9 / 16;
  if (h > SCR_H - 140)
    h = SCR_H - 140, w = h * 16 / 9;
  float x0 = (SCR_W - w) * 0.5f, y0 = (SCR_H - h) * 0.5f - 20;
  if (view_pic.state == PIC_DONE)
    d_image(&view_pic, x0, y0, x0 + w, y0 + h, 1);
  else
    d_text(FONT_BODY, x0, y0 + h * 0.5f, view_pic.state == PIC_FAILED ? "The picture did not load" : "Loading",
           argb(C_DIM, 1));
  static const char *const h_[] = {"L1", "Shot", "R1", "Shot", SYM_CIRCLE, "Back", 0};
  hints_draw(h_);
}
static void view_input(void) {
  const struct entry *e = &entries[page];
  int d = (pad_pressed & (MX_PAD_R1 | MX_PAD_RIGHT) ? 1 : 0) - (pad_pressed & (MX_PAD_L1 | MX_PAD_LEFT) ? 1 : 0);
  if (pad_pressed & (MX_PAD_CIRCLE | MX_PAD_R2)) {
    screen = S_PAGE;
    return;
  }
  if (d && shot_sel + d >= 0 && shot_sel + d < e->nshots && view_pic.state != PIC_BUSY &&
      view_pic.state != PIC_WANT) {
    shot_sel += d;
    if (view_pic.state == PIC_DONE)
      tex_free(&view_pic);
    view_pic.state = PIC_NONE;
    want(&view_pic, &e->shots[shot_sel], 640, 360);
    worker_poke();
  }
}

/* ---- installing (the mockup's "Install") ---- */
static int job_was_busy;
static void install_draw(void) {
  const struct entry *e = &entries[job_entry];
  int show = job_show >= 0 ? job_show : job_entry;
  room_begin(-1);
  room_light(SCR_W * 0.5f, 240, 480, 300, 0.07f);
  d_begin_2d();
  room_draw();
  d_image(&pages[show][0], 0, 0, SCR_W, SCR_W * 9 / 16, 0.25f);
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.55f), argb(C_INK, 1));
  /* the slab in the middle, the light filling it from below */
  struct tile_look slab;
  memset(&slab, 0, sizeof slab);
  slab.w = 204, slab.h = 288, slab.x = SCR_W * 0.5f - 102, slab.y = 92, slab.lit = 1, slab.fade = 1;
  slab.base = entries[show].base;
  if (covers[show].state == PIC_DONE)
    slab.tex = covers[show].tex[0], slab.tex_u = 1, slab.tex_v = 361.0f / 512.0f;
  d_begin_3d();
  light_at(SCR_W * 0.5f, 0);
  d_tile(&slab);
  d_begin_2d();
  int dl = job.phase == PH_DOWNLOAD, removing = job_kind == J_REMOVE;
  float k = removing ? (job.nfiles ? (float)job.files / job.nfiles : 0)
                     : dl ? 0.8f * (job.total ? (float)job.done / job.total : 0)
                          : job.phase == PH_CHECK ? 0.8f
                          : job.phase == PH_UNPACK ? 0.8f + 0.2f * (job.nfiles ? (float)job.files / job.nfiles : 0)
                                                   : 1;
  if (!removing)
    d_rect(slab.x, slab.y + slab.h * (1 - 0.62f * k), slab.x + slab.w, slab.y + slab.h, argb(C_LAMP, 0.05f),
           argb(C_LAMP, 0.22f));
  char t[96], where[64];
  int dev = removing ? e->on_dev : (e->on_dev >= 0 ? e->on_dev : cur_dev);
  int done = job.phase == PH_DONE, failed = job.phase == PH_FAILED || job.phase == PH_CANCELLED;
  mx_snprintf(t, sizeof t, "%s %s", removing ? (done ? "Removed" : "Removing")
                                    : done ? "Installed" : failed ? "Not installed:" : "Installing", e->title);
  float tw = d_text_w(FONT_TITLE, t);
  d_text(FONT_TITLE, SCR_W * 0.5f - tw * 0.5f, 430, t, argb(C_WHITE, 1));
  if (failed)
    mx_snprintf(where, sizeof where, "%s", job.phase == PH_CANCELLED ? "Stopped" : job.message);
  else if (e->kind == K_DATA) {
    const char *dd = kv_get(e, "data");
    mx_snprintf(where, sizeof where, "to %s/%s", dev >= 0 ? sdevs[dev].base : "",
                dd && !strncmp(dd, "../", 3) ? dd + 3 : e->id);
  } else
    mx_snprintf(where, sizeof where, "%s %s/%s", removing ? "from" : "to", dev >= 0 ? sdevs[dev].base : "", e->id);
  tw = d_text_w(FONT_BODY, where);
  d_text(FONT_BODY, SCR_W * 0.5f - tw * 0.5f, 462, where, argb(failed ? C_WARN : C_DIM, 1));
  /* the progress: the floor lit from the left */
  float bx0 = SCR_W * 0.5f - 400, bx1 = SCR_W * 0.5f + 400;
  if (SCR_W < 1100)
    bx0 = MARGIN, bx1 = SCR_W - MARGIN;
  d_bar(bx0, 500, bx1, 502, k, C_LAMP, 1);
  d_glow(bx0 + (bx1 - bx0) * k, 501, 240, 60, argb(C_LAMP, 0.14f));
  /* the steps */
  if (!removing) {
    static const char *const step[3] = {"Download", "Check", "Unpack"};
    float cw = (bx1 - bx0) / 3;
    for (int i = 0; i < 3; i++) {
      float sx = bx0 + cw * i;
      int ph = job.phase >= PH_DONE ? 3 : job.phase;
      int is_done = ph > i, now = ph == i && !failed;
      u32 c = now ? C_LAMP : is_done ? C_WHITE : C_FAINT;
      if (now)
        d_glow(sx + 6, 532, 12, 12, argb(C_LAMP, 0.6f));
      d_text(FONT_BODY, sx + (now || is_done ? 20 : 0), 538, step[i], argb(c, 1));
      if (is_done)
        d_ring(sx + 6, 532, 5, argb(C_WHITE, 0.9f));
      char m[64] = "";
      if (i == 0) {
        char a[16], b[16];
        fmt_size(a, sizeof a, job.done), fmt_size(b, sizeof b, job.total);
        if (job.phase == PH_DOWNLOAD)
          mx_snprintf(m, sizeof m, "%s of %s" DOT "%u KB/s", a, b, job.rate);
        else
          mx_snprintf(m, sizeof m, "%s" DOT "%u KB/s", b, job.rate);
      } else if (i == 1 && ph > 1)
        mx_snprintf(m, sizeof m, "SHA-256 matches the index");
      else if (i == 2 && ph >= 2 && job.kb) {
        char a[16], b[16];
        fmt_size(a, sizeof a, (u64)job.files << 10), fmt_size(b, sizeof b, (u64)job.nfiles << 10);
        mx_snprintf(m, sizeof m, "%s of %s", a, b);
      } else if (i == 2 && ph >= 2)
        mx_snprintf(m, sizeof m, "%u of %u files", job.files, job.nfiles);
      d_text_fit(FONT_SMALL, FONT_LABEL, sx, 564, cw - 16, m, argb(C_FAINT, 1));
    }
  }
  static const char *const h_dl[] = {SYM_SQUARE, "Stop", SYM_CIRCLE, "Go on in the background", 0};
  static const char *const h_go[] = {SYM_CIRCLE, "Go on in the background", 0};
  static const char *const h_end[] = {SYM_CROSS, "Done", 0};
  hints_draw(!worker_busy()                                                  ? h_end
             : (job.phase == PH_DOWNLOAD || (job.phase == PH_UNPACK && job.kb)) && !removing ? h_dl
                                                                                           : h_go);
}
static void install_input(void) {
  if (worker_busy()) {
    if (pad_pressed & MX_PAD_SQUARE && (job.phase == PH_DOWNLOAD || (job.phase == PH_UNPACK && job.kb)))
      job.cancel = 1;
    if (pad_pressed & MX_PAD_CIRCLE)
      screen = S_PAGE, page = job_show;
    return;
  }
  if (pad_pressed & (MX_PAD_CROSS | MX_PAD_CIRCLE))
    screen = S_PAGE, page_open(job_show);
}
/* a job ended: a notification, the shelf's rows again */
static void job_watch(void) {
  int busy = worker_busy() && job_entry >= 0;
  if (job_was_busy && !busy && job_entry >= 0) {
    const struct entry *e = &entries[job_entry];
    char t[64], m[64];
    if (job.phase == PH_DONE) {
      mx_snprintf(t, sizeof t, "%s %s", e->title, job_kind == J_REMOVE ? "removed" : "installed");
      if (e->kind == K_DATA)
        mx_snprintf(m, sizeof m, "for %s", entries[job_show].title);
      else
        mx_snprintf(m, sizeof m, "%s" DOT "%s", e->version, job_kind == J_REMOVE ? "its own files kept" : "on the shelf");
      toast(N_INFO, t, m, 0);
    } else if (job.phase == PH_FAILED) {
      mx_snprintf(t, sizeof t, "%s not %s", e->title, job_kind == J_REMOVE ? "removed" : "installed");
      toast(N_WARN, t, job.message, 0);
    }
  }
  job_was_busy = busy;
}

static void confirm_draw(void) {
  page_draw();
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.62f), argb(C_INK, 0.62f));
  float w = 560, h = 210, x = (SCR_W - w) * 0.5f, y = 230;
  d_card_lit(x, y, x + w, y + h, 10, C_SLAB, 1, 0.5f);
  char t[96];
  mx_snprintf(t, sizeof t, "%s %s?", confirm_replace ? "Replace" : "Remove", entries[page].title);
  d_text_fit(FONT_TITLE, FONT_BODY, x + 32, y + 60, w - 64, t, argb(C_WHITE, 1));
  para(FONT_SMALL, x + 32, y + 98, w - 64,
       confirm_replace ? "This copy was put there by hand. Its program, game.ini and cover become the store's; "
                         "game data and saves stay."
                       : "The files the store put there go. Game data, saves and settings of your own stay.",
       argb(C_DIM, 1), 2);
  float bx = x + 32;
  bx += d_button(bx, y + 140, SYM_CROSS, confirm_replace ? "Replace" : "Remove", 1) + 14;
  d_button(bx, y + 140, SYM_CIRCLE, "Keep it", 0);
}
static void confirm_input(void) {
  if (pad_pressed & MX_PAD_CROSS)
    start_job(confirm_replace ? J_INSTALL : J_REMOVE, page, confirm_replace);
  else if (pad_pressed & MX_PAD_CIRCLE)
    screen = S_PAGE;
}

/* ---- the frame ---- */
static void read_pad(void) {
  pad_pressed = pad_held = 0;
  if (!pad_on)
    return;
  pad_pressed = mx_pad_pressed(0) | mx_pad_pressed(1);
  pad_held = mx_pad_buttons(0) | mx_pad_buttons(1);
  if (pad_pressed && toast_dismiss())
    pad_pressed = 0;
}
static void frame(void) {
  glClearColor((C_INK >> 16 & 255) / 255.0f, (C_INK >> 8 & 255) / 255.0f, (C_INK & 255) / 255.0f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  switch (screen) {
  case S_PAGE: page_draw(); break;
  case S_VIEW: view_draw(); break;
  case S_INSTALL: install_draw(); break;
  case S_CONFIRM: confirm_draw(); break;
  default: shelf_draw(); job_strip(); break;
  }
  toast_draw();
  mxgl_end_frame();
  mxgl_frame_done();
}

static int find(const char *id) {
  for (int i = 0; i < nentries; i++)
    if (!strcmp(entries[i].id, id))
      return i;
  return -1;
}

#ifdef MX_BIOS_CALLS
static char file_buf[512];
static char *file_args[12];
static int args_from_host(char ***argv) {
  int fd = mx_fio_open("host:store.txt", MX_FIO_RDONLY), n = 0, argc = 1;
  if (fd < 0)
    return 1;
  n = mx_fio_read(fd, file_buf, sizeof file_buf - 1);
  mx_fio_close(fd);
  file_buf[n > 0 ? n : 0] = 0;
  file_args[0] = "store";
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

int main(int argc, char **argv) {
  cfg_load();
#ifdef MX_BIOS_CALLS
  if (argc <= 1)
    argc = args_from_host(&argv);
#endif
  int shot = 0;
  const char *o_open = 0, *o_install = 0, *o_remove = 0;
  int o_data = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strncmp(a, "shot=", 5))
      shot = atoi(a + 5);
    else if (!strncmp(a, "base=", 5))
      mx_snprintf(site_base, sizeof site_base, "%s", a + 5);
    else if (!strcmp(a, "dev=host"))
      with_host = 1;
    else if (!strcmp(a, "dev=hostonly"))
      with_host = store_host_only = 1; /* tests that write: never a USB drive */
    else if (!strncmp(a, "sec=", 4))
      sec = atoi(a + 4) % NSS;
    else if (!strncmp(a, "open=", 5))
      o_open = a + 5;
    else if (!strncmp(a, "install=", 8))
      o_install = a + 8;
    else if (!strncmp(a, "data=", 5))
      o_install = a + 5, o_data = 1;
    else if (!strncmp(a, "remove=", 7))
      o_remove = a + 7;
    else if (!strncmp(a, "mode=", 5))
      cfg_arg("video.mode", a + 5);
  }
  mx_printf("store: the site %s, kernel %u\n", site_base, kernel_version());
  mxgl_init();
  pad_on = mx_pad_init() >= 0;
  video_apply();
  devs_refresh();
  worker_start();
  worker_job(J_INDEX, 0, 0, 0);
  int o_done = 0, o_started = 0;
  for (;;) {
    ticks++;
    read_pad();
    if (ticks % 15 == 0)
      devs_refresh();
    pictures_step();
    job_watch();
    toast_step();
    /* tests */
    if (!o_done && index_state == IX_FAILED && (o_open || o_install || o_remove)) {
      mx_printf("store: FAILED: %s\n", index_message);
      MX_TEST_END(1);
      return 1;
    }
    if (!o_done && index_state == IX_READY && !worker_busy()) {
      const char *id = o_install ? o_install : o_remove ? o_remove : o_open;
      if (!id)
        o_done = 1;
      else if (!o_started) {
        int e = find(id);
        if (e < 0) {
          mx_printf("store: FAILED: %s is not in the index\n", id);
          MX_TEST_END(1);
          return 1;
        }
        page_open(e);
        /* data=: the first version of its data the store brings that is
           not there */
        if (o_data)
          for (int v = entries[e].nvers - 1; v >= 0; v--)
            if (entries[e].vers[v].data >= 0 && !ver_there[v])
              job_data = entries[e].vers[v].data;
        if (o_install || o_remove)
          start_job(o_data ? J_DATA : o_install ? J_INSTALL : J_REMOVE, e, 0);
        o_started = 1;
        if (!o_install && !o_remove)
          o_done = 1;
      } else {
        int ok = job.phase == PH_DONE;
        mx_printf("store: %s %s: %s%s\n", o_install ? "install" : "remove", id, ok ? "OK" : "FAILED: ",
                  ok ? "" : job.message);
        if (!shot) {
          MX_TEST_END(ok ? 0 : 1);
          return ok ? 0 : 1;
        }
        o_done = 1;
      }
    }
    switch (screen) {
    case S_PAGE: page_input(); break;
    case S_VIEW: view_input(); break;
    case S_INSTALL: install_input(); break;
    case S_CONFIRM: confirm_input(); break;
    default:
      if (pad_pressed & MX_PAD_CIRCLE && !worker_busy())
        return 0;
      shelf_input();
      break;
    }
    frame();
    if (shot && ticks == (u32)shot) {
      int r = mxgl_screenshot("host:store.tga");
      mx_printf("store: shot %d\n", r);
      MX_TEST_END(r ? 1 : 0);
      return 0;
    }
  }
}
