# MXSTORE, the store of mx OS (an app of MXSHELL2; its index: ps2max32/store):
# the app itself is built by the shell's Makefile (make store). This one makes
# its network tests, MXOS programs (libmxk) in build/apps/mxshell/store:
#
#   tlstest.elf    DNS, a TLS handshake and an HTTPS GET, timed
#   wget.elf       a download by src/https.c (redirects, chunked), timed, with
#                  its SHA-256
#
# mbedTLS (libs/mbedtls, upstream at v3.6.8) with src/mbedtls_user.h.

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
MBEDTLS  ?= $(PS2MAX)/libs/mbedtls
MX_TARGET ?= mxos
OUT      ?= $(PS2MAX)/build/apps/mxshell/store

all: $(OUT)/tlstest.elf $(OUT)/wget.elf

MAXSDK_OUT ?= $(OUT)/maxsdk
export EXTRA_CFLAGS = -ffunction-sections -fdata-sections
include $(MAXSDK)/mk/maxsdk.mk

PICOLIBC ?= $(PS2MAX)/build/libs/picolibc/install
LIBC_LIBS = $(MAXSDK_OUT)/libc/glue.c.o $(MAXSDK_OUT)/libmxc.a $(PICOLIBC)/lib/libc.a \
            $(MX_LIB) $(MAXSDK_OUT)/librt.a
$(MAXSDK_OUT)/libc/glue.c.o $(MAXSDK_OUT)/libmxc.a $(MAXSDK_OUT)/librt.a &: $(MX_LIB)
	$(MAKE) -C $(MAXSDK) OUT=$(MAXSDK_OUT) libc
SCFLAGS = $(filter-out -ffreestanding,$(CFLAGS)) -MMD -nostdlibinc -isystem $(PICOLIBC)/include \
          -I$(MBEDTLS)/include -I$(HERE)/src -DMBEDTLS_USER_CONFIG_FILE='"mbedtls_user.h"'

MBEDTLS_SRCS = $(notdir $(wildcard $(MBEDTLS)/library/*.c))
MBEDTLS_OBJS = $(MBEDTLS_SRCS:%.c=$(OUT)/obj/mbedtls/%.o)
COMMON = $(OUT)/obj/port.o $(OUT)/obj/roots.o
OBJS = $(OUT)/obj/tlstest.o $(OUT)/obj/wget.o $(OUT)/obj/https.o $(COMMON)

$(OUT)/obj/mbedtls/%.o: $(MBEDTLS)/library/%.c $(HERE)/src/mbedtls_user.h
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) -I$(MBEDTLS)/library -c $< -o $@

$(OUT)/obj/%.o: $(HERE)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) $(MX_NET_CFLAGS) -c $< -o $@

$(OUT)/roots.pem: $(sort $(wildcard $(HERE)/certs/*.pem))
	@mkdir -p $(dir $@)
	cat $^ > $@

$(OUT)/obj/roots.o: $(HERE)/src/roots.S $(OUT)/roots.pem $(HERE)/src/index_key.pem
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wa,-I$(OUT) -Wa,-I$(HERE)/src -c $< -o $@

$(OUT)/tlstest.elf: $(OUT)/obj/tlstest.o $(COMMON) $(MBEDTLS_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/tlstest.o $(COMMON) $(MBEDTLS_OBJS) $(LIBC_LIBS))

$(OUT)/wget.elf: $(OUT)/obj/wget.o $(OUT)/obj/https.o $(COMMON) $(MBEDTLS_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/wget.o $(OUT)/obj/https.o $(COMMON) $(MBEDTLS_OBJS) $(LIBC_LIBS))

-include $(OBJS:.o=.d) $(MBEDTLS_OBJS:.o=.d)

.PHONY: all
