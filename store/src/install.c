/* Installing a program (apps/mxstore docs/index-format.md, "The package"),
   on the worker thread: the package to <device>/PS2MAX/.store/<id>.zip,
   its size and SHA-256 checked against the index, then unpacked into
   <device>/PS2MAX/<id>/, and its game.ini written from the index's section
   with the package's elf=, args=, saves=; the cover beside it.

   What the store wrote is listed in PS2MAX/<id>/.store (the version, the
   files): an update removes the files the new version no longer has,
   Remove takes away only those; the game data, saves and configs the
   program wrote are never touched. mxio has no rename, so the files are
   written in place: .store says state=partial until all are there, and the
   store offers to repair such an install. */
#include "store.h"

#include <mbedtls/sha256.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/vm.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define MAX_FILES 512
#define NAMES (32u << 10)
#define CHUNK (32u << 10)
#define CD_MAX (256u << 10)

static u8 inbuf[CHUNK], outbuf[CHUNK];

/* ---- the list of what the store wrote ---- */
struct record {
  char version[24];
  int partial;
  char *names; /* NAMES bytes: file\0file\0 */
  u32 used;
  int n;
};

static void path_of(char *out, int len, int dev, const char *id, const char *file) {
  mx_snprintf(out, len, "%s/%s%s%s", sdevs[dev].base, id, file ? "/" : "", file ? file : "");
}

