# MXSHELL2

The new shell of mx OS on the PS2, on libgl, in its own design language (Penumbra: focus is
light). It replaces mxshell once it does all mxshell does; until then it is a separate ELF.
What is decided, what the OS still lacks and the order of the work: [PLAN.md](PLAN.md).

What it does now (PLAN.md, phase A):

- **Home**: the games on a shelf, the lit one with its caption; covers from `cover.t8`, else a
  tile with the title. The light follows the focus.
- **Game hub** (triangle): the game as a slab, its details from `game.ini`, its size and
  place, its saves where `game.ini` names them (`saves=<directory>`).
- **Starting a game** (cross): the lit tile grows to the screen, the game starts with
  `mx_loader_launch` and the shell waits; its covers and beam racing's memory are given
  back first. Back from the game the shelf comes out of the dark; a game that crashed or
  ended with a status other than 0 leaves a notification.
- **Settings** (R1): Video (without a setting the console's own mode, PAL or NTSC; 1080i, 720p and the others by
  choice; a new mode
  stays only when kept within 15 s, and Start + Select held at power-on goes back to the
  console's own SD mode), Network (`mc0:/MXOS/IPCONFIG.DAT`, used from the kernel's next start),
  Storage (where it looks for games), Controllers (which ports have a pad; the motors tried),
  System (the shell, the kernel's version, `SERVICES.CFG`).
- **Notifications**: cards top right; a pad that comes or goes, a device found, a game that
  ended badly. Urgent ones stay until a button.
- **Power-on**: a point of light, a horizon, the mark, then the shelf.

The games are where mxshell finds them, `<device>/PS2MAX/<game>/game.ini`
(`apps/mxshell/package-games.sh` makes them): on USB (`usb0:`) and the internal HDD
(`ata0:`), and `host:` when Settings > Storage turns it on. The shell's own settings are in
`mc0:/MXOS/MXSHELL2.CF0` and `.CF1` (two files written in turn, so a save cut short loses
nothing) until the kernel has a settings store.

## Build

The shell builds its own maxsdk (in its output directory) with a section per function and
variable, and libgl and itself the same, so the linker leaves out what is not used: the ELF is
0.89 MB (it stays in memory beside a game). `-Os` would make it smaller but slower: Home then
misses frames while the focus moves.

```
make              # build/apps/mxshell2/MXOS/MXSHELL2.ELF: an MXOS program (libmxk)
make standalone   # build/apps/mxshell2-sa/mxshell2.elf: libmx, for pictures on PCSX2
make shot         # that ELF on PCSX2 (RUNNER=...) with build/pcsx2-test/PS2MAX: host:mxshell2.tga
                  # (ARGS="screen=hub", "screen=settings", "note", "right=<n>", "rgb16")
make fakegame     # tests/fakegame.c: a stand-in game that ends with the status it is given
make font         # font/font_<shape>.z and font.h from font/src
make usbcopy      # tools/usbcopy.c: files to and from the USB stick through the host link
```

Arguments (for tests): `dev=host`, `dev=hostonly`, `screen=home|hub|settings`, `right=<n>`,
`note`, `launch` (start the game in focus at once), `mode=1080i|ntsc|pal|sd`, `rgb16`,
`shot=<frames>` (the picture to
`host:mxshell2.tga`, then the end marker), `end=<frames>` (the end marker after that many
frames), `stats` (libgl's counters on the screen and once a second in the log), `walk` (the
focus a tile on every 20 frames, there and back: Home's busiest frames), `switch=<mode>,...`
and `every=<frames>` (the modes one after the other), `refl=a|b|c` (three other looks of the
reflections, to compare), `room=vu` (the room's lights on VU1, `src/room.vsm`). The standalone
build reads them from `host:mxshell2.txt`.

Measured on a PS2 in 1080i (`stats walk`): Home, Hub and Settings keep 60 fps (50 in PAL),
Home also while the focus moves, with about half a millisecond to spare then. The EE's work
is the room (the grid's points worked out once a row, against the lights that reach it, and
drawn as strips in one `mxgl_MultiDraw`; on VU1, `room=vu`, the GS passes took 7 ms instead of
2, as beam racing plays the stream three times a field), the tiles (their vertices as records, one
`mxgl_MultiDraw` a part) and the 2D layer.

To try the MXOS build on PCSX2, under the kernel built for it (`make -C apps/mxos emu`), with
the program and the games in `build/pcsx2-test`: `MXLOAD="host:mxshell2k.elf dev=hostonly"` and
`PCSX2_EE=interp` for the runner.

## Video modes and drawing

Layout is in units that are square on the screen: 720 high, 1280 wide at 16:9 (1080i, 1.5
pixels a unit) and 960 wide at 4:3 (NTSC and PAL, whose pixels are not square: each axis has
its own scale). The SD modes (NTSC, PAL, 480p, 576p) can be 16:9 too (Settings › Video ›
Television, `video.aspect`): anamorphic, 1280 units across the same 640 pixels. Text comes from
an atlas per shape of pixel (`font/font_hd.z`, `font_ntsc.z`, `font_ntscw.z` for NTSC at
16:9, ...; zlib, 1.3 MB in all packed to 0.33 MB, the mode's one unpacked when it is set), its
glyphs made for those pixels and drawn 1:1, filtered bilinearly (beam racing
draws the two fields half a line apart). Interlaced SD keeps its full vertical resolution:
each field draws its own lines of the same picture, for the television's deinterlacer to
weave; a softer picture against flicker is for a setting (libgl's deflicker, to come).

1080i is 16-bit colour (32 bits do not fit in GS memory at 1920 x 540 a field), which the
GS dithers and cuts to 5 bits: half a step darker for each translucent layer, so a soft light
that fades out would leave a dark ring. The background (the room: floor, the cone of light,
its pool, the shadows) and the tiles with their reflections are therefore worked out per
vertex on the EE and drawn opaque, once; opaque colours get the half step added.

The GS does not smooth a triangle's edges. Since the colour under a shape is known (the room,
or a card's colour), every rounded shape (tiles, pills, buttons, rings, cards) gets a rim of a
pixel whose colour goes from its own to what is under it, per vertex: the GS's interpolation
smooths the edge, with no blending and no render to texture.

Covers: `tools/mk-covers.py --boxart <dir> <out>` makes `cover.t8` (128 x 176, mxshell's)
and `coverhd.t8` (256 x 361, a DVD case) from box art you have (`<id>.png` or `.jpg`);
`game.ini` names them with `cover=` and `cover_hd=`. `usbcopy` puts them on the stick:
`usbcopy @host:<list>`, a line `<from> <to>` for each file.

The fonts are Inter Tight and JetBrains Mono (SIL Open Font License, in `font/src` with their
licences), made into the atlases by `tools/mk-font.py`.
