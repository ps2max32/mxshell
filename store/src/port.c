/* What mbedTLS needs of the system (src/mbedtls_user.h): the date, milliseconds,
   and entropy. */
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <mx/thread.h>
#include <mx/time.h>
#include <mx/timer.h>
#include <string.h>

time_t mxstore_time(time_t *t) {
  time_t now = (time_t)mx_time();
  if (t)
    *t = now;
  return now;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *out) {
  struct mx_tm m;
  mx_gmtime((s64)*tt, &m);
  memset(out, 0, sizeof *out);
  out->tm_year = m.year - 1900, out->tm_mon = m.month - 1, out->tm_mday = m.day;
  out->tm_hour = m.hour, out->tm_min = m.minute, out->tm_sec = m.second, out->tm_wday = m.weekday;
  return out;
}

/* COP0 Count, extended to 64 bits (calls less than 14.5 s apart) */
u64 mxstore_cycles(void) {
  static u32 last, high;
  u32 c = mx_count();
  if (c < last)
    high++;
  last = c;
  return (u64)high << 32 | c;
}

mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)(mxstore_cycles() / (MX_EE_HZ / 1000u)); }

/* Entropy: the low bits of the cycle counter around short sleeps, whose length
   the scheduler's timer, the interrupts and the IOP's answers vary. Each output
   byte folds 8 samples. Not a hardware generator (the PS2 has none): good enough
   to measure the handshake; the store needs more (the SIF's and the network's
   timing, a seed kept on the card). */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
  (void)data;
  for (size_t i = 0; i < len; i++) {
    u8 b = 0;
    for (int k = 0; k < 8; k++) {
      mx_sleep_us(1 + (b & 7));
      u32 c = mx_count();
      b = (u8)((b << 1 | b >> 7) ^ c ^ c >> 8);
    }
    out[i] = b;
  }
  *olen = len;
  return 0;
}