static int record_read(int dev, const char *id, struct record *r) {
  char p[128];
  path_of(p, sizeof p, dev, id, ".store");
  r->version[0] = 0, r->partial = 0, r->used = 0, r->n = 0;
  int fd = mx_fio_open(p, MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  static char buf[NAMES + 256];
  int n = mx_fio_read(fd, buf, sizeof buf - 1);
  mx_fio_close(fd);
  buf[n > 0 ? n : 0] = 0;
  for (char *s = buf; *s;) {
    char *e = strchr(s, '\n');
    if (e)
      *e = 0;
    if (!strncmp(s, "version=", 8))
      mx_snprintf(r->version, sizeof r->version, "%s", s + 8);
    else if (!strcmp(s, "state=partial"))
      r->partial = 1;
    else if (!strncmp(s, "file=", 5) && r->names && r->used + strlen(s + 5) + 1 < NAMES) {
      strcpy(r->names + r->used, s + 5);
      r->used += strlen(s + 5) + 1, r->n++;
    }
    if (!e)
      break;
    s = e + 1;
  }
  return 0;
}

static int record_write(int dev, const char *id, const struct record *r) {
  char p[128];
  path_of(p, sizeof p, dev, id, ".store");
  static char buf[NAMES + 256];
  u32 n = (u32)mx_snprintf(buf, sizeof buf, "version=%s\nstate=%s\n", r->version, r->partial ? "partial" : "ok");
  const char *s = r->names;
  for (int i = 0; i < r->n; i++, s += strlen(s) + 1)
    n += (u32)mx_snprintf(buf + n, sizeof buf - n, "file=%s\n", s);
  return write_file(p, buf, n);
}

static int record_has(const struct record *r, const char *name) {
  const char *s = r->names;
  for (int i = 0; i < r->n; i++, s += strlen(s) + 1)
    if (!strcmp(s, name))
      return 1;
  return 0;
}

static void record_add(struct record *r, const char *name) {
  u32 l = (u32)strlen(name) + 1;
  if (record_has(r, name) || r->used + l > NAMES)
    return;
  memcpy(r->names + r->used, name, l);
  r->used += l, r->n++;
}

/* ---- which device has which ---- */
static int exists(const char *p) {
  int fd = mx_fio_open(p, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  mx_fio_close(fd);
  return 1;
}

void scan_installed(void) {
  for (int i = 0; i < nentries; i++) {
    struct entry *e = &entries[i];
    e->on_dev = -1, e->on_version[0] = 0, e->partial = 0;
    if (e->kind == K_DATA) {
      /* there when its needs= are (the user's own copy counts too) */
      for (int k = 0; k < nsdevs && e->on_dev < 0 && e->needs[0]; k++)
        if (needs_there(e, k))
          e->on_dev = k;
      continue;
    }
    for (int k = 0; k < nsdevs && e->on_dev < 0; k++) {
      /* the device installed to first */
      int d = k == 0 && cur_dev >= 0 ? cur_dev : k == cur_dev ? 0 : k;
      if (d >= nsdevs)
        continue;
      struct record r = {0};
      if (record_read(d, e->id, &r) == 0) {
        e->on_dev = d, e->partial = (u8)r.partial;
        mx_snprintf(e->on_version, sizeof e->on_version, "%s", r.version);
        continue;
      }
      char p[128];
      path_of(p, sizeof p, d, e->id, "game.ini");
      if (exists(p))
        e->on_dev = d; /* put there by hand */
    }
  }
}

/* ---- the download ---- */
struct dl {
  int fd;
  mbedtls_sha256_context sha;
  u32 n, max;
  u64 t0;
};
static int dl_sink(void *ctx, const void *data, u32 size) {
  struct dl *d = ctx;
  if (job.cancel || d->n + size > d->max)
    return -1;
  mbedtls_sha256_update(&d->sha, data, size);
  if (aw_write(data, size) != 0)
    return -1;
  d->n += size;
  job.done = d->n;
  u32 ms = (u32)((mxstore_cycles() - d->t0) / (MX_EE_HZ / 1000u));
  job.rate = ms ? d->n / ms : 0; /* bytes a ms: KB/s */
  return 0;
}

static int fail(const char *what) {
  mx_snprintf(job.message, sizeof job.message, "%s", what);
  job.phase = PH_FAILED;
  mx_printf("store: install: %s\n", what);
  return -1;
}

/* ---- the zip ---- */
static u32 le16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static int read_at(int fd, u32 off, void *buf, u32 n) {
  if (mx_fio_lseek(fd, (s32)off, MX_FIO_SEEK_SET) != (int)off)
    return -1;
  return mx_fio_read(fd, buf, n) == (int)n ? 0 : -1;
}

/* a name the package may have: below <id>/, no "..", no "\", no ":" */
static int name_ok(const char *name, const char *id) {
  u32 l = (u32)strlen(id);
  if (strncmp(name, id, l) || name[l] != '/')
    return 0;
  if (strstr(name, "..") || strchr(name, '\\') || strchr(name, ':') || strstr(name, "//"))
    return 0;
  return 1;
}

/* one file of the zip, at its local header, to path (or into mem, max
   bytes: the package's game.ini) */
static int unpack_one(int zfd, u32 local, u32 method, u32 csize, u32 usize, u32 crc, const char *path, char *mem,
                      u32 max) {
  u8 h[30];
  if (read_at(zfd, local, h, 30) || le32(h) != 0x04034b50)
    return -1;
  u32 at = local + 30 + le16(h + 26) + le16(h + 28);
  if (mx_fio_lseek(zfd, (s32)at, MX_FIO_SEEK_SET) != (int)at)
    return -1;
  int out = -1;
  if (!mem) {
    mkdirs(path);
    out = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
    if (out < 0)
      return -1;
  }
  z_stream z;
  memset(&z, 0, sizeof z);
  if (method == 8 && inflateInit2(&z, -15) != Z_OK) {
    if (out >= 0)
      mx_fio_close(out);
    return -1;
  }
  u32 left = csize, wrote = 0, c = (u32)crc32(0, 0, 0);
  int r = 0, end = 0;
  while (!r && !end) {
    u32 n = left < CHUNK ? left : CHUNK;
    if (n && mx_fio_read(zfd, inbuf, n) != (int)n) {
      r = -1;
      break;
    }
    left -= n;
    const u8 *piece;
    u32 pn;
    if (method == 0) {
      piece = inbuf, pn = n, end = left == 0;
      if (!n)
        break;
      goto put;
    }
    z.next_in = inbuf, z.avail_in = n;
    do {
      z.next_out = outbuf, z.avail_out = CHUNK;
      int zr = inflate(&z, Z_NO_FLUSH);
      if (zr != Z_OK && zr != Z_STREAM_END && !(zr == Z_BUF_ERROR && z.avail_out == CHUNK && left)) {
        r = -1;
        break;
      }
      if (zr == Z_STREAM_END)
        end = 1;
      piece = outbuf, pn = CHUNK - z.avail_out;
      if (pn) {
        c = (u32)crc32(c, piece, pn);
        if (mem) {
          if (wrote + pn > max) {
            r = -1;
            break;
          }
          memcpy(mem + wrote, piece, pn);
        } else if (mx_fio_write(out, piece, pn) != 0) {
          r = -1;
          break;
        }
        wrote += pn;
      }
    } while (!r && !end && z.avail_out == 0);
    if (!end && !n)
      r = -1; /* the data ended before the stream */
    continue;
  put:
    c = (u32)crc32(c, piece, pn);
    if (mem) {
      if (wrote + pn > max) {
        r = -1;
        break;
      }
      memcpy(mem + wrote, piece, pn);
    } else if (mx_fio_write(out, piece, pn) != 0)
      r = -1;
    wrote += pn;
  }
  if (method == 8)
    inflateEnd(&z);
  if (out >= 0)
    mx_fio_close(out);
  if (mem && wrote < max)
    mem[wrote] = 0;
  if (!r && (wrote != usize || c != crc))
    r = -1;
  return r;
}

/* ---- game.ini: the index's section, the package's elf=, args=, saves=, data=, setup=, setup_args=, versions= ---- */
static const char *ini_get(const char *ini, const char *key, char *out, u32 n) {
  u32 l = (u32)strlen(key);
  for (const char *s = ini; s && *s;) {
    while (*s == '\r' || *s == '\n' || *s == ' ')
      s++;
    if (!strncmp(s, key, l) && s[l] == '=') {
      const char *v = s + l + 1;
      u32 k = 0;
      while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < n)
        out[k] = v[k], k++;
      out[k] = 0;
      return out;
    }
    s = strchr(s, '\n');
  }
  return 0;
}

/* a file of an entry's data directory (data=, relative to PS2MAX/<id>/;
   "../DATA/quake" is PS2MAX/DATA/quake), without data= its own directory:
   where needs= are looked for */
void data_path(char *out, int len, const struct entry *e, int dev, const char *file) {
  const char *d = kv_get(e, "data");
  if (d && !strncmp(d, "../", 3))
    mx_snprintf(out, len, "%s/%s/%s", sdevs[dev].base, d + 3, file);
  else if (d && *d)
    mx_snprintf(out, len, "%s/%s/%s/%s", sdevs[dev].base, e->id, d, file);
  else
    mx_snprintf(out, len, "%s/%s/%s", sdevs[dev].base, e->id, file);
}

int data_there(const struct entry *e, int dev, const char *file) {
  char p[256];
  data_path(p, sizeof p, e, dev, file);
  return exists(p);
}

int needs_there(const struct entry *e, int dev) {
  char list[200];
  mx_snprintf(list, sizeof list, "%s", e->needs);
  for (char *s = list; s && *s;) {
    char *c = strchr(s, ',');
    if (c)
      *c = 0;
    /* one of them is enough: "baseq3/pak0.pk3|demoq3/pak0.pk3" */
    int any = 0;
    for (char *a = s; a && *a && !any;) {
      char *bar = strchr(a, '|');
      if (bar)
        *bar = 0;
      char p[256];
      data_path(p, sizeof p, e, dev, a);
      any = exists(p);
      a = bar ? bar + 1 : 0;
    }
    if (!any)
      return 0;
    s = c ? c + 1 : 0;
  }
  return 1;
}

static int write_ini(int dev, const struct entry *e, const char *pkg) {
  static char buf[4096];
  u32 n = 0;
#define PUT(...) n += (u32)mx_snprintf(buf + n, sizeof buf - n, __VA_ARGS__)
  static const char *const shell[] = {"title", "section", "type", "year", "description", "order", 0};
  static const char *const store[] = {"kind", "developer", "players", "version", "kernel", "needs", "needs_text",
                                      "source", "license", 0};
  for (int i = 0; shell[i]; i++)
    if (kv_get(e, shell[i]))
      PUT("%s=%s\n", shell[i], kv_get(e, shell[i]));
  if (e->cover.path)
    PUT("cover_hd=coverhd.t8\n");
  char v[256];
  if (pkg && ini_get(pkg, "elf", v, sizeof v))
    PUT("elf=%s\n", v);
  else
    PUT("elf=%s.elf\n", e->id);
  if (pkg && ini_get(pkg, "args", v, sizeof v))
    PUT("args=%s\n", v);
  if (pkg && ini_get(pkg, "saves", v, sizeof v))
    PUT("saves=%s\n", v);
  if (pkg && ini_get(pkg, "data", v, sizeof v))
    PUT("data=%s\n", v);
  if (pkg && ini_get(pkg, "setup", v, sizeof v))
    PUT("setup=%s\n", v);
  if (pkg && ini_get(pkg, "setup_args", v, sizeof v))
    PUT("setup_args=%s\n", v);
  if (pkg && ini_get(pkg, "versions", v, sizeof v))
    PUT("versions=%s\n", v);
  for (int i = 0; store[i]; i++)
    if (kv_get(e, store[i]))
      PUT("%s=%s\n", store[i], kv_get(e, store[i]));
#undef PUT
  char p[128];
  path_of(p, sizeof p, dev, e->id, "game.ini");
  return write_file(p, buf, n);
}

/* the entry's url= to path, its size and SHA-256 checked against the index */
static int download(const struct entry *e, const char *path) {
  mkdirs(path);
  job.phase = PH_DOWNLOAD, job.total = e->size, job.done = 0, job.kb = 0;
  static struct dl d;
  d.fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (d.fd < 0)
    return fail("Cannot write to the device");
  mbedtls_sha256_init(&d.sha);
  mbedtls_sha256_starts(&d.sha, 0);
  d.n = 0, d.max = e->size, d.t0 = mxstore_cycles();
  aw_begin(d.fd);
  int r = fetch(e->url, dl_sink, &d);
  if (aw_end() && !r && !job.cancel) {
    mx_fio_close(d.fd);
    mx_fio_remove(path);
    return fail("Cannot write to the device");
  }
  mx_fio_close(d.fd);
  u8 h[32];
  mbedtls_sha256_finish(&d.sha, h);
  mbedtls_sha256_free(&d.sha);
  if (job.cancel) {
    mx_fio_remove(path);
    job.phase = PH_CANCELLED;
    return -1;
  }
  if (r) {
    mx_fio_remove(path);
    return fail(d.n >= d.max ? "Larger than the index says" : fetch_error(r));
  }
  {
    u32 ms = (u32)((mxstore_cycles() - d.t0) / (MX_EE_HZ / 1000u));
    mx_printf("store: %s: %u KB in %u ms (%u KB/s)\n", e->id, d.n >> 10, ms, ms ? d.n / ms : 0);
  }
  job.phase = PH_CHECK;
  if (d.n != e->size || memcmp(h, e->sha, 32)) {
    mx_fio_remove(path);
    return fail("Not the file the index names (SHA-256)");
  }
  return 0;
}

/* a data entry: its archive to PS2MAX/.store/, unpacked into its data
   directory (unpack.c), the archive removed. Nothing is recorded: what is
   there is the user's from then on (needs= says whether it is there) */
static int install_data(struct entry *e, int dev) {
  char arc[128];
  mx_snprintf(arc, sizeof arc, "%s/.store/%s.dat", sdevs[dev].base, e->id);
  if (download(e, arc))
    return -1;
  job.phase = PH_UNPACK, job.kb = 1;
  int r = unpack_data(e, dev, arc);
  mx_fio_remove(arc);
  if (job.cancel) {
    job.phase = PH_CANCELLED;
    return -1;
  }
  if (r) {
    job.phase = PH_FAILED;
    mx_printf("store: install: %s\n", job.message);
    return -1;
  }
  e->on_dev = dev;
  job.phase = PH_DONE;
  return 0;
}

/* The store's own update writes MXSTORE.ELF while it runs from it: the VM
   reads the program's pages from its ELF, and drops clean ones to read them
   again later, so all of them (text, rodata, data: up to .bss) are made
   resident and kept so first, until the store ends (it runs the new one
   from its next start) */
extern char __text_start[], __bss_start[];
static void pin_self(void) {
  static int pinned;
  if (!pinned && mx_vm_active())
    pinned = mx_vm_pin(__text_start, (u32)(__bss_start - __text_start), 0) == 0;
  mx_printf("store: its own pages %s\n", pinned ? "pinned" : "not pinned");
}

int install_entry(int ei, int dev) {
  struct entry *e = &entries[ei];
  if (e->kind == K_DATA)
    return install_data(e, dev);
  if (e->update != U_NONE)
    return fail("mx OS's own parts are installed from the PC for now");
  /* a copy put there by hand (no .store) is the user's: replaced only when
     they said so */
  {
    struct record r = {0};
    char gi[128];
    path_of(gi, sizeof gi, dev, e->id, "game.ini");
    if (record_read(dev, e->id, &r) < 0 && exists(gi) && !job.replace)
      return fail("A copy put there by hand: not replaced");
  }
  char zip[128];
  mx_snprintf(zip, sizeof zip, "%s/.store/%s.zip", sdevs[dev].base, e->id);
  if (download(e, zip))
    return -1;
  if (!strcmp(e->id, "mxstore"))
    pin_self();
  int r;

  /* its directory */
  int zfd = mx_fio_open(zip, MX_FIO_RDONLY);
  if (zfd < 0)
    return fail("Cannot read the package");
  u32 size = (u32)mx_fio_lseek(zfd, 0, MX_FIO_SEEK_END);
  u8 tail[22 + 1024];
  u32 tn = size < sizeof tail ? size : sizeof tail;
  u8 *eocd = 0;
  if (read_at(zfd, size - tn, tail, tn) == 0)
    for (int i = (int)tn - 22; i >= 0 && !eocd; i--)
      if (le32(tail + i) == 0x06054b50)
        eocd = tail + i;
  u32 cd_n = eocd ? le16(eocd + 10) : 0, cd_size = eocd ? le32(eocd + 12) : 0, cd_off = eocd ? le32(eocd + 16) : 0;
  u8 *cd = 0;
  if (!eocd || cd_size > CD_MAX || cd_off + cd_size > size || !(cd = malloc(cd_size)) ||
      read_at(zfd, cd_off, cd, cd_size)) {
    free(cd);
    mx_fio_close(zfd);
    mx_fio_remove(zip);
    return fail("Not a package this store can read");
  }

  /* what the store wrote before, then the new list */
  static struct record old, now;
  static char old_names[NAMES], now_names[NAMES];
  old.names = old_names, now.names = now_names;
  record_read(dev, e->id, &old);
  now.used = 0, now.n = 0, now.partial = 1;
  mx_snprintf(now.version, sizeof now.version, "%s", e->version);
  char top[128];
  path_of(top, sizeof top, dev, e->id, "");
  mkdirs(top);
  mx_fio_mkdir(top, 0777);
  /* the files of both until this one is done */
  for (const char *s = old.names; s < old.names + old.used; s += strlen(s) + 1)
    record_add(&now, s);
  record_write(dev, e->id, &now);
  now.used = 0, now.n = 0;

  job.phase = PH_UNPACK, job.files = 0, job.nfiles = cd_n;
  static char pkg_ini[2048];
  pkg_ini[0] = 0;
  u32 idl = (u32)strlen(e->id);
  r = 0;
  u8 *p = cd;
  for (u32 i = 0; i < cd_n && !r; i++) {
    if (p + 46 > cd + cd_size || le32(p) != 0x02014b50) {
      r = -1;
      break;
    }
    u32 flags = le16(p + 8), method = le16(p + 10), crc = le32(p + 16), cs = le32(p + 20), us = le32(p + 24);
    u32 nl = le16(p + 28), xl = le16(p + 30), cl = le16(p + 32), local = le32(p + 42);
    char name[256];
    if (nl >= sizeof name || p + 46 + nl > cd + cd_size) {
      r = -1;
      break;
    }
    memcpy(name, p + 46, nl);
    name[nl] = 0;
    p += 46 + nl + xl + cl;
    job.files = i + 1;
    if (!name_ok(name, e->id) || (flags & 1) || (method != 0 && method != 8) || cs == 0xFFFFFFFF ||
        us == 0xFFFFFFFF) {
      mx_printf("store: %s: %s left out\n", e->id, name);
      continue;
    }
    const char *rel = name + idl + 1;
    if (!*rel || name[nl - 1] == '/') {
      char dp[320];
      mx_snprintf(dp, sizeof dp, "%s/%s", sdevs[dev].base, name);
      mkdirs(dp);
      continue;
    }
    if (!strcmp(rel, "game.ini")) {
      r = unpack_one(zfd, local, method, cs, us, crc, 0, pkg_ini, sizeof pkg_ini - 1);
      continue;
    }
    if (!strcmp(rel, ".store"))
      continue;
    char fp[320];
    mx_snprintf(fp, sizeof fp, "%s/%s", sdevs[dev].base, name);
    r = unpack_one(zfd, local, method, cs, us, crc, fp, 0, 0);
    if (r)
      mx_snprintf(job.message, sizeof job.message, "%s", rel);
    else
      record_add(&now, rel);
  }
  free(cd);
  mx_fio_close(zfd);
  mx_fio_remove(zip);
  if (r)
    return fail(job.message[0] ? "A file of the package did not unpack" : "Not a package this store can read");

  /* the cover and game.ini, from the index */
  if (e->cover.path) {
    u32 n;
    u8 *c = pic_bytes(&e->cover, COVER_BYTES, &n);
    char cp[128];
    path_of(cp, sizeof cp, dev, e->id, "coverhd.t8");
    if (c && n == COVER_BYTES && write_file(cp, c, n) == 0)
      record_add(&now, "coverhd.t8");
    free(c);
  }
  if (write_ini(dev, e, pkg_ini[0] ? pkg_ini : 0))
    return fail("Cannot write its game.ini");
  record_add(&now, "game.ini");
  /* what the old version had and this one does not */
  for (const char *s = old.names; s < old.names + old.used; s += strlen(s) + 1)
    if (!record_has(&now, s)) {
      char fp[320];
      path_of(fp, sizeof fp, dev, e->id, s);
      mx_fio_remove(fp);
      /* and its directories, when that left them empty (rmdir refuses others) */
      for (char *sl; (sl = strrchr(fp, '/')) && sl > fp + strlen(top);) {
        *sl = 0;
        mx_fio_rmdir(fp);
      }
    }
  now.partial = 0;
  record_write(dev, e->id, &now);
  e->on_dev = dev, e->partial = 0;
  mx_snprintf(e->on_version, sizeof e->on_version, "%s", e->version);
  job.phase = PH_DONE;
  mx_printf("store: %s %s installed in %s: %d files\n", e->id, e->version, sdevs[dev].base, now.n);
  return 0;
}

int remove_entry(int ei) {
  struct entry *e = &entries[ei];
  int dev = e->on_dev;
  job.phase = PH_UNPACK, job.kb = 0;
  if (e->kind == K_DATA)
    return fail("Game data is the user's: remove it with Files");
  if (dev < 0)
    return fail("It is not installed");
  static struct record r;
  static char names[NAMES];
  r.names = names;
  if (record_read(dev, e->id, &r))
    return fail("Installed by hand: remove it with Files");
  job.nfiles = (u32)r.n, job.files = 0;
  /* the files, then the directories they leave empty (deepest first) */
  const char *s = r.names;
  for (int i = 0; i < r.n; i++, s += strlen(s) + 1) {
    char fp[320];
    path_of(fp, sizeof fp, dev, e->id, s);
    mx_fio_remove(fp);
    job.files = (u32)i + 1;
  }
  char top[128];
  path_of(top, sizeof top, dev, e->id, "");
  s = r.names;
  for (int i = 0; i < r.n; i++, s += strlen(s) + 1) {
    char fp[320];
    path_of(fp, sizeof fp, dev, e->id, s);
    for (char *sl; (sl = strrchr(fp, '/')) && sl > fp + strlen(top);) {
      *sl = 0;
      mx_fio_rmdir(fp);
    }
  }
  char p[128];
  path_of(p, sizeof p, dev, e->id, ".store");
  mx_fio_remove(p);
  path_of(p, sizeof p, dev, e->id, 0);
  mx_fio_rmdir(p); /* only when nothing of the user's is left */
  e->on_dev = -1, e->on_version[0] = 0;
  job.phase = PH_DONE;
  return 0;
}
