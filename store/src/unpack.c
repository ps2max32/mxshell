/* A data entry's archive (kind=data, apps/mxstore docs/index-format.md "Game
   data"): the original archive as its makers gave it out, unpacked here into
   the data directory, only the files its take= names.

   take= is a list, "," between its items, of "<path in the archive> <path
   in the data directory>"; a path that ends with "/" takes all below it. A
   path goes into an archive inside the archive with "/" too:
   "resource.1/ID1/PAK0.PAK id1/pak0.pak" is ID1\PAK0.PAK of the LHA archive
   resource.1 of the zip. Names are matched without case, "\" as "/".

   The archives: zip (stored, deflated; also with a program before it, a
   self-extracting one), LHA (-lh0-, -lh5-, headers of level 0 and 1; also
   with a DOS program before it), tar, gzip (a .tar.gz), and makeself (a
   shell script with a .tar.gz behind it). A zip is read by its directory
   (random access: only at the top); all others as a stream, so they can be
   inside another one.

   A file that is already there is left alone: it is the user's (a full
   version's pak0.pak, say). A file this left half written (an error, Stop)
   is removed. */
#include "store.h"

#include <mx/mx.h>
#include <mx/sif.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CHUNK (32u << 10)
#define PEEK (32u << 10) /* how far a program before an archive may go */
#define MAX_RULES 16
#define DEPTH 4

/* ---- take= ---- */
struct rule {
  char src[96], dst[96];
  int dir, hit;
};
static struct rule rules[MAX_RULES];
static int nrules;
static const struct entry *ue;
static int udev;
static u32 written; /* files */
static char werr[96];

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* a name of the archive as the rules compare it: "/" for "\", no leading "./" or "/" */
static void norm(char *s) {
  for (char *p = s; *p; p++)
    if (*p == '\\')
      *p = '/';
  while (s[0] == '.' && s[1] == '/')
    memmove(s, s + 2, strlen(s + 2) + 1);
  while (s[0] == '/')
    memmove(s, s + 1, strlen(s));
}
static int prefix_nc(const char *s, const char *p) {
  for (; *p; s++, p++)
    if (lower(*s) != lower(*p))
      return 0;
  return 1;
}
static int eq_nc(const char *a, const char *b) { return strlen(a) == strlen(b) && prefix_nc(a, b); }
/* a path in the data directory: relative, no "..", ":" or "\" */
static int dst_ok(const char *s) {
  return *s && *s != '/' && !strstr(s, "..") && !strchr(s, ':') && !strchr(s, '\\') && !strstr(s, "//");
}

static int rules_parse(const char *take) {
  nrules = 0;
  while (take && *take) {
    while (*take == ' ' || *take == ',')
      take++;
    if (!*take)
      break;
    const char *end = strchr(take, ',');
    u32 n = end ? (u32)(end - take) : (u32)strlen(take);
    char item[200];
    if (n >= sizeof item || nrules == MAX_RULES)
      return -1;
    memcpy(item, take, n), item[n] = 0;
    char *sp = strchr(item, ' ');
    if (!sp)
      return -1;
    *sp = 0;
    struct rule *r = &rules[nrules++];
    mx_snprintf(r->src, sizeof r->src, "%s", item);
    mx_snprintf(r->dst, sizeof r->dst, "%s", sp + 1);
    norm(r->src);
    r->dir = r->src[0] && r->src[strlen(r->src) - 1] == '/';
    r->hit = 0;
    if (!dst_ok(r->dst) || (r->dir && r->dst[strlen(r->dst) - 1] != '/'))
      return -1;
    take = end;
  }
  return nrules ? 0 : -1;
}

/* what a member of an archive is to the rules: 1 a file to take (its path
   in the data directory in dst), 2 an archive a rule goes into, 0 neither */
static int wanted(const char *name, char *dst, int len) {
  for (int i = 0; i < nrules; i++) {
    struct rule *r = &rules[i];
    if (r->dir) {
      u32 l = (u32)strlen(r->src);
      if (strlen(name) > l && prefix_nc(name, r->src)) {
        const char *rest = name + l;
        if (strstr(rest, "..") || strchr(rest, ':'))
          continue;
        mx_snprintf(dst, len, "%s%s", r->dst, rest);
        r->hit = 1;
        return 1;
      }
    } else if (eq_nc(name, r->src)) {
      mx_snprintf(dst, len, "%s", r->dst);
      r->hit = 1;
      return 1;
    }
  }
  u32 l = (u32)strlen(name);
  for (int i = 0; i < nrules; i++)
    if (strlen(rules[i].src) > l + 1 && prefix_nc(rules[i].src, name) && rules[i].src[l] == '/')
      return 2;
  return 0;
}

