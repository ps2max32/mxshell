# MXSHELL2: the new shell of mx OS (PLAN.md), on libgl.
#
#   make                 build/apps/mxshell2/MXOS/MXSHELL2.ELF: an MXOS program
#                        (libmxk), started by the kernel
#   make standalone      build/apps/mxshell2-sa/mxshell2.elf: libmx, for
#                        pictures on PCSX2 (-elf; no games are started)
#   make shot            that ELF on PCSX2 with the games of
#                        build/pcsx2-test/PS2MAX: host:mxshell2.tga
#   make font            font/font_<mode>.z and font/font.h again from
#                        font/src (Inter Tight, JetBrains Mono)

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
MXGL     ?= $(PS2MAX)/libs/libgl
MX_TARGET ?= mxos
OUT      ?= $(PS2MAX)/build/apps/mxshell2$(if $(filter standalone,$(MX_TARGET)),-sa)

all: $(OUT)/MXOS/MXSHELL2.ELF

# its own maxsdk, built with a section per function and variable (and
# libgl and the shell the same), for the linker to leave out what is not
# used: 1.05 to 0.89 MB, the shell stays in memory beside a game. At -O2:
# maxsdk at -Os saves 65 KB more but Home's frames come late more often, all
# at -Os (0.72 MB) drops Home to 54 fps while the focus moves
MAXSDK_OUT ?= $(OUT)/maxsdk
export EXTRA_CFLAGS = -ffunction-sections -fdata-sections
# texture names: the font and a cover a game (MAX_GAMES 64), with room to
# spare; libgl's default (2048) is 500 KB of core the shell keeps beside a
# game
MXGL_MAX_TEXTURES = 128
# room.vsm: the room's lights on VU1, by hand (libgl's MXGL_VSM_PROGS)
MXGL_VSM_DIR   := $(HERE)/src
MXGL_VSM_PROGS := room
include $(MXGL)/mk/mxgl.mk
include $(MAXSDK)/mk/maxsdk.mk
# in VM: the shell in VM, libgl (what the DMAC reads) in core
MX_CORE_OBJS = $(MXGL_OBJS)

PICOLIBC ?= $(PS2MAX)/build/libs/picolibc/install
LIBC_LIBS = $(MAXSDK_OUT)/libc/glue.c.o $(MAXSDK_OUT)/libmxc.a $(PICOLIBC)/lib/libc.a \
            $(MX_LIB) $(MAXSDK_OUT)/librt.a
# maxsdk's libc parts in our own MAXSDK_OUT (maxsdk.mk makes only the library)
$(MAXSDK_OUT)/libc/glue.c.o $(MAXSDK_OUT)/libmxc.a $(MAXSDK_OUT)/librt.a &: $(MX_LIB)
	$(MAKE) -C $(MAXSDK) OUT=$(MAXSDK_OUT) libc
SCFLAGS   = $(filter-out -ffreestanding,$(CFLAGS)) -MMD -nostdlibinc -isystem $(PICOLIBC)/include \
            -I$(MXGL_INC) -ffast-math -cl-single-precision-constant

SRCS = main draw font tile games home hub settings settings_store toast dither
# zlib's inflate (libs/zlib), for the font atlases
ZLIB   ?= $(PS2MAX)/libs/zlib
ZSRCS   = inflate inftrees inffast zutil adler32 uncompr
OBJS = $(SRCS:%=$(OUT)/obj/%.o) $(OUT)/obj/font_bin.o $(ZSRCS:%=$(OUT)/obj/zlib/%.o)

$(OUT)/obj/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) -DNO_GZIP -w -c $< -o $@

$(OUT)/obj/%.o: $(HERE)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) -I$(ZLIB) -c $< -o $@

$(OUT)/obj/font_bin.o: $(HERE)/src/font_bin.S $(wildcard $(HERE)/font/font_*.z)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wa,-I$(HERE)/font -I$(HERE)/font -c $< -o $@

$(OUT)/mxshell2.elf: $(OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

$(OUT)/MXOS/MXSHELL2.ELF: $(OUT)/mxshell2.elf
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $<

-include $(OBJS:.o=.d)

# usbcopy (tools/usbcopy.c): files to and from the USB stick from the PC,
# through the host link; it loads the USB drivers itself
usbcopy: $(OUT)/usbcopy.elf
$(OUT)/usbcopy.elf: $(HERE)/tools/usbcopy.c $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	@mkdir -p $(OUT)/obj
	$(CC) $(SCFLAGS) -c $< -o $(OUT)/obj/usbcopy.o
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/usbcopy.o $(LIBC_LIBS))

# a stand-in game for the tests (tests/fakegame.c), an MXOS program
fakegame: $(OUT)/fakegame.elf
$(OUT)/fakegame.elf: $(HERE)/tests/fakegame.c $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	@mkdir -p $(OUT)/obj
	$(CC) $(SCFLAGS) -c $< -o $(OUT)/obj/fakegame.o
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/fakegame.o $(LIBC_LIBS))

standalone:
	$(MAKE) MX_TARGET=standalone

# a picture on PCSX2: ARGS for more (screen=hub, screen=settings, right=<n>, note)
# RUNNER: a command that starts an ELF in PCSX2 (headless) and stops it at
# the end marker "@@PS2TEST_END@@ status=N"; args: the ELF, a timeout in s
SHOT_FRAMES ?= 200
shot: standalone
	@test -n "$(RUNNER)" || { echo "make shot: set RUNNER (see the Makefile)"; exit 2; }
	cp $(PS2MAX)/build/apps/mxshell2-sa/mxshell2.elf $(PS2MAX)/build/pcsx2-test/mxshell2.elf
	echo "dev=hostonly shot=$(SHOT_FRAMES) $(ARGS)" > $(PS2MAX)/build/pcsx2-test/mxshell2.txt
	PCSX2_RENDERER=sw $(RUNNER) $(PS2MAX)/build/pcsx2-test/mxshell2.elf 240

font:
	python3 $(HERE)/tools/mk-font.py $(HERE)/font/src $(HERE)/font

clean:
	rm -rf $(OUT)

.PHONY: all standalone shot font clean fakegame usbcopy
