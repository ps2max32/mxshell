# MXSHELL2

The new shell of mx OS on the PS2, on libgl, in its own design language (Penumbra: focus is
light). It is the shell of mx OS (`mc0:/MXOS/MXSHELL.ELF`); the first shell is the branch `mxshell1`.
What is decided, what the OS still lacks and the order of the work: [PLAN.md](PLAN.md).

What it does now (PLAN.md, phase A):

- **Home**: sections, L1 and R1 go through them: Games, Media, Emulation, Apps and Settings.
  Each is a shelf, the lit tile with its caption; covers from `cover.t8`, else a tile with the
  title. The light follows the focus. A program's `game.ini` names its section with
  `section=media`, `emulation` or `apps` (without it: Games), its place with `order=<n>`
  (by it first, 0 without, then by title); `elf=../<dir>/<file>` names a program in another
  directory of `PS2MAX/` (Media's three tiles share one).
- **Game hub** (triangle): the game as a slab, its details from `game.ini`, its size and
  place, its saves where `game.ini` names them (`saves=<directory>`).
- **Starting a game** (cross): the lit tile goes to the middle as a poster, the rest goes dark,
  a line under it fills, then black; the shell gives back its covers and beam racing's memory.
  The game starts beside the shell (`mx_proc_spawn`), which waits for the kernel's events: the
  home button in the game pauses it and brings the shell in front with the game's hub (Paused:
  cross Resume, square Close, the home button back to it); starting another game closes the
  paused one first. Back from the game the shelf comes out of the dark; a game that ended with
  a status other than 0 leaves a notification (a crash is the kernel's).
- **Settings** (the last section, its own screen: the pages in a list, L1 back to Apps): Video (a family of modes, 1080, 720p, 480p/576p or NTSC/PAL, and under it the frame rate or
  standard, colour, dithering, television and flicker filter where they apply; without a setting
  the console's own mode, PAL or NTSC; a new mode
  stays only when kept within 15 s, and Start + Select held at power-on goes back to the
  console's own SD mode), Network (`mc0:/MXOS/IPCONFIG.DAT`, used from the kernel's next start),
  Storage (the kernel's devices with size, free space where known, games and the swap file;
  cross on one: ready to remove; where MXOS's swap file is, or none; games from the PC),
  Controllers (kernel version 28: each pad with its player, kind, link, battery, charging and
  light bar; left and right choose a DS4's or DualSense's colour, cross tries the motors,
  square ends a Bluetooth link; pairing over Bluetooth; the light bars' brightness and turning
  idle Bluetooth pads off; the paired pads not there, square forgets one),
  System (the shell, the kernel's version, `SERVICES.CFG`).
- **Files** (`files/`, an app in Apps: `make files` makes `PS2MAX/files/` with `FILES.ELF` and its
  `game.ini`, for a device's `PS2MAX/`): the devices the kernel lists and the PC (host:);
  select (square), and from the actions (triangle) copy, cut, paste, rename, delete, a new folder,
  select all; names from an on-screen keyboard; long jobs with their progress, circle stops
  them. Until mxio can say what a directory entry is, rename and give free space, a folder is
  what opens as one, rename and move are a copy and a delete, and no free space is shown.
  Cross on an ELF starts it (Files waits). `make files-shot RUNNER=... FILES_ARGS="dev=host path=PS2MAX ops"` runs every
  action once in PCSX2.
- **Media** (`media/`, three tiles in Media, Photos, Videos and Music, each starting
  `PS2MAX/media/PLAYER.ELF` with its kind; `make media`): a device's `Media/<kind>` folder
  first, any folder from there, L1 and R1 between the kinds, a preview of the chosen file at
  16:9. Photos (JPEG, PNG; not progressive JPEG) full screen, scaled down to fit 9 tiles of
  256 x 256, the most that fit GS memory beside 1080i's buffer; L1/R1 the folder's others.
  Videos (MPEG-1/2 with LPCM or MP2 sound, `<mx/movie.h>` on the IPU, up to 768 x 768) with a
  bar of where it is: cross pauses, L1/R1 10 s back or on. Music (WAV, MP2; MP3 listed, not
  played) in a thread of its own: it plays on while you browse or look at photos, the folder's
  next track after it, START shows Now playing (cross pause, L1/R1 previous and next, square
  stop). It ends with Media: until the OS can keep a program beside the shell and games.
  `tools/media-samples.sh build/pcsx2-test` downloads samples (Wikimedia Commons, Big Buck
  Bunny) and `make media-shot RUNNER=... MEDIA_ARGS="kind=video dev=host path=Media/Videos sel=1 open keys=rx"`
  plays one in PCSX2 (`keys=`: buttons a second apart).
- **Notifications**: cards top right, from the kernel's ring (`<mx/notify.h>`: devices that come
  and go, the swap file, controllers, crashes, other programs) and the shell's own; the ones
  that came during a game when the shell is in front again. Urgent ones stay until a button or
  until the kernel says they are over; triangle on one with an action opens its page (Storage,
  Controllers). The start is quiet: the devices, controllers and swap file that are there when
  the PS2 starts get no card (during the boot screen and 5 s after it), only what comes later.
  While MXOS has no swap file in use a card with the memory in use as a bar (amber from 85 %)
  comes for 4 s, and again each time one more MiB is in use, so the user sees memory run out; the kernel's own swap file cards ("in use",
  "no swap file") are not shown.
- **Devices**: the kernel (version 26) loads the storage drivers and lists the devices
  (`<mx/dev.h>`); when one comes or goes the shelf is filled again.
- **Power-on**: a point of light, a horizon, the mark, then the shelf.

The games are where mxshell finds them, `<device>/PS2MAX/<game>/game.ini`
(`apps/mxshell/package-games.sh` makes them): on the devices the kernel lists but the memory
cards (USB `usb0:`, the internal HDD `ata0:`), and `host:` when Settings > Storage turns it on.
The shell needs kernel version 27. Its settings (`video.*`, `dev.host`) are in the kernel's
settings store (`<mx/setting.h>`), which the kernel writes to the memory card; the shell's test
arguments (`mode=`, `rgb16`, ...) only change them for that run.

## Build

The shell builds its own maxsdk (in its output directory) with a section per function and
variable, and libgl and itself the same, so the linker leaves out what is not used: the ELF is
0.89 MB (it stays in memory beside a game). `-Os` would make it smaller but slower: Home then
misses frames while the focus moves.

```
make              # build/apps/mxshell/MXOS/MXSHELL.ELF: an MXOS program (libmxk)
make standalone   # build/apps/mxshell-sa/mxshell.elf: libmx, for pictures on PCSX2
make shot         # that ELF on PCSX2 (RUNNER=...) with build/pcsx2-test/PS2MAX: host:mxshell.tga
                  # (ARGS="screen=hub", "screen=settings", "note", "right=<n>", "rgb16")
make fakegame     # tests/fakegame.c: a stand-in game that ends with the status it is given
                  # (args "0 home": runs 30 s and lets the home button through)
make home-test    # the home button on PCSX2 (RUNNER=..., HOME_KERNEL=<emulator kernel, MODULE_DIR=host:>):
                  # a stand-in game pressed home twice: hub (host:mxshell-home1.tga), Resume, Close
make files        # PS2MAX/files: FILES.ELF and its game.ini
make media        # PS2MAX/media/PLAYER.ELF, PS2MAX/photos, videos, music (game.ini, covers)
make font         # font/font_<shape>.z and font.h from font/src
make usbcopy      # tools/usbcopy.c: files to and from the USB stick through the host link
```

Arguments (for tests): `dev=host`, `dev=hostonly`, `screen=home|hub|settings`, `sec=<n>` (the section, 0 Games .. 4 Settings), `right=<n>`,
`note`, `launch` (start the game in focus at once), `mode=1080i|ntsc|pal|sd`, `rgb16`,
`shot=<frames>` (the picture to
`host:mxshell.tga`, then the end marker), `end=<frames>` (the end marker after that many
frames), `stats` (libgl's counters on the screen and once a second in the log), `walk` (the
focus a tile on every 20 frames, there and back: Home's busiest frames), `switch=<mode>,...`
and `every=<frames>` (the modes one after the other), `refl=a|b|c` (three other looks of the
reflections, to compare), `room=vu` (the room's lights on VU1, `src/room.vsm`). The standalone
build reads them from `host:mxshell.txt`.

Measured on a PS2 in 1080i (`stats walk`): Home, Hub and Settings keep 60 fps (50 in PAL),
Home also while the focus moves, with about half a millisecond to spare then. The EE's work
is the room (the grid's points worked out once a row, against the lights that reach it, and
drawn as strips in one `mxgl_MultiDraw`; on VU1, `room=vu`, the GS passes took 7 ms instead of
2, as beam racing plays the stream three times a field), the tiles (their vertices as records, one
`mxgl_MultiDraw` a part) and the 2D layer.

To try the MXOS build on PCSX2, under the kernel built for it (`make -C apps/mxos emu`), with
the program and the games in `build/pcsx2-test`: `MXLOAD="host:mxshellk.elf dev=hostonly"` and
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

The GS does not smooth a triangle's edges. Every rounded shape is drawn one way, as a surface
(`src/surface.c`, used by the shell, Files and Media): cards, notifications, dialogs,
highlighted rows, pills, buttons, rings and bars. Its rim goes from 1.5 pixels inside its
outline to nothing a pixel outside, blended, so its corners look right over anything (the room,
a tile, another card); only the rim is translucent on an opaque surface. A lit surface takes the
lamp's colour along its top edge, round the corners and fading out there, as the game tiles
do (`tile.c`, the same `lamp_edge`). Fading and sliding in keep the corners round.

Covers: `tools/mk-covers.py --boxart <dir> <out>` makes `cover.t8` (128 x 176, mxshell's)
and `coverhd.t8` (256 x 361, a DVD case) from box art you have (`<id>.png` or `.jpg`);
`game.ini` names them with `cover=` and `cover_hd=`. `usbcopy` puts them on the stick:
`usbcopy @host:<list>`, a line `<from> <to>` for each file.

The fonts are Inter Tight and JetBrains Mono (SIL Open Font License, in `font/src` with their
licences), made into the atlases by `tools/mk-font.py`.
