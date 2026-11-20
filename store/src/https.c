/* HTTPS GET (src/https.h). */
#include "https.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mx/net.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <string.h>

u64 mxstore_cycles(void);
extern const char mxstore_roots[];
extern const unsigned mxstore_roots_size;

#define MAX_REDIRECTS 5
#define CONNECT_MS 15000
#define IDLE_MS 20000
#define DNS_MS 20000
#define HEADER_MAX 4096 /* a header line: a release's Location is about 1 KB */

static int ready;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt roots;
static mbedtls_ssl_config conf;
static char tls_error[256];

/* one connection */
static struct {
  int s;
  mbedtls_ssl_context ssl;
  u8 buf[16384];
  u32 pos, end;
  u64 idle_since;
  u64 waited;
} c;
static char line[HEADER_MAX];
static char location[HEADER_MAX];

static u32 ms(u64 cycles) { return (u32)(cycles / (MX_EE_HZ / 1000u)); }

int https_init(void) {
  if (ready)
    return 0;
  if (psa_crypto_init() != PSA_SUCCESS)
    return HTTPS_ESETUP;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_x509_crt_init(&roots);
  mbedtls_ssl_config_init(&conf);
  if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const u8 *)"mxstore", 7) ||
      mbedtls_x509_crt_parse(&roots, (const u8 *)mxstore_roots, mxstore_roots_size) ||
      mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT))
    return HTTPS_ESETUP;
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conf, &roots, 0);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  ready = 1;
  return 0;
}

const char *https_tls_error(void) { return tls_error; }

const char *https_strerror(int e) {
  static const char *const text[] = {
      "done",          "bad URL",         "no address for the host", "no connection",
      "TLS failed",    "untrusted certificate", "connection broken or idle", "bad HTTP answer",
      "HTTP status",   "too many redirects", "stopped",               "set-up failed"};
  return e <= 0 && -e < (int)(sizeof text / sizeof text[0]) ? text[-e] : "?";
}

static void tls_text(int r) {
  mbedtls_strerror(r, tls_error, sizeof tls_error);
}

/* ---- the socket ---- */

static void wait_net(int want) {
  u64 t0 = mxstore_cycles();
  mx_net_wait(want == MX_NET_READ ? 1u << c.s : 0, want == MX_NET_WRITE ? 1u << c.s : 0, 100000, 0, 0);
  c.waited += mxstore_cycles() - t0;
}

static int bio_send(void *ctx, const unsigned char *data, size_t len) {
  (void)ctx;
  int n = mx_net_send(c.s, data, (u32)len);
  if (n == MX_NET_AGAIN)
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  return n < 0 ? MBEDTLS_ERR_NET_CONN_RESET : n;
}

static int bio_recv(void *ctx, unsigned char *data, size_t len) {
  (void)ctx;
  int n = mx_net_recv(c.s, data, (u32)len);
  if (n == MX_NET_AGAIN)
    return MBEDTLS_ERR_SSL_WANT_READ;
  return n < 0 ? MBEDTLS_ERR_NET_CONN_RESET : n;
}

/* an mbedTLS call again while it wants the network, until IDLE_MS pass
   without progress: its result, or MBEDTLS_ERR_SSL_TIMEOUT */
#define RETRY(call)                                                                        \
  ({                                                                                       \
    int r_;                                                                                \
    c.idle_since = mxstore_cycles();                                                       \
    while ((r_ = (call)) == MBEDTLS_ERR_SSL_WANT_READ || r_ == MBEDTLS_ERR_SSL_WANT_WRITE) { \
      if (ms(mxstore_cycles() - c.idle_since) > IDLE_MS) {                                 \
        r_ = MBEDTLS_ERR_SSL_TIMEOUT;                                                      \
        break;                                                                             \
      }                                                                                    \
      wait_net(r_ == MBEDTLS_ERR_SSL_WANT_READ ? MX_NET_READ : MX_NET_WRITE);              \
    }                                                                                      \
    r_;                                                                                    \
  })

static void conn_close(int polite) {
  if (c.s < 0)
    return;
  if (polite)
    mbedtls_ssl_close_notify(&c.ssl);
  mbedtls_ssl_free(&c.ssl);
  mx_net_close(c.s);
  c.s = -1;
}

