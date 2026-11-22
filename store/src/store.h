/* MXSTORE, the store of mx OS: what its parts share.

   catalog.c  the index (apps/mxstore docs/index-format.md): its signature
              checked, its sections as entries
   fetch.c    files from the site or from a device: HTTPS (https.c) or a
              path (host:, usb0:, ... for tests); the worker thread that does
              all of the network and file work, the pictures' cache
   install.c  a package: downloaded, checked, unpacked, its game.ini
              written; removing it
   store.c    the screens, on the shell's drawing (../src) */
#ifndef MXSTORE_STORE_H
#define MXSTORE_STORE_H

#include <mx/types.h>

/* ---- the index ---- */
#define INDEX_MAX (256u << 10)
#define MAX_ENTRIES 64
#define MAX_SHOTS 6
#define MAX_KV 40

struct kv {
  const char *k, *v;
};

/* a picture of the index: its path below base and its SHA-256 */
struct pic_ref {
  const char *path; /* 0: none */
  u8 sha[32];
};

/* K_DATA: a game's data, the original archive of its makers (unpack.c) */
enum { K_GAME, K_EMULATOR, K_APP, K_SYSTEM, K_DATA };
enum { U_NONE, U_KERNEL, U_BOOT };

struct entry {
  const char *id, *title, *section, *type, *year, *description, *developer, *players;
  const char *version, *date, *url, *needs, *needs_text, *source, *license;
  int kind, update, order;
  u32 size, installed, kernel, provides;
  u8 sha[32];
  struct pic_ref cover, hero, shots[MAX_SHOTS], trailer;
  int nshots;
  struct kv kv[MAX_KV]; /* all of its keys, in order: the game.ini an install writes */
  int nkv;
  u32 base; /* the shell's colour for a tile without a cover */
  /* on the devices (fetch.c looks): the device it is on, -1 none; the
     version the store put there ("" by hand, not by the store) */
  int on_dev;
  char on_version[24];
  u8 partial; /* an install that did not finish */
  int data; /* a game: its data entry (for= names the game), -1 none */
};

extern struct entry entries[MAX_ENTRIES];
extern int nentries;
extern char index_generated[24], index_base[160];

/* the index in data (n bytes, kept: the entries point into it) checked
   against sig (64 bytes, r and s) and read: 0, or < 0 with why in
   catalog_error */
int catalog_load(char *data, u32 n, const u8 *sig, u32 sig_n);
extern const char *catalog_error;
int hex32(const char *s, u8 out[32]); /* 64 hex digits: 0, else -1 */
const char *kv_get(const struct entry *e, const char *k);
/* the shell's tile colour of a title (apps/mxshell src/games.c) */
u32 title_colour(const char *t);

/* ---- the devices the store installs to ---- */
#define SDEV_MAX 8
struct sdev {
  char root[12]; /* "usb0:" */
  char name[24];
  u64 free;      /* bytes, 0 not known */
  int writable;
};
extern struct sdev sdevs[SDEV_MAX];
extern int nsdevs, cur_dev;

/* ---- the worker (fetch.c) ---- */
/* where the index comes from: the site, or (tests) a directory such as
   "host:site/"; packages and pictures are relative to it */
extern char site_base[160];
/* a picture made ready for a texture by the worker */
enum { PIC_NONE, PIC_WANT, PIC_BUSY, PIC_READY, PIC_DONE, PIC_FAILED };
struct picture {
  volatile int state;
  const struct pic_ref *ref;
  int cover;     /* the shell's T8 (bytes), else a JPEG decoded (pixels) */
  u8 *data;      /* cover: 1024 + 256 * 361 bytes; else w * h RGBA pixels */
  u16 w, h;
  int tex[9];    /* the main thread's textures (store.c) */
  u8 nx, ny;
  u16 tw, th;
};
extern struct picture covers[MAX_ENTRIES];
#define PAGE_PICS (1 + MAX_SHOTS)
/* each page's: the hero, then the shots; wanted when the page opens, the
   textures given back when another one does (a picture still being made
   is left to the worker) */
extern struct picture pages[MAX_ENTRIES][PAGE_PICS];
extern struct picture view_pic; /* a screenshot at its full size */

/* the state of the index, for the screens */
enum { IX_NONE, IX_LOADING, IX_READY, IX_FAILED };
extern volatile int index_state;
extern char index_message[96];

/* a job of the worker's, one at a time */
enum { J_NONE, J_INDEX, J_INSTALL, J_REMOVE, J_SCAN };
enum { PH_DOWNLOAD, PH_CHECK, PH_UNPACK, PH_DONE, PH_FAILED, PH_CANCELLED };
struct job {
  volatile int kind;   /* J_NONE when the worker is free */
  int entry, dev;
  volatile int phase;
  volatile u32 done, total; /* bytes (download), files (unpack) */
  volatile u32 files, nfiles;
  volatile u32 rate;   /* KB/s */
  volatile int cancel;
  int replace;         /* J_INSTALL: a copy put there by hand may be replaced */
  int kb;              /* PH_UNPACK counts KB of the archive (a data entry), not files */
  char message[96];
};
extern struct job job;
void worker_start(void);
int worker_busy(void);
void worker_job(int kind, int entry, int dev, int replace); /* when not busy */
void worker_poke(void);                        /* new pictures are wanted */

/* fetch.c: url (absolute, or relative to site_base) into sink */
typedef int (*fetch_sink)(void *ctx, const void *data, u32 size);
int fetch(const char *url, fetch_sink sink, void *ctx);
const char *fetch_error(int e);
/* a picture's bytes (malloc'd), checked: from the cache, else the site */
u8 *pic_bytes(const struct pic_ref *ref, u32 max, u32 *n);
#define COVER_BYTES (1024u + 256u * 361u)
u64 mxstore_cycles(void);

/* ---- install.c (the worker's) ---- */
int install_entry(int e, int dev);
int remove_entry(int e);
/* unpack.c: a data entry's archive (take=) into its data directory */
int unpack_data(const struct entry *e, int dev, const char *archive);
/* are all of e's needs= on device dev ("," between them, "|" between ones
   that do instead of each other) */
int needs_there(const struct entry *e, int dev);
void scan_installed(void); /* entries[].on_dev from the devices */
/* a file of e's data directory on device dev (data=; else PS2MAX/<id>/) */
void data_path(char *out, int len, const struct entry *e, int dev, const char *file);
void mkdirs(const char *path); /* each directory of path (up to its last /) */
int write_file(const char *path, const void *data, u32 n);

/* the running kernel's version */
u32 kernel_version(void);

#endif