/* ---- streams ---- */
struct in {
  int (*read)(struct in *, u8 *, u32); /* bytes, 0 at the end, < 0 an error */
};

/* a range of the archive file; its position is the progress */
struct fin {
  struct in in;
  int fd;
  u32 pos, end;
};
static int fin_read(struct in *s, u8 *b, u32 n) {
  struct fin *f = (struct fin *)s;
  if (job.cancel)
    return -1;
  if (n > f->end - f->pos)
    n = f->end - f->pos;
  if (!n)
    return 0;
  int r = mx_fio_read(f->fd, b, n);
  if (r <= 0)
    return -1;
  f->pos += (u32)r;
  job.files = f->pos >> 10;
  return r;
}

/* raw deflate of another stream: a zip member, a gzip member after its header */
struct zin {
  struct in in;
  struct in *src;
  z_stream z;
  int end;
  u32 crc, want_crc, check; /* a zip member's CRC-32 */
  u8 buf[CHUNK];
};
static int zin_read(struct in *s, u8 *b, u32 n) {
  struct zin *z = (struct zin *)s;
  if (z->end)
    return 0;
  z->z.next_out = b, z->z.avail_out = n;
  while (z->z.avail_out == n) {
    if (!z->z.avail_in) {
      int r = z->src->read(z->src, z->buf, CHUNK);
      if (r < 0)
        return -1;
      z->z.next_in = z->buf, z->z.avail_in = (u32)r;
    }
    int had = (int)z->z.avail_in;
    int zr = inflate(&z->z, Z_NO_FLUSH);
    if (zr == Z_STREAM_END) {
      z->end = 1;
      break;
    }
    if (zr != Z_OK && !(zr == Z_BUF_ERROR && had))
      return -1;
    if (!had && z->z.avail_out == n)
      return -1; /* the data ended before the stream */
  }
  u32 got = n - z->z.avail_out;
  if (z->check) {
    z->crc = (u32)crc32(z->crc, b, got);
    if (z->end && z->crc != z->want_crc)
      return -1;
  }
  return (int)got;
}
static struct zin *zin_new(struct in *src) {
  struct zin *z = malloc(sizeof *z);
  if (!z)
    return 0;
  memset(z, 0, sizeof *z - CHUNK);
  z->in.read = zin_read, z->src = src;
  if (inflateInit2(&z->z, -15) != Z_OK) {
    free(z);
    return 0;
  }
  return z;
}
static void zin_free(struct zin *z) {
  if (z) {
    inflateEnd(&z->z);
    free(z);
  }
}

/* a stream with a look ahead, for the archives read as one */
struct br {
  struct in in; /* reads through the buffer */
  struct in *s;
  u8 *buf;
  u32 pos, len;
  int eof;
};
static int br_fill(struct br *b, u32 need) {
  if (b->pos + need <= b->len)
    return 0;
  if (b->pos) {
    memmove(b->buf, b->buf + b->pos, b->len - b->pos);
    b->len -= b->pos, b->pos = 0;
  }
  while (b->len < need && b->len < PEEK && !b->eof) {
    int r = b->s->read(b->s, b->buf + b->len, PEEK - b->len);
    if (r < 0)
      return -1;
    if (r == 0)
      b->eof = 1;
    b->len += (u32)r;
  }
  return b->len >= need ? 0 : 1;
}
static int br_read(struct in *s, u8 *out, u32 n) {
  struct br *b = (struct br *)s;
  if (b->pos < b->len) {
    u32 k = b->len - b->pos < n ? b->len - b->pos : n;
    memcpy(out, b->buf + b->pos, k);
    b->pos += k;
    return (int)k;
  }
  if (b->eof)
    return 0;
  return b->s->read(b->s, out, n);
}
static int br_skip(struct br *b, u32 n) {
  static u8 junk[CHUNK];
  while (n) {
    int r = br_read(&b->in, junk, n < CHUNK ? n : CHUNK);
    if (r <= 0)
      return -1;
    n -= (u32)r;
  }
  return 0;
}
static struct br *br_new(struct in *s) {
  struct br *b = malloc(sizeof *b);
  u8 *buf = malloc(PEEK);
  if (!b || !buf) {
    free(b), free(buf);
    return 0;
  }
  memset(b, 0, sizeof *b);
  b->in.read = br_read, b->s = s, b->buf = buf;
  return b;
}
static void br_free(struct br *b) {
  if (b)
    free(b->buf), free(b);
}

/* n bytes of a stream, no more */
struct lin {
  struct in in;
  struct in *s;
  u32 left;
};
static int lin_read(struct in *s, u8 *b, u32 n) {
  struct lin *l = (struct lin *)s;
  if (n > l->left)
    n = l->left;
  if (!n)
    return 0;
  int r = l->s->read(l->s, b, n);
  if (r <= 0)
    return -1; /* shorter than its header says */
  l->left -= (u32)r;
  return r;
}

