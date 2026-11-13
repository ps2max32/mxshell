# MXSHELL2: implementation plan

A new shell for mx OS, written from scratch: not built on `apps/mxshell`'s code. It does on the
PS2 what the home screens of later consoles do (games, a menu over a running game, settings,
controllers, notifications), in one design language of its own, and shows what maxsdk's LLVM
toolchain (HLSL vertex programs on VU1) and libgl (3D) can do. Started 2026-11-13.

The design was settled on mockups first (not in this repository). This plan says what is decided, what the OS still lacks, and in which order it is built.

## Decided (2026-11-13)

| | |
|---|---|
| Design language | **Penumbra**: focus is light. One warm light follows the selection; the rest stays in half-shadow. Ink, graphite and warm white, and one accent (lamp, `#E9C98F`) that is only ever the light. Tiles are real geometry: thickness, a lit edge, a floor reflection. Motion: critically damped springs (focus 180 ms, layers 320 ms, launch and resume 400-560 ms), no bounce; a locked 60 fps (50 PAL), or the effect is cut. Monochrome pad glyphs. Inter Tight and JetBrains Mono (the mockups' fonts), an atlas per video mode drawn 1:1 |
| Home | **A, Shelf**: a row of tiles on a floor; the lit tile has a caption under it (title, last played, Resume / Options); sections Play, Library, Media, System |
| Game hub | the game as a 3D slab, its saves, its settings (video Auto, aspect, controls), its size |
| Menu over a running game | **Pedestal**: the game's last picture becomes the first, lit tile on the shelf; the rest of the row is the menu: Controllers, Notifications, Capture, Settings, Home. Square closes the game, the home button goes back to it. A tile opens a page on the same floor (Controllers: four slots P1-P4) |
| Settings | Video (the shell's mode is the system's and the most a game gets; games ask for Auto), Controllers, Network, Storage, Date and time, Notifications, System |
| Notifications | **Toast**: cards top right in the shell, the newest lit, older ones smaller and dimmer, about 4 s; urgent ones (battery empty) stay until a button. During a game: not shown for now (the game owns the GS); they wait and are shown when the shell comes in front, and in the Notifications tile. The same card drawn over the game is a later step |
| Sound | not adjustable: always 100 % on a PS2 |
| Power-on, transitions | a point of light, a horizon, MXOS, the shelf (3.4 s); launch: the tile opens into the game's first frame; home: the game's picture shrinks to the first tile; resume: the reverse |
| Video | 1080i by default (16:9, 16-bit colour, beam racing); NTSC 640 x 448 and PAL 640 x 512 (4:3, pixels not square; 32-bit by beam racing). Layout in square units (1280 or 960 x 720). Dithered 16-bit colour darkens translucent layers: the background and the tiles are worked out on the EE and drawn opaque. No depth test. Over a game: the game's kept picture + the shell's buffers |

## What the shell needs from the OS

What exists today, and what is missing, found in the code on 2026-11-13. Everything in this
section is kernel, maxsdk, mxpad or mxnet work: it is agreed with the mx OS work
(`libs/maxsdk/docs/mx-os-plan.md`) before it is built, and the interfaces below are proposals.
`mx-os-plan.md` step 3 (stable on the PS2) comes first; none of this starts before it, and the
shell's phase A (below) does not wait for any of it.

### O1. One settings store

Today: settings are spread over files (`IPCONFIG.DAT`, `SERVICES.CFG`, `SWAP.CFG`, `SHELL.CFG`,
`BTPADS.DAT`), and the pad's settings (`apps/mxos/ee/pad.c`, `.kept`) live in RAM until a reset.

- `mc0:/MXOS/SETTINGS.DAT`: text, one `key=value` a line, a version line first. Written
  safely: write `SETTINGS.NEW`, then rename over the old; at boot a left-over `.NEW` that
  reads complete wins.
- The kernel reads it at boot and keeps it. `mx_setting_get(key, buf, size)`,
  `mx_setting_set(key, value)` (the shell only; the kernel writes the file a few seconds after
  the last change, not on every step of a slider), and an event `MX_EVENT_SETTING` (which key)
  to every process that registered for events.
- Keys from the start: `video.mode`, `video.aspect`, `pad.swap_ports`, `pad.swap_xo`,
  `pad.idle_off`, `pad.<id>.colour`, `net.mode` (dhcp, manual), `net.ip`, `net.mask`, `net.gw`,
  `net.dns`, `net.name`, `time.zone`, `time.dst`, `notify.<kind>` (on, off).
- `IPCONFIG.DAT` is read once more if there is no `net.*` key yet, then no longer.

### O2. Events for the shell

Today: `proc_events` delivers `MX_EVENT_END` only. A message slot is 256 bytes and there are
4; an event finding them all taken is dropped.

- New kinds: `MX_EVENT_PAD` (slot, came, went, battery level changed, charging started or
  stopped), `MX_EVENT_DEVICE` (a device mounted or removed: `usb0`, `ata0`, `mc1`, ...),
  `MX_EVENT_NET` (link up or down, address got or lost), `MX_EVENT_SETTING` (O1).
- Events are hints: the shell reads the state again (O4, O5, O7) after one, and when it comes
  in front, so a dropped event loses nothing.

### O3. Notifications

- The kernel keeps the last 32 notifications in a ring (`kind`, `time`, `title`, `text`,
  `action`, `urgent`; about 128 bytes each, 4 KiB), so nothing depends on the shell running
  or being in front. The kernel adds its own (pads, devices, network, a program that crashed);
  any program can add one: `mx_notify(kind, title, text)`. The shell reads them
  (`mx_notify_read(after_id, ...)`) and marks them read.
- Not now: drawing a card over a game. That needs GS memory and the GIF while the game owns
  them (`mx-os-plan.md` step 5); it comes after step 5 as its own step.

### O4. Controllers

Today: `mxpad.irx` records (`common/include/mxpad.h`) give kind (DS3, DS4, DualSense), cable,
order of arrival and battery 0-10 (255: not known); a DualShock 2 comes through PADMAN;
`mx_pad_rumble` works on all; `MXPAD_PAIR` pairs over Bluetooth, and DS3, DS4 and DualSense
pair over USB; the light shows the player (`MXPAD_PLAYERS`).

- Charging: a flag in the record (`MXPAD_CHARGING`; the DS4 and DualSense report it, the
  record keeps only the level today).
- A pad's identity: its Bluetooth address (or a short hash of it) in the record's
  `reserved2`, so a pad's colour and name follow it.
- `MXPAD_DISCONNECT` (slot): ends the Bluetooth link; the DS3, DS4 and DualSense turn
  themselves off then. The kernel turns off a pad that sent nothing for `pad.idle_off`
  minutes.
- `MXPAD_LIST` and `MXPAD_FORGET`: the paired pads in `BTPADS.DAT`, and one taken out.
- Pairing progress (searching, found, paired, failed) for the screen.
- The light: the kernel applies `pad.<id>.colour` (or the player's colour) and a brightness.
- One call for the shell, `mx_pad_info(port, struct mx_pad_info *)`: kind, link (port, USB,
  Bluetooth), battery, charging, id, colour, for SIO2 pads too.
- Player assignment beyond swapping ports 1 and 2 ("make this pad P1"): the kernel's port
  order, from a setting.

### O5. Network

Today: the address comes from `IPCONFIG.DAT` at boot (else 192.168.1.10). mxnet is built with
DHCP and can read and set its config on the IOP; mxeth knows the link and its speed but only
prints them.

- `mx_net_status(struct mx_net_status *)`: link, speed, duplex, address, mask, gateway, DNS,
  DHCP lease left, MAC.
- `mx_net_config(const struct mx_net_config *)`: DHCP or manual, at run time; open
  connections of a running program are kept where the address does not change.
- Link up and down as `MX_EVENT_NET` (O2).
- The host link (mxlink) follows the address.

### O6. Time

Today: no clock call. The PS2 has a battery-backed clock in its CD/DVD controller, read on the
IOP through CDVDMAN.

- The kernel reads it at boot and keeps time with its timer: `mx_time()` (seconds, UTC) and
  `mx_localtime()` from `time.zone` and `time.dst`.
- Later: SNTP over the network (lwIP has a client), which also sets the CD/DVD clock.

### O7. Storage

Today: `mc0` (mxio), `ata0` (mxata), `usb0` (usbmass_bd).

- `mx_dev_list()`: name, label, size, free space; an `MX_EVENT_DEVICE` (O2) when one comes or
  goes; removing a USB drive safely (files closed, no write under way).

### O8. Graphics (maxsdk, libgl)

- `mx-os-plan.md` step 5 (the owner of the graphics path, the hand-over, home is the shell,
  the kept picture as a texture) is what the game menu stands on. 5a gives the system video
  mode and Auto.
- Normals for the focus light: libgl's vertex programs take `POSITION`, `TEXCOORD` and
  `COLOR` only. Either the shell's meshes carry the normal in `COLOR` (8 bits a component is
  enough for flat tiles), or libgl gets a `NORMAL` input. The light itself is written first as
  hand-written VSM and measured; HLSL comes after it performs (as with every new VU1 idea).
- Capture: read the kept picture's pages back (a GS to EE transfer) and write a PNG (zlib is
  in `libs/`). `mxgl_screenshot` exists for the shell's own frames.

