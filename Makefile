# mxshell: the shell of mx OS, a carousel of the games on a device; started
# by the kernel (apps/mxos) from mc0:/MXOS/MXSHELL.ELF.
#
#   make    build/apps/mxshell/MXOS/: MXSHELL.ELF and the storage drivers it
#           loads from mc0:/MXOS (apps/mxos puts them on the card)

all:

PS2MAX ?= $(abspath $(CURDIR)/../..)
OUT    ?= $(PS2MAX)/build/apps/mxshell
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_IRX     ?= $(PS2MAX_IOP_SDK)/iop/irx

# its own libmx, built for size with a section per function and variable:
# the shell's core stays in memory beside a game
MAXSDK_OUT = $(OUT)/maxsdk
export EXTRA_CFLAGS = -Os -ffunction-sections -fdata-sections

# an MXOS program with VM: it stays while another program runs, in an address
# space of its own (the kernel's VM programs side by side, mx OS plan 3a); what
# the DMAC or the IOP reads is in core (MX_CORE_DATA)
MX_TARGET = mxos
include $(PS2MAX)/libs/maxsdk/mk/maxsdk.mk

SRCS = main.c draw.c cover.c games.c console.c
OBJS = $(SRCS:%.c=$(OUT)/obj/%.o) $(OUT)/obj/font.o
# the storage drivers, one device's at a time
STORAGE_MODULES = bdm bdmfs_fatfs usbd usbmass_bd mx4sio_bd mxata

all: $(OUT)/MXOS/MXSHELL.ELF $(STORAGE_MODULES:%=$(OUT)/MXOS/%.irx)

$(OUT)/obj/%.o: shell/%.c shell/shell.h shell/font.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/font.o: shell/font.S shell/font.bin
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# libmxk: the kernel's services
LIB = $(MAXSDK_OUT)/libmxk.a
$(LIB): $(MX_LIB)
$(OUT)/mxshell.elf: $(OBJS) $(LIB) $(MX_CRT0) $(MX_LD)
	$(call link,$@,$(MX_CRT0) $(OBJS) $(LIB))

$(OUT)/MXOS/MXSHELL.ELF: $(OUT)/mxshell.elf
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $<

# maxsdk's USB mass storage and HDD drivers, from one make of its iop/
MAXSDK_IRX = $(PS2MAX)/build/libs/maxsdk/iop
$(OUT)/MXOS/usbmass_bd.irx $(OUT)/MXOS/mxata.irx: maxsdk-iop
	@mkdir -p $(dir $@)
	cp $(MAXSDK_IRX)/$(notdir $@) $@
maxsdk-iop:
	$(MAKE) -C $(PS2MAX)/libs/maxsdk/iop EXTRA_CFLAGS= OUT=$(MAXSDK_IRX)
# ps2sdk's prebuilt ones
$(OUT)/MXOS/%.irx: $(PS2MAX_IRX)/%.irx
	@mkdir -p $(dir $@)
	cp $< $@

clean:
	rm -rf $(OUT)

.PHONY: all clean maxsdk-iop