/* ---- a file taken ---- */
static int exists(const char *p) {
  int fd = mx_fio_open(p, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  mx_fio_close(fd);
  return 1;
}
static int walk(struct in *s, const char *base, int depth);
static u8 obuf[CHUNK];

/* a member: taken, gone into, or passed over (skip: how to pass over it
   without reading, 0 to read it) */
static int member(const char *base, const char *raw, struct in *data, u32 size, int depth) {
  char name[256], dst[160];
  mx_snprintf(name, sizeof name, "%s%s", base, raw);
  norm(name);
  int w = wanted(name, dst, sizeof dst);
  if (w == 2) {
    char nb[260];
    mx_snprintf(nb, sizeof nb, "%s/", name);
    return depth < DEPTH ? walk(data, nb, depth + 1) : -1;
  }
  if (w != 1) {
    /* read through: a stream cannot jump */
    for (;;) {
      int r = data->read(data, obuf, CHUNK);
      if (r <= 0)
        return r;
    }
  }
  char path[256];
  data_path(path, sizeof path, ue, udev, dst);
  if (exists(path)) {
    mx_printf("store: %s: %s is there: kept\n", ue->id, dst);
    for (;;) {
      int r = data->read(data, obuf, CHUNK);
      if (r <= 0)
        return r;
    }
  }
  mkdirs(path);
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_snprintf(werr, sizeof werr, "Cannot write %s", dst);
    return -1;
  }
  u32 n = 0;
  int r;
  aw_begin(fd);
  while ((r = data->read(data, obuf, CHUNK)) > 0) {
    if (aw_write(obuf, (u32)r) != 0) {
      r = -1;
      break;
    }
    n += (u32)r;
  }
  if (aw_end())
    r = -1;
  mx_fio_close(fd);
  if (r < 0 || (size != 0xFFFFFFFF && n != size)) {
    mx_fio_remove(path);
    if (!werr[0])
      mx_snprintf(werr, sizeof werr, "%s did not unpack", dst);
    return -1;
  }
  written++;
  mx_printf("store: %s: %s (%u bytes)\n", ue->id, dst, n);
  return 0;
}

/* ---- LHA: -lh0- and -lh5- (Okumura's ar002, LHa's decoder) ---- */
#define DICBIT 13
#define DICSIZ (1u << DICBIT)
#define NC (255 + 256 + 2 - 3) /* 510 */
#define NP (DICBIT + 1)
#define NT 19
#define NPT 19
#define CBIT 9
#define TBIT 5
#define PBIT 4

