# MXSHELL2: the new shell of mx OS (PLAN.md), on libgl.
#
#   make                 build/apps/mxshell/MXOS/MXSHELL.ELF: an MXOS program
#                        (libmxk), started by the kernel
#   make standalone      build/apps/mxshell-sa/mxshell.elf: libmx, for
#                        pictures on PCSX2 (-elf; no games are started)
#   make shot            that ELF on PCSX2 with the games of
#                        build/pcsx2-test/PS2MAX: host:mxshell.tga
#   make font            font/font_<mode>.z and font/font.h again from
#                        font/src (Inter Tight, JetBrains Mono)

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
MXGL     ?= $(PS2MAX)/libs/libgl
MX_TARGET ?= mxos
OUT      ?= $(PS2MAX)/build/apps/mxshell$(if $(filter standalone,$(MX_TARGET)),-sa)

PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_IRX     ?= $(PS2MAX_IOP_SDK)/iop/irx
all: $(OUT)/MXOS/MXSHELL.ELF

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

SRCS = main draw surface font tile games home hub settings pads tray settings_store toast dither video overscan devlist
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

$(OUT)/mxshell.elf: $(OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

$(OUT)/MXOS/MXSHELL.ELF: $(OUT)/mxshell.elf
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $<

-include $(OBJS:.o=.d)

# usbcopy (tools/usbcopy.c): files to and from the USB stick from the PC,
# through the host link
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

# Files, an app of the shell (files/files.c, in its Apps section): the
# shell's drawing, font, video mode and notifications, a main loop of its
# own. $(OUT)/PS2MAX/files: FILES.ELF and its game.ini, for
# <device>/PS2MAX/files/
FILES_SRCS = draw surface font dither video settings_store toast devlist
FILES_OBJS = $(FILES_SRCS:%=$(OUT)/obj/%.o) $(OUT)/obj/font_bin.o $(ZSRCS:%=$(OUT)/obj/zlib/%.o) $(OUT)/obj/files.o
$(OUT)/obj/files.o: $(HERE)/files/files.c
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) -I$(HERE)/src -c $< -o $@
$(OUT)/files.elf: $(FILES_OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(FILES_OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))
$(OUT)/PS2MAX/files/FILES.ELF: $(OUT)/files.elf $(HERE)/files/game.ini
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $<
	cp $(HERE)/files/game.ini $(dir $@)
files: $(OUT)/PS2MAX/files/FILES.ELF

# Media, an app of the shell (media/media.c, in its Media section): photos,
# videos and music, as Files on the shell's drawing
MEDIA_OBJS = $(FILES_SRCS:%=$(OUT)/obj/%.o) $(OUT)/obj/font_bin.o $(ZSRCS:%=$(OUT)/obj/zlib/%.o) $(OUT)/obj/media.o
$(OUT)/obj/media.o: $(HERE)/media/media.c
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) -I$(HERE)/src -c $< -o $@
$(OUT)/media.elf: $(MEDIA_OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(MEDIA_OBJS) $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))
# the program in PS2MAX/media, and three tiles that start it, each with
# its kind: PS2MAX/photos, videos and music (game.ini and covers)
MEDIA_KINDS = photos videos music
$(OUT)/PS2MAX/media/PLAYER.ELF: $(OUT)/media.elf $(MEDIA_KINDS:%=$(HERE)/media/%/game.ini) $(HERE)/tools/mk-media-covers.py
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $<
	for k in $(MEDIA_KINDS); do mkdir -p $(OUT)/PS2MAX/$$k && cp $(HERE)/media/$$k/game.ini $(OUT)/PS2MAX/$$k/; done
	python3 $(HERE)/tools/mk-media-covers.py $(HERE)/font/src/InterTight-Variable.ttf $(OUT)/PS2MAX
media: $(OUT)/PS2MAX/media/PLAYER.ELF

