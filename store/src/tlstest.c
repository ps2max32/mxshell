/* tlstest: DNS, a TLS handshake and an HTTPS GET on the PS2, timed.

     tlstest.elf <host> [path] [a.b.c.d] [cipher suite]

   Resolves host by the DNS (mx_net_resolve); with an address given, that one
   when the DNS has no answer (an older mxsock). Then TLS to port 443 with the
   GitHub roots (certs/), and a GET of path (default "/"), the body counted, not
   kept. Each step says its time, and how much of it the EE spent waiting for
   the network: the rest is the EE's own work (the crypto). */
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <mx/mx.h>
#include <mx/net.h>
#include <mx/thread.h>
#include <mx/time.h>
#include <mx/timer.h>
#include <mbedtls/net_sockets.h>
#include <stdio.h>
#include <string.h>

u64 mxstore_cycles(void);
extern const char mxstore_roots[];
extern const unsigned mxstore_roots_size;

static u64 waited; /* cycles in mx_net_wait */
static u8 buf[16384];

static u32 ms(u64 c) { return (u32)(c / (MX_EE_HZ / 1000u)); }

static void wait_net(int s, int want) {
  u64 t0 = mxstore_cycles();
  mx_net_wait(want == MX_NET_READ ? 1u << s : 0, want == MX_NET_WRITE ? 1u << s : 0, 100000, 0, 0);
  waited += mxstore_cycles() - t0;
}

static int bio_send(void *ctx, const unsigned char *data, size_t len) {
  int n = mx_net_send((int)(long)ctx, data, (u32)len);
  if (n == MX_NET_AGAIN)
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  return n < 0 ? MBEDTLS_ERR_NET_CONN_RESET : n;
}

static int bio_recv(void *ctx, unsigned char *data, size_t len) {
  int n = mx_net_recv((int)(long)ctx, data, (u32)len);
  if (n == MX_NET_AGAIN)
    return MBEDTLS_ERR_SSL_WANT_READ;
  return n < 0 ? MBEDTLS_ERR_NET_CONN_RESET : n;
}

/* an mbedTLS call again until it is not WANT_*, sleeping on the socket */
#define RETRY(s, call)                                                                     \
  ({                                                                                       \
    int r_;                                                                                \
    while ((r_ = (call)) == MBEDTLS_ERR_SSL_WANT_READ || r_ == MBEDTLS_ERR_SSL_WANT_WRITE) \
      wait_net(s, r_ == MBEDTLS_ERR_SSL_WANT_READ ? MX_NET_READ : MX_NET_WRITE);           \
    r_;                                                                                    \
  })

static int fail(const char *what, int r) {
  char e[100];
  mbedtls_strerror(r, e, sizeof e);
  mx_printf("tlstest: FAILED: %s: -0x%04x %s\n", what, (unsigned)-r, e);
  return 1;
}

static void step(const char *what, u64 t0, u64 w0) {
  u64 all = mxstore_cycles() - t0, w = waited - w0;
  mx_printf("tlstest: %-10s %5u ms (network %u ms, EE %u ms)\n", what, ms(all), ms(w), ms(all - w));
}