struct lha {
  struct in in;
  struct br *b;
  u32 packed, left; /* packed bytes left, output bytes left */
  int lh5, err;
  u16 crc, want_crc;
  u32 bitbuf, subbitbuf;
  int bitcount;
  u32 blocksize;
  u32 r;              /* the window's position */
  u32 copy_n, copy_i; /* a match not yet all written */
  u8 c_len[NC], pt_len[NPT];
  u16 c_table[4096], pt_table[256], left_[2 * NC - 1], right_[2 * NC - 1];
  u8 dic[DICSIZ];
};
static u16 crc16_table[256];
static void crc16_init(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 c = i;
    for (int k = 0; k < 8; k++)
      c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1;
    crc16_table[i] = (u16)c;
  }
}
static int lha_byte(struct lha *h) {
  if (!h->packed)
    return 0;
  u8 c;
  if (br_fill(h->b, 1) || br_read(&h->b->in, &c, 1) != 1) {
    h->err = 1;
    return 0;
  }
  h->packed--;
  return c;
}
static void fillbuf(struct lha *h, int n) {
  h->bitbuf = (h->bitbuf << n) & 0xFFFF;
  while (n > h->bitcount) {
    n -= h->bitcount;
    h->bitbuf |= (h->subbitbuf << n) & 0xFFFF;
    h->subbitbuf = (u32)lha_byte(h);
    h->bitcount = 8;
  }
  h->bitcount -= n;
  h->bitbuf |= h->subbitbuf >> h->bitcount;
  h->subbitbuf &= (1u << h->bitcount) - 1;
}
static u32 getbits(struct lha *h, int n) {
  u32 x = h->bitbuf >> (16 - n);
  fillbuf(h, n);
  return x;
}
static int make_table(struct lha *h, int nchar, const u8 *bitlen, int tablebits, u16 *table) {
  u32 count[17], weight[17], start[18];
  for (int i = 1; i <= 16; i++)
    count[i] = 0;
  for (int i = 0; i < nchar; i++) {
    if (bitlen[i] > 16)
      return -1;
    count[bitlen[i]]++;
  }
  start[1] = 0;
  for (int i = 1; i <= 16; i++)
    start[i + 1] = start[i] + (count[i] << (16 - i));
  if (start[17] != 1u << 16)
    return -1;
  int jutbits = 16 - tablebits, i;
  for (i = 1; i <= tablebits; i++) {
    start[i] >>= jutbits;
    weight[i] = 1u << (tablebits - i);
  }
  for (; i <= 16; i++)
    weight[i] = 1u << (16 - i);
  u32 k = start[tablebits + 1] >> jutbits;
  while (k < (1u << tablebits))
    table[k++] = 0;
  u32 avail = (u32)nchar, mask = 1u << (15 - tablebits);
  for (int ch = 0; ch < nchar; ch++) {
    u32 len = bitlen[ch];
    if (!len)
      continue;
    u32 nextcode = start[len] + weight[len];
    if ((int)len <= tablebits) {
      if (nextcode > (1u << tablebits))
        return -1;
      for (u32 j = start[len]; j < nextcode; j++)
        table[j] = (u16)ch;
    } else {
      u32 kk = start[len];
      u16 *p = &table[kk >> jutbits];
      for (int j = (int)len - tablebits; j; j--) {
        if (*p == 0) {
          if (avail >= 2 * NC - 1)
            return -1;
          h->right_[avail] = h->left_[avail] = 0;
          *p = (u16)avail++;
        }
        p = kk & mask ? &h->right_[*p] : &h->left_[*p];
        kk <<= 1;
      }
      *p = (u16)ch;
    }
    start[len] = nextcode;
  }
  return 0;
}
static int read_pt_len(struct lha *h, int nn, int nbit, int i_special) {
  int n = (int)getbits(h, nbit);
  if (n == 0) {
    u32 c = getbits(h, nbit);
    memset(h->pt_len, 0, (u32)nn);
    for (int i = 0; i < 256; i++)
      h->pt_table[i] = (u16)c;
    return 0;
  }
  if (n > nn)
    return -1;
  int i = 0;
  while (i < n) {
    u32 c = h->bitbuf >> 13;
    if (c == 7) {
      u32 mask = 1u << 12;
      while (mask & h->bitbuf) {
        mask >>= 1, c++;
        if (c > 16)
          return -1;
      }
    }
    fillbuf(h, c < 7 ? 3 : (int)c - 3);
    h->pt_len[i++] = (u8)c;
    if (i == i_special) {
      int z = (int)getbits(h, 2);
      while (--z >= 0 && i < nn)
        h->pt_len[i++] = 0;
    }
  }
  while (i < nn)
    h->pt_len[i++] = 0;
  return make_table(h, nn, h->pt_len, 8, h->pt_table);
}
static int read_c_len(struct lha *h) {
  int n = (int)getbits(h, CBIT);
  if (n == 0) {
    u32 c = getbits(h, CBIT);
    memset(h->c_len, 0, NC);
    for (int i = 0; i < 4096; i++)
      h->c_table[i] = (u16)c;
    return 0;
  }
  if (n > NC)
    return -1;
  int i = 0;
  while (i < n) {
    u32 c = h->pt_table[h->bitbuf >> 8];
    if (c >= NT) {
      u32 mask = 1u << 7;
      do {
        c = h->bitbuf & mask ? h->right_[c] : h->left_[c];
        mask >>= 1;
      } while (c >= NT && mask);
      if (c >= NT)
        return -1;
    }
    fillbuf(h, h->pt_len[c]);
    if (c <= 2) {
      int z = c == 0 ? 1 : c == 1 ? (int)getbits(h, 4) + 3 : (int)getbits(h, CBIT) + 20;
      while (--z >= 0 && i < NC)
        h->c_len[i++] = 0;
    } else
      h->c_len[i++] = (u8)(c - 2);
  }
  while (i < NC)
    h->c_len[i++] = 0;
  return make_table(h, NC, h->c_len, 12, h->c_table);
}
static int decode_c(struct lha *h) {
  if (h->blocksize == 0) {
    h->blocksize = getbits(h, 16);
    if (read_pt_len(h, NT, TBIT, 3) || read_c_len(h) || read_pt_len(h, NP, PBIT, -1))
      return -1;
  }
  h->blocksize--;
  u32 j = h->c_table[h->bitbuf >> 4];
  if (j >= NC) {
    u32 mask = 1u << 3;
    do {
      j = h->bitbuf & mask ? h->right_[j] : h->left_[j];
      mask >>= 1;
    } while (j >= NC && mask);
    if (j >= NC)
      return -1;
  }
  fillbuf(h, h->c_len[j]);
  return (int)j;
}
static int decode_p(struct lha *h) {
  u32 j = h->pt_table[h->bitbuf >> 8];
  if (j >= NP) {
    u32 mask = 1u << 7;
    do {
      j = h->bitbuf & mask ? h->right_[j] : h->left_[j];
      mask >>= 1;
    } while (j >= NP && mask);
    if (j >= NP)
      return -1;
  }
  fillbuf(h, h->pt_len[j]);
  if (j)
    j = (1u << (j - 1)) + getbits(h, (int)j - 1);
  return (int)j;
}
static int lha_read(struct in *s, u8 *out, u32 n) {
  struct lha *h = (struct lha *)s;
  if (job.cancel)
    return -1;
  if (n > h->left)
    n = h->left;
  u32 k = 0;
  if (!h->lh5) {
    if (n && (br_fill(h->b, 1) < 0 || (int)(k = (u32)br_read(&h->b->in, out, n)) <= 0))
      return -1;
  } else {
    while (k < n) {
      if (h->copy_n) {
        u8 c = h->dic[h->copy_i];
        h->copy_i = (h->copy_i + 1) & (DICSIZ - 1);
        h->copy_n--;
        out[k++] = h->dic[h->r] = c;
        h->r = (h->r + 1) & (DICSIZ - 1);
        continue;
      }
      int c = decode_c(h);
      if (c < 0 || h->err)
        return -1;
      if (c <= 255) {
        out[k++] = h->dic[h->r] = (u8)c;
        h->r = (h->r + 1) & (DICSIZ - 1);
      } else {
        int p = decode_p(h);
        if (p < 0)
          return -1;
        h->copy_n = (u32)c - (256 - 3);
        h->copy_i = (h->r - (u32)p - 1) & (DICSIZ - 1);
      }
    }
  }
  for (u32 i = 0; i < k; i++)
    h->crc = (u16)(crc16_table[(h->crc ^ out[i]) & 0xFF] ^ (h->crc >> 8));
  h->left -= k;
  if (!h->left && h->crc != h->want_crc)
    return -1;
  return (int)k;
}

