/* HTTPS GET for the store: DNS (mx_net_resolve), TLS (mbedTLS, the roots of
   certs/), HTTP/1.1 with redirects and chunked bodies. One request at a time
   (one connection's state, static): from one thread. It waits by sleeping on
   the socket (mx_net_wait), never by spinning. */
#ifndef MXSTORE_HTTPS_H
#define MXSTORE_HTTPS_H

#include <mx/types.h>

#define HTTPS_EURL (-1)      /* not https://host/path, or too long */
#define HTTPS_EDNS (-2)      /* the host has no address */
#define HTTPS_ECONNECT (-3)  /* no connection, or it took over 15 s */
#define HTTPS_ETLS (-4)      /* the handshake failed (detail: https_tls_error) */
#define HTTPS_ECERT (-5)     /* the certificate is not trusted (https_tls_error) */
#define HTTPS_EIO (-6)       /* the connection broke, or 20 s without data */
#define HTTPS_EHTTP (-7)     /* not an HTTP answer we understand */
#define HTTPS_ESTATUS (-8)   /* an answer other than 200 (https_result.status) */
#define HTTPS_EREDIRECT (-9) /* more than 5 redirects */
#define HTTPS_ESINK (-10)    /* the sink stopped it */
#define HTTPS_ESETUP (-11)   /* no sockets, no memory, the random generator */

/* each piece of the body, in order: < 0 stops the download */
typedef int (*https_sink)(void *ctx, const void *data, u32 size);

struct https_result {
  int status;      /* the last HTTP status */
  u32 length;      /* the body's Content-Length, 0xFFFFFFFF when not given */
  u32 size;        /* bytes given to the sink */
  u32 redirects;   /* redirects followed */
  char host[256];  /* the host the body came from */
  /* milliseconds, summed over the redirects */
  u32 ms_dns, ms_connect, ms_handshake, ms_body;
  u32 ms_waited; /* of all, the time the EE waited for the network */
};

/* The roots and the random generator, once (also done by the first GET): 0 or
   HTTPS_ESETUP */
int https_init(void);
/* GET url ("https://host[:port]/path") into sink: 0 when the whole body came
   (200), else HTTPS_E*. r (not 0) says what happened, also on an error. */
int https_get(const char *url, https_sink sink, void *ctx, struct https_result *r);
/* the last TLS error or certificate problem, as text ("" when none) */
const char *https_tls_error(void);
/* the last certificate's verification flags (mbedTLS MBEDTLS_X509_BADCERT_*),
   0 when none: HTTPS_ECERT with BADCERT_FUTURE or _EXPIRED is mostly a clock
   that is wrong */
extern u32 https_cert_flags;
/* error codes as text */
const char *https_strerror(int e);

#endif
