/* MXSHELL2: its settings, key=value lines on the memory card, until the
   kernel has a settings store (PLAN.md, O1). Written safely without a
   rename (mxio has none): two files, mc0:/MXOS/MXSHELL2.CF0 and .CF1, each
   with a sequence number and an end line; a save writes the older one, a
   load takes the newer complete one. */
#include <mx/debug.h>
#include <mx/sif.h>
#include <string.h>

#include "pn.h"

#define MAX_KEYS 32

static struct {
  char key[24], value[40];
} kv[MAX_KEYS];
static int nkv;
static u32 seq;     /* of the file loaded */
static int last;    /* which file that was */

static const char *file(int i) { return i ? "mc0:/MXOS/MXSHELL2.CF1" : "mc0:/MXOS/MXSHELL2.CF0"; }

const char *cfg_get(const char *key, const char *def) {
  for (int i = 0; i < nkv; i++)
    if (!strcmp(kv[i].key, key))
      return kv[i].value;
  return def;
}

static void put(const char *key, u32 klen, const char *value, u32 vlen) {
  int i = 0;
  for (; i < nkv; i++)
    if (strlen(kv[i].key) == klen && !strncmp(kv[i].key, key, klen))
      break;
  if (i == nkv) {
    if (nkv == MAX_KEYS)
      return;
    nkv++;
  }
  klen = klen < sizeof kv[i].key - 1 ? klen : sizeof kv[i].key - 1;
  vlen = vlen < sizeof kv[i].value - 1 ? vlen : sizeof kv[i].value - 1;
  memcpy(kv[i].key, key, klen), kv[i].key[klen] = 0;
  memcpy(kv[i].value, value, vlen), kv[i].value[vlen] = 0;
}

void cfg_set(const char *key, const char *value) { put(key, strlen(key), value, strlen(value)); }

static char buf[2048];

/* the file's sequence number if it is complete, else 0 (contents in buf) */
static u32 read_file(int i) {
  int fd = mx_fio_open(file(i), MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  int n = mx_fio_read(fd, buf, sizeof buf - 1);
  mx_fio_close(fd);
  if (n < 16 || strncmp(buf, "mxshell2 ", 9) || strncmp(buf + n - 4, "end\n", 4))
    return 0;
  buf[n] = 0;
  u32 s = 0;
  for (const char *p = buf + 9; *p >= '0' && *p <= '9'; p++)
    s = s * 10 + (u32)(*p - '0');
  return s;
}

static void parse(void) {
  char *s = buf;
  while (*s && *s != '\n')
    s++;
  while (*s == '\n')
    s++;
  while (*s) {
    char *e = s, *eq = 0;
    while (*e && *e != '\n') {
      if (*e == '=' && !eq)
        eq = e;
      e++;
    }
    if (eq)
      put(s, (u32)(eq - s), eq + 1, (u32)(e - eq - 1));
    s = *e ? e + 1 : e;
  }
}

void cfg_load(void) {
  nkv = 0;
  u32 s0 = read_file(0), s1 = read_file(1);
  last = s1 > s0;
  seq = last ? s1 : s0;
  if (seq && read_file(last) == seq)
    parse();
}

void cfg_save(void) {
  char *e = buf;
  e += mx_snprintf(e, 32, "mxshell2 %u\n", seq + 1);
  for (int i = 0; i < nkv && e < buf + sizeof buf - 80; i++)
    e += mx_snprintf(e, 80, "%s=%s\n", kv[i].key, kv[i].value);
  e += mx_snprintf(e, 8, "end\n");
  int to = !last;
  int fd = mx_fio_open(file(to), MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return;
  int ok = mx_fio_write(fd, buf, (u32)(e - buf)) == (int)(e - buf);
  mx_fio_close(fd);
  if (ok)
    seq++, last = to;
}