static int lha_walk(struct br *b, const char *base, int depth) {
  /* the first header: after a program (an SFX), at most PEEK in */
  if (br_fill(b, 64) < 0)
    return -1;
  u32 at = 0;
  for (;; at++) {
    if (br_fill(b, at + 24) < 0)
      return -1;
    if (b->pos + at + 24 > b->len)
      return -1;
    const u8 *p = b->buf + b->pos + at;
    if (p[2] == '-' && p[3] == 'l' && p[4] == 'h' && p[6] == '-' && p[20] <= 1 && p[0] >= 22) {
      if (br_fill(b, at + p[0] + 2u) < 0 || b->pos + at + p[0] + 2u > b->len)
        return -1;
      p = b->buf + b->pos + at;
      u8 sum = 0;
      for (u32 i = 2; i < p[0] + 2u; i++)
        sum = (u8)(sum + p[i]);
      if (sum == p[1])
        break;
    }
  }
  b->pos += at;
  static struct lha h;
  crc16_init();
  for (;;) {
    if (br_fill(b, 1) < 0)
      return -1;
    if (b->pos >= b->len || b->buf[b->pos] == 0)
      return 0; /* the end */
    u32 hs = b->buf[b->pos];
    if (br_fill(b, hs + 2) || hs < 22)
      return -1;
    u8 hd[260];
    memcpy(hd, b->buf + b->pos, hs + 2);
    b->pos += hs + 2;
    int level = hd[20];
    u32 packed = hd[7] | hd[8] << 8 | hd[9] << 16 | (u32)hd[10] << 24;
    u32 size = hd[11] | hd[12] << 8 | hd[13] << 16 | (u32)hd[14] << 24;
    u32 nl = hd[21];
    if (22 + nl + 2 > hs + 2 || level > 1)
      return -1;
    char name[256];
    memcpy(name, hd + 22, nl), name[nl] = 0;
    u16 crc = (u16)(hd[22 + nl] | hd[23 + nl] << 8);
    if (level == 1) {
      /* the extended headers, counted in packed: a directory's name is one */
      u32 next = hd[hs] | hd[hs + 1] << 8;
      while (next) {
        if (br_fill(b, next) || next < 3 || next > packed)
          return -1;
        const u8 *x = b->buf + b->pos;
        if (x[0] == 2 && next - 3 + nl < 250) {
          /* a directory: its parts end with 0xFF */
          char dir[256];
          u32 dl = next - 3;
          memcpy(dir, x + 1, dl);
          for (u32 i = 0; i < dl; i++)
            if ((u8)dir[i] == 0xFF)
              dir[i] = '/';
          if (dl && dir[dl - 1] != '/')
            dir[dl++] = '/';
          memcpy(dir + dl, name, nl + 1);
          memcpy(name, dir, dl + nl + 1);
        }
        packed -= next;
        u32 nn = x[next - 2] | x[next - 1] << 8;
        b->pos += next;
        next = nn;
      }
    }
    int lh0 = !memcmp(hd + 2, "-lh0-", 5) || !memcmp(hd + 2, "-lhd-", 5), lh5 = !memcmp(hd + 2, "-lh5-", 5);
    char full[256], dst[160];
    mx_snprintf(full, sizeof full, "%s%s", base, name);
    norm(full);
    if (!(lh0 || lh5) || !memcmp(hd + 2, "-lhd-", 5) || !wanted(full, dst, sizeof dst)) {
      if (br_skip(b, packed))
        return -1;
      continue;
    }
    memset(&h, 0, offsetof(struct lha, c_len));
    h.in.read = lha_read, h.b = b, h.packed = packed, h.left = size, h.lh5 = lh5, h.want_crc = crc;
    if (lh5)
      fillbuf(&h, 16);
    else
      h.packed = 0; /* lha_read reads it as it is */
    int r = member(base, name, &h.in, size, depth);
    if (r)
      return -1;
    if (h.packed && br_skip(b, h.packed))
      return -1; /* the rest of the packed bits */
    if (lh0 && h.left)
      return -1;
  }
}