# Media on PCSX2 with the samples in build/pcsx2-test/Media
# (tools/media-samples.sh makes them): a picture after MEDIA_FRAMES frames
# (MEDIA_ARGS: kind=photo|video|music dev=host path=<dir> sel=<n> open
# keys=<buttons> mode=<video mode>; media/media.c says more). PCSX2 shows
# no 720p (no vblanks): 1080i or the SD modes
MEDIA_FRAMES ?= 300
media-shot:
	@test -n "$(RUNNER)" || { echo "make media-shot: set RUNNER (see the Makefile)"; exit 2; }
	@test -d $(PS2MAX)/build/pcsx2-test/Media || { echo "make media-shot: no samples: tools/media-samples.sh $(PS2MAX)/build/pcsx2-test"; exit 2; }
	$(MAKE) MX_TARGET=standalone media
	cp $(PS2MAX)/build/apps/mxshell-sa/media.elf $(PS2MAX)/build/pcsx2-test/media.elf
	echo "shot=$(MEDIA_FRAMES) $(MEDIA_ARGS)" > $(PS2MAX)/build/pcsx2-test/media.txt
	PCSX2_RENDERER=sw $(RUNNER) $(PS2MAX)/build/pcsx2-test/media.elf 60

# The home button on PCSX2 (kernel version 26): the shell (the MXOS build)
# as the kernel's shell, a stand-in game that presses home twice (fakegame
# "0 home=100", the kernel's "@@PAD" test lines): the paused game's hub,
# Resume, Close (hometest). HOME_KERNEL: an emulator kernel built with
# MODULE_DIR=host: (its shell host:MXSHELL.ELF) and the @@PAD test hook.
HOME_KERNEL ?= $(PS2MAX)/build/apps/mxos-emuhost/mxkernel.elf
home-test: all fakegame
	@test -n "$(RUNNER)" || { echo "make home-test: set RUNNER (see the Makefile)"; exit 2; }
	@test -f $(HOME_KERNEL) || { echo "make home-test: no $(HOME_KERNEL)"; exit 2; }
	mkdir -p $(PS2MAX)/build/pcsx2-test/PS2MAX/home
	cp $(OUT)/fakegame.elf $(PS2MAX)/build/pcsx2-test/PS2MAX/home/
	printf 'title=A Home Test\ntype=Test\nelf=fakegame.elf\nargs=0 home=100\ndescription=Presses the home button twice.\n' \
	  > $(PS2MAX)/build/pcsx2-test/PS2MAX/home/game.ini
	cp $(OUT)/MXOS/MXSHELL.ELF $(PS2MAX)/build/pcsx2-test/MXSHELL.ELF
	cp $(HOME_KERNEL) $(PS2MAX)/build/pcsx2-test/mxk-home.elf
	MXLOAD="host:MXSHELL.ELF dev=hostonly launch hometest" MXSWAPCFG="host:mxos.swp 64" PCSX2_EE=interp PCSX2_RENDERER=sw \
	  $(RUNNER) $(PS2MAX)/build/pcsx2-test/mxk-home.elf 60

# Files on PCSX2: a picture (FILES_ARGS: dev=<n>|<name> path=<dir> sel=<n> mark=<n> menu keys)
files-shot:
	@test -n "$(RUNNER)" || { echo "make files-shot: set RUNNER (see the Makefile)"; exit 2; }
	$(MAKE) MX_TARGET=standalone files
	cp $(PS2MAX)/build/apps/mxshell-sa/files.elf $(PS2MAX)/build/pcsx2-test/files.elf
	echo "shot=$(SHOT_FRAMES) $(FILES_ARGS)" > $(PS2MAX)/build/pcsx2-test/files.txt
	PCSX2_RENDERER=sw $(RUNNER) $(PS2MAX)/build/pcsx2-test/files.elf 240

# a picture on PCSX2: ARGS for more (screen=hub, screen=settings, right=<n>, note)
# RUNNER: a command that starts an ELF in PCSX2 (headless) and stops it at
# the end marker "@@PS2TEST_END@@ status=N"; args: the ELF, a timeout in s
SHOT_FRAMES ?= 200
shot: standalone
	@test -n "$(RUNNER)" || { echo "make shot: set RUNNER (see the Makefile)"; exit 2; }
	cp $(PS2MAX)/build/apps/mxshell-sa/mxshell.elf $(PS2MAX)/build/pcsx2-test/mxshell.elf
	echo "dev=hostonly shot=$(SHOT_FRAMES) $(ARGS)" > $(PS2MAX)/build/pcsx2-test/mxshell.txt
	PCSX2_RENDERER=sw $(RUNNER) $(PS2MAX)/build/pcsx2-test/mxshell.elf 240

font:
	python3 $(HERE)/tools/mk-font.py $(HERE)/font/src $(HERE)/font

clean:
	rm -rf $(OUT)

.PHONY: all standalone shot font clean fakegame usbcopy files files-shot media media-shot home-test
