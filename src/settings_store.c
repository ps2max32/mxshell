/* MXSHELL2: its settings, kept in the kernel's store (<mx/setting.h>,
   kernel version 27), which writes them to the memory card itself. The
   shell holds a copy: cfg_set changes the copy, cfg_save hands the changed
   keys to the kernel (a video mode being tried is not saved until it is
   confirmed), and cfg_arg sets a key for this run only (test arguments,
   never saved). The apps (Files, Media) read the store too; only the shell
   may change it. */
#include <mx/debug.h>
#include <mx/setting.h>
#include <string.h>

#include "pn.h"

#define MAX_KEYS 48
#define DIRTY 1 /* changed here, not yet in the store */
#define TEMP 2  /* for this run only */

static struct {
  char key[MX_SETTING_KEY], value[MX_SETTING_VALUE];
  u8 flags;
} kv[MAX_KEYS];
static int nkv;
static u32 seen;    /* the store's counter at the last read */

static int find(const char *key) {
  for (int i = 0; i < nkv; i++)
    if (!strcmp(kv[i].key, key))
      return i;
  return -1;
}

static void copy(char *d, u32 n, const char *s) {
  u32 i = 0;
  for (; s[i] && i + 1 < n; i++)
    d[i] = s[i];
  d[i] = 0;
}

static int put(const char *key, const char *value) {
  int i = find(key);
  if (i < 0) {
    if (nkv == MAX_KEYS)
      return -1;
    i = nkv++;
    copy(kv[i].key, sizeof kv[i].key, key);
  }
  copy(kv[i].value, sizeof kv[i].value, value ? value : "");
  return i;
}

const char *cfg_get(const char *key, const char *def) {
  int i = find(key);
  return i >= 0 && kv[i].value[0] ? kv[i].value : def;
}

void cfg_set(const char *key, const char *value) {
  int i = put(key, value);
  if (i >= 0)
    kv[i].flags = DIRTY;
}

void cfg_arg(const char *key, const char *value) {
  int i = put(key, value);
  if (i >= 0)
    kv[i].flags = TEMP;
}

/* the store's changes since the last read, but over keys changed here */
void cfg_refresh(void) {
  static struct mx_setting s[16];
  u32 now = mx_setting_seq();
  while (seen != now) {
    int n = mx_setting_list(seen, s, 16);
    if (n <= 0)
      break;
    u32 top = seen;
    for (int k = 0; k < n; k++) {
      int i = find(s[k].key);
      if (!(i >= 0 && kv[i].flags))
        put(s[k].key, s[k].value);
      if (s[k].seq > top)
        top = s[k].seq;
    }
    seen = n < 16 || top == seen ? now : top;
  }
}

void cfg_load(void) {
  nkv = 0, seen = 0;
  cfg_refresh();
}

void cfg_save(void) {
  for (int i = 0; i < nkv; i++)
    if (kv[i].flags & DIRTY) {
      if (mx_setting_set(kv[i].key, kv[i].value) < 0)
        mx_printf("mxshell: setting %s not saved\n", kv[i].key);
      kv[i].flags = 0;
    }
}