/* ---- tar ---- */
static u32 octal(const u8 *s, int n) {
  u32 v = 0;
  int i = 0;
  while (i < n && s[i] == ' ')
    i++; /* old GNU tar: spaces before it */
  for (; i < n && s[i] >= '0' && s[i] <= '7'; i++)
    v = v * 8 + (u32)(s[i] - '0');
  return v;
}
static int tar_walk(struct br *b, const char *base, int depth) {
  static char longname[256];
  int have_long = 0;
  for (;;) {
    u8 h[512];
    int f = br_fill(b, 512);
    if (f)
      return f < 0 ? -1 : 0; /* the end, without its zero blocks */
    memcpy(h, b->buf + b->pos, 512);
    b->pos += 512;
    if (!h[0])
      return 0; /* a zero block: the end */
    u32 size = octal(h + 124, 12), pad = (512 - (size & 511)) & 511;
    char name[256];
    if (have_long)
      mx_snprintf(name, sizeof name, "%s", longname), have_long = 0;
    else if (!memcmp(h + 257, "ustar", 5) && h[345])
      mx_snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h + 0);
    else
      mx_snprintf(name, sizeof name, "%.100s", (const char *)h);
    if (h[156] == 'L') {
      /* GNU's long name: the next member's */
      u32 n = size < sizeof longname - 1 ? size : sizeof longname - 1;
      if (br_fill(b, n))
        return -1;
      memcpy(longname, b->buf + b->pos, n), longname[n] = 0;
      b->pos += n;
      if (br_skip(b, size - n + pad))
        return -1;
      have_long = 1;
      continue;
    }
    if (h[156] != '0' && h[156] != 0) {
      if (br_skip(b, size + pad))
        return -1;
      continue;
    }
    char full[256], dst[160];
    mx_snprintf(full, sizeof full, "%s%s", base, name);
    norm(full);
    if (!wanted(full, dst, sizeof dst)) {
      if (br_skip(b, size + pad))
        return -1;
      continue;
    }
    struct lin l = {{lin_read}, &b->in, size};
    if (member(base, name, &l.in, size, depth) || l.left || br_skip(b, pad))
      return -1;
  }
}

/* a gzip member's header at the buffer's position passed over (zlib here is
   built without gzip: its deflate is read raw) */
static int gzip_skip(struct br *b) {
  if (br_fill(b, 10))
    return -1;
  const u8 *p = b->buf + b->pos;
  u32 flags = p[3], at = 10;
  if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8 || (flags & 0xE0))
    return -1;
  if (flags & 4) { /* FEXTRA */
    if (br_fill(b, at + 2))
      return -1;
    at += 2 + (b->buf[b->pos + at] | b->buf[b->pos + at + 1] << 8);
  }
  for (int k = 0; k < 2; k++)
    if (flags & (k ? 16 : 8)) /* FNAME, FCOMMENT: ends with a 0 */
      for (;;) {
        if (br_fill(b, at + 1))
          return -1;
        if (!b->buf[b->pos + at++])
          break;
      }
  if (flags & 2) /* FHCRC */
    at += 2;
  if (br_fill(b, at))
    return -1;
  b->pos += at;
  return 0;
}

/* ---- a zip read as a stream: its local headers one after the other (a zip
   inside an archive; also with a program before it, as PKLITE's
   self-extracting DN3DSW13.SHR of Duke Nukem 3D's shareware) ---- */
