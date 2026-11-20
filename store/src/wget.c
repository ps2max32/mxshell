/* wget: one HTTPS download (src/https.c), timed, with its SHA-256.

     wget.elf <url> [file]

   Follows the redirects (a GitHub release goes on to
   release-assets.githubusercontent.com), says each step's time and how much of
   it the EE waited for the network, and the body's size and SHA-256. With a
   file (host:..., mass0:/...), the body is written there too. */
#include "https.h"

#include <mbedtls/sha256.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/timer.h>
#include <stdio.h>

u64 mxstore_cycles(void);

struct out {
  mbedtls_sha256_context sha;
  int fd; /* the file, or -1 */
  u64 hashing;
};

static int sink(void *ctx, const void *data, u32 size) {
  struct out *o = ctx;
  u64 t0 = mxstore_cycles();
  mbedtls_sha256_update(&o->sha, data, size);
  o->hashing += mxstore_cycles() - t0;
  if (o->fd >= 0 && mx_fio_write(o->fd, data, size) != 0)
    return -1;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("usage: wget.elf <url> [file]\n");
    return 2;
  }
  static struct out o;
  mbedtls_sha256_init(&o.sha);
  mbedtls_sha256_starts(&o.sha, 0);
  o.fd = argc > 2 ? mx_fio_open(argv[2], MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC) : -1;
  if (argc > 2 && o.fd < 0) {
    mx_printf("wget: FAILED: cannot write %s\n", argv[2]);
    return 1;
  }

  u64 t0 = mxstore_cycles();
  int r = https_init();
  mx_printf("wget: set-up %u ms\n", (u32)((mxstore_cycles() - t0) / (MX_EE_HZ / 1000u)));

  static struct https_result res;
  t0 = mxstore_cycles();
  r = https_get(argv[1], sink, &o, &res);
  u32 all = (u32)((mxstore_cycles() - t0) / (MX_EE_HZ / 1000u));
  if (o.fd >= 0)
    mx_fio_close(o.fd);

  mx_printf("wget: dns %u ms, connect %u ms, handshake %u ms, request and body %u ms\n", res.ms_dns,
            res.ms_connect, res.ms_handshake, res.ms_body);
  mx_printf("wget: %u ms in all, %u of them waiting for the network, %u hashing\n", all, res.ms_waited,
            (u32)(o.hashing / (MX_EE_HZ / 1000u)));
  mx_printf("wget: status %d from %s after %u redirects\n", res.status, res.host, res.redirects);
  if (r) {
    mx_printf("wget: FAILED: %s %s\n", https_strerror(r), https_tls_error());
    return 1;
  }
  u8 h[32];
  mbedtls_sha256_finish(&o.sha, h);
  char hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(hex + 2 * i, 3, "%02x", h[i]);
  mx_printf("wget: %u bytes (length %d), %u KB/s\n", res.size, (int)res.length, all ? res.size / all : 0);
  mx_printf("wget: sha256 %s\n", hex);
  return 0;
}