static int conn_open(const char *host, u32 port, struct https_result *r) {
  u64 t0 = mxstore_cycles();
  u32 ip;
  int n;
  while ((n = mx_net_resolve(host, &ip)) == MX_NET_AGAIN) {
    if (ms(mxstore_cycles() - t0) > DNS_MS)
      break;
    mx_sleep_us(10000);
  }
  u64 t1 = mxstore_cycles();
  r->ms_dns += ms(t1 - t0), r->ms_waited += ms(t1 - t0);
  if (n != 0)
    return HTTPS_EDNS;

  c.s = mx_net_socket(MX_NET_TCP);
  if (c.s < 0)
    return HTTPS_ESETUP;
  c.waited = 0;
  while ((n = mx_net_connect(c.s, ip, port)) == MX_NET_AGAIN && ms(mxstore_cycles() - t1) <= CONNECT_MS)
    wait_net(MX_NET_WRITE);
  u64 t2 = mxstore_cycles();
  r->ms_connect += ms(t2 - t1), r->ms_waited += ms(c.waited);
  if (n != 0) {
    mx_net_close(c.s);
    c.s = -1;
    return HTTPS_ECONNECT;
  }

  mbedtls_ssl_init(&c.ssl);
  if (mbedtls_ssl_setup(&c.ssl, &conf) || mbedtls_ssl_set_hostname(&c.ssl, host)) {
    conn_close(0);
    return HTTPS_ESETUP;
  }
  mbedtls_ssl_set_bio(&c.ssl, 0, bio_send, bio_recv, 0);
  c.waited = 0;
  n = RETRY(mbedtls_ssl_handshake(&c.ssl));
  r->ms_handshake += ms(mxstore_cycles() - t2), r->ms_waited += ms(c.waited);
  if (n) {
    u32 flags = mbedtls_ssl_get_verify_result(&c.ssl);
    int cert = flags && flags != (u32)-1;
    if (cert)
      mbedtls_x509_crt_verify_info(tls_error, sizeof tls_error, "", flags);
    else
      tls_text(n);
    conn_close(0);
    return cert ? HTTPS_ECERT : HTTPS_ETLS;
  }
  c.pos = c.end = 0;
  return 0;
}

/* more of the answer into c.buf: > 0, 0 at its end, < 0 an error */
static int fill(void) {
  for (;;) {
    int n = RETRY(mbedtls_ssl_read(&c.ssl, c.buf, sizeof c.buf));
    if (n > 0) {
      c.pos = 0, c.end = (u32)n;
      return n;
    }
    if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
      return 0;
    if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
      continue;
    tls_text(n);
    return HTTPS_EIO;
  }
}

/* a line without its CRLF into line: its length, HTTPS_EHTTP if the answer
   ended, HTTPS_EIO. A longer line (GitHub's Content-Security-Policy is over
   4 KB) is cut to its start: line_cut says so. */
static int line_cut;
static int read_line(void) {
  u32 n = 0;
  line_cut = 0;
  for (;;) {
    if (c.pos == c.end) {
      int f = fill();
      if (f <= 0)
        return f < 0 ? f : HTTPS_EHTTP;
    }
    char ch = (char)c.buf[c.pos++];
    if (ch == '\n') {
      if (n && line[n - 1] == '\r')
        n--;
      line[n] = 0;
      return (int)n;
    }
    if (n == HEADER_MAX - 1) {
      line_cut = 1;
      continue;
    }
    line[n++] = ch;
  }
}

/* count bytes of the body to the sink (all of it: count 0xFFFFFFFF, to the end) */
static int body(u32 count, https_sink sink, void *ctx, struct https_result *r) {
  while (count) {
    if (c.pos == c.end) {
      int f = fill();
      if (f < 0)
        return f;
      if (f == 0)
        return count == 0xFFFFFFFFu ? 0 : HTTPS_EIO;
    }
    u32 n = c.end - c.pos;
    if (count != 0xFFFFFFFFu && n > count)
      n = count;
    if (sink && sink(ctx, c.buf + c.pos, n) < 0)
      return HTTPS_ESINK;
    c.pos += n, r->size += n;
    if (count != 0xFFFFFFFFu)
      count -= n;
  }
  return 0;
}

static int chunked(https_sink sink, void *ctx, struct https_result *r) {
  for (;;) {
    int n = read_line();
    if (n < 0)
      return n;
    u32 size = 0;
    const char *p = line;
    for (; *p; p++) {
      int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
      if (d < 0)
        break; /* ';' extensions, spaces */
      if (size >> 28)
        return HTTPS_EHTTP;
      size = size << 4 | (u32)d;
    }
    if (p == line)
      return HTTPS_EHTTP;
    if (!size) { /* the trailer, to its empty line */
      while ((n = read_line()) > 0)
        ;
      return n < 0 ? n : 0;
    }
    if ((n = body(size, sink, ctx, r)) < 0)
      return n;
    if ((n = read_line()) != 0) /* the chunk's CRLF */
      return n < 0 ? n : HTTPS_EHTTP;
  }
}