static u32 z16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 z32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static int zipstream_walk(struct br *b, const char *base, int depth) {
  for (;;) {
    int f = br_fill(b, 30);
    if (f < 0)
      return -1;
    if (f || z32(b->buf + b->pos) != 0x04034b50)
      return 0; /* the central directory, or the end */
    const u8 *h = b->buf + b->pos;
    u32 flags = z16(h + 6), method = z16(h + 8), crc = z32(h + 14), cs = z32(h + 18), us = z32(h + 22);
    u32 nl = z16(h + 26), xl = z16(h + 28);
    if (nl >= 256 || br_fill(b, 30 + nl))
      return -1;
    char name[256];
    memcpy(name, b->buf + b->pos + 30, nl), name[nl] = 0;
    b->pos += 30 + nl;
    if (br_skip(b, xl))
      return -1;
    char full[256], dst[160];
    mx_snprintf(full, sizeof full, "%s%s", base, name);
    norm(full);
    int w = nl && name[nl - 1] != '/' ? wanted(full, dst, sizeof dst) : 0;
    if (flags & 8) {
      /* the sizes after the data: not readable as a stream */
      mx_snprintf(werr, sizeof werr, "%s: a zip this store cannot read as a stream", base);
      return -1;
    }
    if (!w) {
      if (br_skip(b, cs))
        return -1;
      continue;
    }
    if ((flags & 1) || (method != 0 && method != 8)) {
      mx_snprintf(werr, sizeof werr, "%s%s: a zip this store cannot read", base, name);
      return -1;
    }
    struct lin l = {{lin_read}, &b->in, cs};
    int r;
    if (method == 8) {
      struct zin *z = zin_new(&l.in);
      if (!z)
        return -1;
      z->check = 1, z->crc = (u32)crc32(0, 0, 0), z->want_crc = crc;
      r = member(base, name, &z->in, us, depth);
      zin_free(z);
    } else
      r = member(base, name, &l.in, us, depth);
    if (r || (l.left && br_skip(b, l.left)))
      return -1;
  }
}

/* ---- a stream: which archive it is ---- */
static int walk(struct in *s, const char *base, int depth) {
  struct br *b = br_new(s);
  if (!b)
    return -1;
  int r = -1;
  if (br_fill(b, 512) < 0)
    goto out;
  const u8 *p = b->buf;
  u32 n = b->len;
  if (n >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8) {
    struct zin *z = gzip_skip(b) ? 0 : zin_new(&b->in);
    if (z)
      r = walk(&z->in, base, depth + 1);
    zin_free(z);
  } else if (n >= 2 && p[0] == '#' && p[1] == '!') {
    /* makeself: a script, then a .tar.gz at the start of a line */
    br_fill(b, PEEK);
    for (u32 i = 0; i + 4 <= b->len; i++)
      if (b->buf[i] == '\n' && b->buf[i + 1] == 0x1F && b->buf[i + 2] == 0x8B && b->buf[i + 3] == 8) {
        b->pos = i + 1;
        struct zin *z = gzip_skip(b) ? 0 : zin_new(&b->in);
        if (z)
          r = walk(&z->in, base, depth + 1);
        zin_free(z);
        break;
      }
  } else if (n >= 262 && !memcmp(p + 257, "ustar", 5)) {
    r = tar_walk(b, base, depth);
  } else if (n >= 4 && z32(p) == 0x04034b50) {
    r = zipstream_walk(b, base, depth);
  } else if (n >= 2 && p[0] == 'M' && p[1] == 'Z' && br_fill(b, PEEK) >= 0) {
    /* a program, then a zip or an LHA archive: whichever starts first */
    u32 zip = 0xFFFFFFFF, lha = 0xFFFFFFFF;
    for (u32 i = 0; i + 7 <= b->len && zip == 0xFFFFFFFF && lha == 0xFFFFFFFF; i++) {
      if (z32(b->buf + i) == 0x04034b50)
        zip = i;
      else if (i >= 2 && b->buf[i] == '-' && b->buf[i + 1] == 'l' && b->buf[i + 2] == 'h' && b->buf[i + 4] == '-')
        lha = i - 2;
    }
    if (zip < lha) {
      b->pos = zip;
      r = zipstream_walk(b, base, depth);
    } else
      r = lha_walk(b, base, depth);
  } else if ((n >= 7 && p[2] == '-' && p[3] == 'l' && p[4] == 'h') || (n >= 2 && p[0] == 'M' && p[1] == 'Z')) {
    r = lha_walk(b, base, depth);
  } else if (!werr[0])
    mx_snprintf(werr, sizeof werr, "%s: not an archive this store reads", base[0] ? base : "The file");
out:
  br_free(b);
  return r;
}

