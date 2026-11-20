/* mbedTLS on the PS2 (MBEDTLS_USER_CONFIG_FILE, after mbedTLS's own default
   configuration): no files, no POSIX sockets or timers; the date from the
   kernel's clock (<mx/time.h>), the entropy from src/port.c. */
#ifndef MXSTORE_MBEDTLS_USER_H
#define MXSTORE_MBEDTLS_USER_H

#include <time.h>

/* the program talks to <mx/net.h> itself (src/tls.c) */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SELF_TEST

/* 32-bit limbs: the R5900 multiplies 32 x 32 to 64 bits (multu), but has no
   64-bit multiply (dmultu), which 64-bit limbs on a mips64 would need */
#define MBEDTLS_HAVE_INT32
#undef MBEDTLS_HAVE_ASM

/* no /dev/urandom: mbedtls_hardware_poll (src/port.c) */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* certificates' dates: the kernel's clock (mx_time), not libc's time(),
   which counts from the program's start */
time_t mxstore_time(time_t *t);
#define MBEDTLS_PLATFORM_TIME_MACRO mxstore_time
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

#endif