static int starts(const char *s, const char *prefix) {
  for (; *prefix; s++, prefix++)
    if ((*s | 32) != (*prefix | 32))
      return 0;
  return 1;
}

static const char *value(const char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
}

/* "https://host[:port]/path": host, port, path; 0 or HTTPS_EURL */
static int parse_url(const char *url, char *host, u32 *port, const char **path) {
  if (!starts(url, "https://"))
    return HTTPS_EURL;
  const char *h = url + 8, *e = h;
  while (*e && *e != '/' && *e != ':' && *e != '?')
    e++;
  if (e == h || e - h > 255)
    return HTTPS_EURL;
  memcpy(host, h, (u32)(e - h));
  host[e - h] = 0;
  *port = 443;
  if (*e == ':') {
    u32 p = 0;
    for (e++; *e >= '0' && *e <= '9'; e++)
      p = p * 10 + (u32)(*e - '0');
    if (!p || p > 65535)
      return HTTPS_EURL;
    *port = p;
  }
  *path = *e ? e : "/";
  return 0;
}

/* one request: 0 with the body sent to sink, 1 with a redirect in location,
   or HTTPS_E* */
static int request(const char *url, https_sink sink, void *ctx, struct https_result *r) {
  char host[256];
  u32 port;
  const char *path;
  int n = parse_url(url, host, &port, &path);
  if (n)
    return n;
  if (strlen(path) > sizeof c.buf - 512)
    return HTTPS_EURL;
  strcpy(r->host, host);
  if ((n = conn_open(host, port, r)))
    return n;

  u64 t0 = mxstore_cycles();
  c.waited = 0;
  int len = snprintf((char *)c.buf, sizeof c.buf,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: mxstore/0.1\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
  for (int done = 0; done < len;) {
    n = RETRY(mbedtls_ssl_write(&c.ssl, c.buf + done, (size_t)(len - done)));
    if (n <= 0) {
      tls_text(n);
      conn_close(0);
      return HTTPS_EIO;
    }
    done += n;
  }
  c.pos = c.end = 0;

  /* the status line, the headers */
  int status = 0, chunks = 0, redirect;
  r->length = 0xFFFFFFFFu;
  location[0] = 0;
  if ((n = read_line()) < 0)
    goto out;
  if (!starts(line, "HTTP/1.") || strlen(line) < 12) {
    n = HTTPS_EHTTP;
    goto out;
  }
  status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
  r->status = status;
  while ((n = read_line()) > 0) {
    if (starts(line, "content-length:")) {
      u32 v = 0;
      for (const char *p = value(line + 15); *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (u32)(*p - '0');
      r->length = v;
    } else if (starts(line, "transfer-encoding:") && strstr(value(line + 18), "chunked"))
      chunks = 1;
    else if (starts(line, "location:")) {
      if (line_cut) {
        n = HTTPS_EURL;
        break;
      }
      strcpy(location, value(line + 9));
    }
  }
  if (n < 0)
    goto out;

  redirect = (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location[0];
  if (redirect)
    n = 1;
  else if (status != 200)
    n = HTTPS_ESTATUS;
  else {
    r->size = 0;
    n = chunks ? chunked(sink, ctx, r) : body(r->length, sink, ctx, r);
  }
out:
  r->ms_body += ms(mxstore_cycles() - t0), r->ms_waited += ms(c.waited);
  conn_close(n == 0);
  return n;
}

int https_get(const char *url, https_sink sink, void *ctx, struct https_result *r) {
  static struct https_result none;
  static char next[HEADER_MAX + 300];
  if (!r)
    r = &none;
  memset(r, 0, sizeof *r);
  tls_error[0] = 0;
  c.s = -1;
  if (https_init())
    return HTTPS_ESETUP;
  if (mx_net_init() < 0)
    return HTTPS_ESETUP;
  if (strlen(url) >= sizeof next)
    return HTTPS_EURL;
  strcpy(next, url);
  for (;;) {
    int n = request(next, sink, ctx, r);
    if (n != 1)
      return n;
    if (++r->redirects > MAX_REDIRECTS)
      return HTTPS_EREDIRECT;
    if (location[0] == '/') { /* on the same host */
      char *p = next + 8;
      while (*p && *p != '/' && *p != '?')
        p++;
      if ((u32)(p - next) + strlen(location) >= sizeof next)
        return HTTPS_EURL;
      strcpy(p, location);
    } else
      strcpy(next, location);
  }
}