## The shell, step by step

Decided 2026-11-13 (user): the OS parts come as late as possible. First everything the shell
can do with the OS as it is (phase A); what needs O1-O8 or step 5 comes after it (phase B).
Each step runs on the PS2 with a television (NTSC and PAL) before it counts as done.

### Phase A: with the OS as it is

| Step | What | With what exists today |
|---|---|---|
| A0 | The app: `apps/mxshell2`, libmxk (`MX_TARGET=mxos`) and libgl; Penumbra as one header (colours, sizes, spring constants); the font atlas; a frame-time counter; a standalone build (`MX_TARGET=standalone`) for pictures on PCSX2 | – |
| A1 | Home Shelf: games from the devices (mxshell's `game.ini` and `cover.t8`, so `package-games.sh` keeps working), the focus light (on the EE first; VU1 later), the reflection, the springs, the caption, sections. Start a game (`mx_loader_launch`: the shell waits), back at its end; its textures given back while the game runs | `mx_loader_launch`, mxio, the storage drivers on `mc0:/MXOS` |
| A2 | Game hub: the slab, details from `game.ini`, the size of its directory, saves where `game.ini` names them (a `saves` key) | mxio |
| A3 | Settings, the parts that need no OS change: Video (the shell's own mode: NTSC, PAL, 480p as libgl offers them), Network (edits `IPCONFIG.DAT`: used at the next start of the kernel, which the page says), Storage (the devices the shell can open, their games), Controllers (which ports have a pad, rumble test), System (the shell's and the kernel's versions, `SERVICES.CFG`). Kept in the shell's own file, `mc0:/MXOS/MXSHELL2.CFG`, until O1 | mxio, `mx_pad_*`, `mx_pad_rumble` |
| A4 | Notifications as toasts, from what the shell sees itself: a pad came or went (`mx_pad_connected`), a device came or went (looked at every few seconds), a game ended with an error or a crash (`mx_crash_last`), a setting saved. Urgent ones stay; a history page | `mx_pad_*`, mxio, `mx_crash_last` |
| A5 | Power-on sequence, the launch move (the lit tile grows to the screen, then the game starts) and the return (the shelf comes back with the game's tile lit) | – |
| A6 | Measure and finish: GS pages and frame time against the budget, 480i flicker, PAL, 16:9; the focus light as hand-written VSM on VU1, then HLSL. Then MXSHELL2 can replace mxshell as `mc0:/MXOS/MXSHELL.ELF` (asked first) | – |

Status 2026-11-13 (evening): A6 measured on the PS2 (1080i, `stats walk`): Home, Hub and
Settings at 60 fps (50 PAL) in every mode, Home also while the focus moves (half a millisecond
to spare then); GS memory: 1.3 of 1.9 MB of texture room with the covers. The room's light as
hand-written VSM on VU1 (`src/room.vsm`, `room=vu`) works (the same picture to a 16-bit
step) but costs more than it saves: beam racing plays the frame's stream once a strip, three
times a field, so VU1's work counts three times and the GS passes took 7 ms instead of 2
(passes came late). The EE keeps the light; VU1 waits for a pipelined program on fewer
points. Open in A6: 480i flicker, 16:9 at 640 x 448, a smaller build.

Status 2026-11-13 (later): 1080i the default, NTSC and PAL with their own pixel shapes, the
mockups' fonts and layout; launch and return checked again under the kernel in 1080i.

Status 2026-11-13: A0-A5 are built and run on PCSX2 (standalone, and under the kernel with
`PCSX2_EE=interp`: a game started, its end status shown, the shell back in 32-bit colour);
not yet on the PS2. Open in phase A: a device that comes or goes while the shell is on (now
only when it looks), mxshell's MX4SIO, A6.

### Phase B: with the OS parts

| Step | What | Needs |
|---|---|---|
| B1 | Settings store: `MXSHELL2.CFG` moves into the kernel's | O1 |
| B2 | Controllers in full: kind, battery, charging, light colour, turn off, forget, pair | O4 |
| B3 | Network status and settings at run time; Date and time; Storage events | O5, O6, O7 |
| B4 | Notifications from the kernel's ring (pads, devices, network, other programs) | O2, O3 |
| B5 | Game menu, Pedestal: the kept picture as the first tile, the menu tiles, the Controllers page, Capture, Close; notifications that came during the game appear when it opens | step 5 (5c), O8 |
| B6 | The home and resume moves as one camera move | step 5 (5d) |
| later | The notification card drawn over a running game | step 5, O3 |

## Open points

- Where a game keeps its saves and how the hub finds them; play time per game needs O6 and a
  record the shell writes (in O1, or a file per game).
- The font: an open font with Latin accents (Inter Tight: SIL OFL), and the size of its atlas.
- 16:9: anamorphic at 640 x 448, set by `video.aspect`; what a 4:3 game does.
- Whether mxshell's devices that need drivers loaded on demand (MX4SIO, `host:`) stay as they
  are in MXSHELL2.