/* ---- a zip, by its directory (the top only: random access) ---- */
static u32 le16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static int at(int fd, u32 off, void *buf, u32 n) {
  if (mx_fio_lseek(fd, (s32)off, MX_FIO_SEEK_SET) != (int)off)
    return -1;
  return mx_fio_read(fd, buf, n) == (int)n ? 0 : -1;
}
/* 1: not a zip */
static int zip_walk(int fd, u32 size) {
  u8 tail[22 + 1024];
  u32 tn = size < sizeof tail ? size : sizeof tail;
  u8 *eocd = 0;
  if (at(fd, size - tn, tail, tn))
    return -1;
  for (int i = (int)tn - 22; i >= 0 && !eocd; i--)
    if (le32(tail + i) == 0x06054b50)
      eocd = tail + i;
  if (!eocd)
    return 1;
  u32 cd_n = le16(eocd + 10), cd_size = le32(eocd + 12), cd_off = le32(eocd + 16);
  u32 eocd_at = size - tn + (u32)(eocd - tail);
  if (cd_size > eocd_at)
    return 1;
  /* a self-extracting zip may count from its own start: the difference */
  u32 delta = eocd_at - cd_size - cd_off;
  u8 *cd = cd_size <= (1u << 20) ? malloc(cd_size) : 0;
  if (!cd || at(fd, cd_off + delta, cd, cd_size)) {
    free(cd);
    return -1;
  }
  int r = 0;
  u8 *p = cd;
  for (u32 i = 0; i < cd_n && !r; i++) {
    if (p + 46 > cd + cd_size || le32(p) != 0x02014b50) {
      r = -1;
      break;
    }
    u32 flags = le16(p + 8), method = le16(p + 10), crc = le32(p + 16), cs = le32(p + 20), us = le32(p + 24);
    u32 nl = le16(p + 28), xl = le16(p + 30), cl = le16(p + 32), local = le32(p + 42) + delta;
    char name[256];
    if (nl >= sizeof name || p + 46 + nl > cd + cd_size) {
      r = -1;
      break;
    }
    memcpy(name, p + 46, nl), name[nl] = 0;
    p += 46 + nl + xl + cl;
    char full[256], dst[160];
    mx_snprintf(full, sizeof full, "%s", name);
    norm(full);
    if (!nl || name[nl - 1] == '/' || !wanted(full, dst, sizeof dst))
      continue;
    if ((flags & 1) || (method != 0 && method != 8)) {
      mx_snprintf(werr, sizeof werr, "%s: a zip this store cannot read", name);
      r = -1;
      break;
    }
    u8 h[30];
    if (at(fd, local, h, 30) || le32(h) != 0x04034b50) {
      r = -1;
      break;
    }
    static struct fin f;
    f.in.read = fin_read, f.fd = fd, f.pos = local + 30 + le16(h + 26) + le16(h + 28), f.end = f.pos + cs;
    if (f.end > size || mx_fio_lseek(fd, (s32)f.pos, MX_FIO_SEEK_SET) != (int)f.pos) {
      r = -1;
      break;
    }
    if (method == 8) {
      struct zin *z = zin_new(&f.in);
      if (!z) {
        r = -1;
        break;
      }
      z->check = 1, z->crc = (u32)crc32(0, 0, 0), z->want_crc = crc;
      r = member("", name, &z->in, us, 1);
      zin_free(z);
    } else {
      /* stored: its CRC by the size alone (the archive's SHA-256 matched) */
      r = member("", name, &f.in, us, 1);
    }
  }
  free(cd);
  return r;
}

int unpack_data(const struct entry *e, int dev, const char *archive) {
  ue = e, udev = dev, written = 0, werr[0] = 0;
  if (rules_parse(kv_get(e, "take"))) {
    mx_snprintf(job.message, sizeof job.message, "The index's take= cannot be read");
    return -1;
  }
  int fd = mx_fio_open(archive, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_snprintf(job.message, sizeof job.message, "Cannot read the archive");
    return -1;
  }
  u32 size = (u32)mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  job.files = 0, job.nfiles = size >> 10; /* KB of the archive, as the unpacking reads it */
  int r = zip_walk(fd, size);
  if (r == 1) {
    static struct fin f;
    f.in.read = fin_read, f.fd = fd, f.pos = 0, f.end = size;
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
    r = walk(&f.in, "", 0);
  }
  mx_fio_close(fd);
  job.files = job.nfiles;
  if (!r)
    for (int i = 0; i < nrules; i++)
      if (!rules[i].hit) {
        mx_snprintf(werr, sizeof werr, "%s is not in the archive", rules[i].src);
        r = -1;
      }
  if (r)
    mx_snprintf(job.message, sizeof job.message, "%s", werr[0] ? werr : "The archive did not unpack");
  else
    mx_printf("store: %s: %u files into its data directory\n", e->id, written);
  return r;
}
