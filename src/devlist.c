/* MXSHELL2: the storage devices, for the shell and its apps. The kernel
   (version 26) loads the storage drivers, watches the devices come and go
   and lists them (<mx/dev.h>); host: is not in its list and is added here
   when asked for. The standalone build (libmx, pictures on PCSX2) has no
   list: host: only. */
#include <mx/dev.h>
#include <mx/debug.h>
#include <string.h>

#include "pn.h"

static void copy(char *d, u32 n, const char *s) {
  u32 i = 0;
  for (; s[i] && i + 1 < n; i++)
    d[i] = s[i];
  d[i] = 0;
}

int pdevs_read(struct pdev *d, int max, int host) {
  struct mx_dev_info in[PDEV_MAX];
  int n = mx_dev_list(in, PDEV_MAX), k = 0;
  for (int i = 0; i < n && i < PDEV_MAX && k < max; i++) {
    if (!in[i].present || in[i].kind == MX_DEV_HOST)
      continue;
    struct pdev *p = &d[k++];
    memset(p, 0, sizeof *p);
    copy(p->root, sizeof p->root, in[i].name);
    copy(p->label, sizeof p->label, in[i].label);
    p->kind = in[i].kind, p->fs = in[i].fs, p->flags = in[i].flags;
    p->size = in[i].size, p->free = in[i].free;
  }
  if (host && k < max) {
    struct pdev *p = &d[k++];
    memset(p, 0, sizeof *p);
    strcpy(p->root, "host:");
    p->kind = MX_DEV_HOST;
  }
  return k;
}

u32 pdevs_gen(void) { return mx_dev_gen(); }

/* "USB drive", "Memory card 2", ...: from the kind and the number in the name */
const char *pdev_name(const struct pdev *d) {
  static char s[24];
  const char *r = d->root;
  int num = 0;
  while (*r && (*r < '0' || *r > '9'))
    r++;
  num = *r ? *r - '0' : 0;
  switch (d->kind) {
  case MX_DEV_MC: mx_snprintf(s, sizeof s, "Memory card %d", num + 1); break;
  case MX_DEV_USB: mx_snprintf(s, sizeof s, num ? "USB drive %d" : "USB drive", num + 1); break;
  case MX_DEV_MX4SIO: return "SD card (MX4SIO)";
  case MX_DEV_HDD: return "Hard disk";
  case MX_DEV_HOST: return "PC (host)";
  default: return d->root;
  }
  return s;
}

/* "29.8 GB", "7.6 MB": bytes in the steps people read */
void pdev_bytes(char *out, int len, u64 b) {
  static const char *const unit[] = {"bytes", "KB", "MB", "GB", "TB"};
  int u = 0;
  u64 whole = b, tenth = 0;
  while (whole >= 1000 && u < 4) {
    tenth = (whole % 1000) / 100;
    whole /= 1000, u++;
  }
  if (u == 0 || whole >= 100)
    mx_snprintf(out, len, "%u %s", (u32)whole, unit[u]);
  else
    mx_snprintf(out, len, "%u.%u %s", (u32)whole, (u32)tenth, unit[u]);
}
