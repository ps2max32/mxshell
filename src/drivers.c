/* MXSHELL2: the storage drivers of a device (USB, the HDD), from mc0:/MXOS,
   each loaded once on the IOP: for the shell and its apps. */
#include <mx/iop.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/debug.h>

#include "pn.h"

#define MXOS "mc0:/MXOS/"

/* the drivers of a device: file on mc0:/MXOS, the module's name on the IOP */
static const char *const mods[NDEVS][4][2] = {
    {{"bdm", "bdm"}, {"bdmfs_fatfs", "bdmff"}, {"usbd", "USB_driver"}, {"usbmass_bd", "usbmass_bd"}},
    {{"bdm", "bdm"}, {"bdmfs_fatfs", "bdmff"}, {"mxata", "mxata"}, {0, 0}},
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
};


static int load_module(const char *file, const char *name) {
  struct mx_iop_module m;
  if (mx_iop_find_module(name, &m) > 0) /* its id; 0: not loaded */
    return 0;
  char path[48];
  mx_snprintf(path, sizeof path, MXOS "%s.irx", file);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("mxshell: %s: not there (%d)\n", path, fd);
    return -1;
  }
  /* the IOP reads it by SIF DMA: pinned, and only while it is loaded (128
     KiB of core the shell would keep beside a game) */
  const u32 room = 128 * 1024;
  u8 *irx = mx_dma_alloc(room);
  if (!irx) {
    mx_fio_close(fd);
    return -1;
  }
  int n = mx_fio_read(fd, irx, room), res = 0;
  mx_fio_close(fd);
  if (n <= 0 || n == (int)room) {
    mx_printf("mxshell: %s: read %d\n", path, n);
    mx_dma_free(irx);
    return -1;
  }
  int id = mx_iop_load_module_buffer(irx, (u32)n, 0, 0, &res);
  mx_dma_free(irx);
  mx_printf("mxshell: %s: %d, %d\n", file, id, res);
  return id < 0 ? -1 : 0;
}

int dev_drivers(int d) {
  for (int m = 0; m < 4 && mods[d][m][0]; m++)
    if (load_module(mods[d][m][0], mods[d][m][1]) < 0)
      return -1;
  return 0;
}