static u32 ip_of(const char *s) {
  u32 ip;
  return mx_net_resolve(s, &ip) == 0 ? ip : 0; /* dotted: no DNS */
}

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "github.com";
  const char *path = argc > 2 ? argv[2] : "/";
  u32 fallback = argc > 3 ? ip_of(argv[3]) : 0;
  int r;

  if (mx_net_init() < 0) {
    mx_printf("tlstest: FAILED: no socket service\n");
    return 1;
  }
  struct mx_net_status st;
  memset(&st, 0, sizeof st);
  int got = -1;
  for (int i = 0; i < 30 && ((got = mx_net_status(&st)) < 0 || !st.ip); i++)
    mx_sleep_us(100000);
  mx_printf("tlstest: net status %d up %u link %u dhcp %u\n", got, st.up, st.link, st.dhcp);
  mx_printf("tlstest: ip %u.%u.%u.%u dns %u.%u.%u.%u\n", st.ip & 255, st.ip >> 8 & 255, st.ip >> 16 & 255,
            st.ip >> 24, st.dns & 255, st.dns >> 8 & 255, st.dns >> 16 & 255, st.dns >> 24);
  struct mx_tm tm;
  s64 now = mx_time();
  mx_gmtime(now, &tm);
  mx_printf("tlstest: date %04u-%02u-%02u %02u:%02u UTC%s\n", tm.year, tm.month, tm.day, tm.hour, tm.minute,
            now ? "" : " (no clock: certificate dates fail)");

  /* DNS */
  u64 t0 = mxstore_cycles(), w0 = waited;
  u32 ip = 0;
  while ((r = mx_net_resolve(host, &ip)) == MX_NET_AGAIN) {
    if (ms(mxstore_cycles() - t0) > 20000)
      break;
    mx_sleep_us(10000);
  }
  waited += mxstore_cycles() - t0; /* all of it is the network's */
  if (r != 0) {
    mx_printf("tlstest: dns %s: %d%s\n", host, r, fallback ? ", taking the address given" : "");
    if (!fallback)
      return 1;
    ip = fallback;
  } else
    step("dns", t0, w0);
  mx_printf("tlstest: %s is %u.%u.%u.%u\n", host, ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24);

  /* the crypto's set-up: the roots, the random generator */
  t0 = mxstore_cycles(), w0 = waited;
  if (psa_crypto_init() != PSA_SUCCESS) {
    mx_printf("tlstest: FAILED: psa_crypto_init\n");
    return 1;
  }
  static mbedtls_entropy_context entropy;
  static mbedtls_ctr_drbg_context drbg;
  static mbedtls_x509_crt roots;
  static mbedtls_ssl_config conf;
  static mbedtls_ssl_context ssl;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_x509_crt_init(&roots);
  mbedtls_ssl_config_init(&conf);
  mbedtls_ssl_init(&ssl);
  if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const u8 *)"mxstore", 7)))
    return fail("drbg seed", r);
  if ((r = mbedtls_x509_crt_parse(&roots, (const u8 *)mxstore_roots, mxstore_roots_size)))
    return fail("roots", r);
  if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT)))
    return fail("config", r);
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conf, &roots, 0);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  /* a fifth argument: the one cipher suite to offer (for comparisons) */
  static int suite[2];
  if (argc > 4) {
    suite[0] = mbedtls_ssl_get_ciphersuite_id(argv[4]);
    mbedtls_ssl_conf_ciphersuites(&conf, suite);
  }
  if ((r = mbedtls_ssl_setup(&ssl, &conf)) || (r = mbedtls_ssl_set_hostname(&ssl, host)))
    return fail("setup", r);
  step("setup", t0, w0);

  /* TCP */
  t0 = mxstore_cycles(), w0 = waited;
  int s = mx_net_socket(MX_NET_TCP);
  if (s < 0)
    return fail("socket", s);
  while ((r = mx_net_connect(s, ip, 443)) == MX_NET_AGAIN) {
    if (ms(mxstore_cycles() - t0) > 15000)
      break;
    wait_net(s, MX_NET_WRITE);
  }
  if (r != 0) {
    mx_printf("tlstest: FAILED: connect %d\n", r);
    return 1;
  }
  step("connect", t0, w0);

  /* TLS */
  t0 = mxstore_cycles(), w0 = waited;
  mbedtls_ssl_set_bio(&ssl, (void *)(long)s, bio_send, bio_recv, 0);
  r = RETRY(s, mbedtls_ssl_handshake(&ssl));
  u32 flags = mbedtls_ssl_get_verify_result(&ssl);
  if (flags && flags != (u32)-1) {
    char v[256];
    mbedtls_x509_crt_verify_info(v, sizeof v, "tlstest:   ", flags);
    mx_printf("tlstest: certificate:\n%s", v);
  }
  if (r)
    return fail("handshake", r);
  step("handshake", t0, w0);
  mx_printf("tlstest: %s, %s\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));

  /* GET */
  t0 = mxstore_cycles(), w0 = waited;
  int len = snprintf((char *)buf, sizeof buf,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: mxstore-tlstest\r\nConnection: close\r\n\r\n",
                     path, host);
  for (int done = 0; done < len;) {
    r = RETRY(s, mbedtls_ssl_write(&ssl, buf + done, (size_t)(len - done)));
    if (r < 0)
      return fail("write", r);
    done += r;
  }
  u32 total = 0, shown = 0;
  for (;;) {
    r = RETRY(s, mbedtls_ssl_read(&ssl, buf, sizeof buf - 1));
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_NET_CONN_RESET)
      break;
    if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
      continue;
    if (r < 0)
      return fail("read", r);
    if (!shown) { /* the status line and the headers */
      buf[r] = 0;
      char *end = strstr((char *)buf, "\r\n\r\n");
      if (end)
        *end = 0;
      mx_printf("%s\n", (char *)buf);
      shown = 1;
    }
    total += (u32)r;
  }
  step("get", t0, w0);
  u32 t = ms(mxstore_cycles() - t0);
  mx_printf("tlstest: %u bytes, %u KB/s\n", total, t ? total / t : 0);

  mbedtls_ssl_close_notify(&ssl);
  mx_net_close(s);
  mx_printf("tlstest: done\n");
  return 0;
}
