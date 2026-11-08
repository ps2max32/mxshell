# mxshell

The shell of mx OS on the PS2: a carousel of the games on a device, drawn by
beam racing in 24 bits. The kernel ([mxos](https://github.com/ps2max32/mxos))
starts it from `mc0:/MXOS/MXSHELL.ELF` at the boot and after each program it
started (`mx_loader_launch`). A game built on maxsdk for kernel version 9 or
later runs beside it: the shell waits in memory (its core at the top, under its
own ASID) and goes on at once when the game ends; an older game takes all
memory and the shell starts again after it. Select leaves the shell.

- The games: `<device>/PS2MAX/<game>/` with a `game.ini` (`package-games.sh`
  puts the ps2max games on a USB stick or SD card). All paths of a `game.ini`
  are relative to its directory: `title`, `year`, `type`, `elf`, `args`,
  `description` and `cover`, a file made by `tools/mk-cover.py` (the shell only
  copies it to GS memory; without one it draws a pattern from the title). A
  directory with just `<directory>.elf` is a game too.
- The devices: USB (`usb0:`), MX4SIO (`mx4sio0:`), the internal HDD (`ata0:`,
  FAT32 or exFAT, maxsdk's `mxata`) and HOST (`host:`, the directory of a
  `ps2client listen` on the PC, which then holds `PS2MAX/`). Storage drivers
  are loaded from `mc0:/MXOS` for the chosen device only (`SHELL.CFG`
  remembers it), so MX4SIO's polling costs nothing without one.
- The font is Lato (SIL Open Font License), made into `shell/font.bin` by
  `shell/gen-font.py`.

## Build

```
make      # build/apps/mxshell/MXOS: MXSHELL.ELF and the storage drivers
```

Needs maxsdk (`libmxk`, its own copy built for size in
`build/apps/mxshell/maxsdk`), the feature LLVM, and ps2sdk's prebuilt IOP
modules (`PS2MAX_IRX`). The kernel's build (`apps/mxos`, `make mxos-fill`)
puts these files into its `MXOS/` for the memory card.
