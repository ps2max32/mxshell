#!/bin/bash
# Put the ps2max games on a USB stick or SD card, for mxshell:
#
#   package-games.sh <mount point of the card>
#
# Each game gets /PS2MAX/<game>/: its ELF, game.ini, and its data as the game
# finds it under host:. A game whose own game.ini (its ps2/game.ini, the one
# of its package: elf=, args=, data=) names a data directory elsewhere,
# data=../DATA/quake, gets its data there: /PS2MAX/DATA/quake, which the
# games of Quake share. Writes below /PS2MAX only, and removes nothing. Bring
# your own game data: DATA is the directory holding it (default
# build/ps2-hw-test, what the games use as host: in tests). Running it again
# copies what changed.
set -eu
PS2MAX=$(cd "$(dirname "$0")/../.." && pwd)
APPS=$PS2MAX/build/apps
DATA=${DATA:-$PS2MAX/build/ps2-hw-test}
[ $# = 1 ] && [ -d "$1" ] || { echo "usage: $0 <mount point>" >&2; exit 2; }
ROOT=$1/PS2MAX
SWAP_MIB=${SWAP_MIB:-64}

# id|title|year|type|elf under build/apps|data directories|description
GAMES='
quake1|Quake|1996|Shooter|quake1/quake1.elf|id1|First person shooter, on GLQuake.
quakespasm|QuakeSpasm|1996|Shooter|quakespasm/quakespasm.elf|id1|Quake on the QuakeSpasm engine.
quake2|Quake II|1997|Shooter|quake2/quake2.elf|baseq2|First person shooter with a connected campaign.
quake3|Quake III Arena|1999|Shooter|quake3/quake3.elf|baseq3|Arena shooter, on ioquake3.
hexen2|Hexen II|1997|Action RPG|hexen2/hexen2.elf|data1|Action role-playing game with four character classes.
halflife|Half-Life|1998|Shooter|halflife/halflife.elf|hl|Story-driven shooter, on Xash3D.
descent3|Descent 3|1999|Shooter|descent3-demo-vm/descent3.elf|d3|Six degrees of freedom through tunnels.
duke3d|Duke Nukem 3D|1996|Shooter|duke3d/duke3d.elf|duke3d|Sprite-based shooter, on JFDuke3D.
doom|Doom II|1994|Shooter|doom/doom.elf|doom|Classic shooter, on PrBoom+.
'

copy() { # FAT and exFAT: no owners, links or exact times
  rsync -rL --times --modify-window=2 --no-perms --no-owner --no-group \
    --exclude='*.swp' --exclude='*.log' --exclude='*.tga' --exclude='*.tmp' "$@"
}

mkdir -p "$ROOT"
echo "$GAMES" | while IFS='|' read -r id title year type elf dirs desc; do
  [ -n "$id" ] || continue
  [ -f "$APPS/$elf" ] || { echo "$id: no $elf, skipped" >&2; continue; }
  [ -e "$DATA/$dirs" ] || { echo "$id: no data in $DATA/$dirs, skipped" >&2; continue; }
  echo "$id"
  mkdir -p "$ROOT/$id"
  copy "$APPS/$elf" "$ROOT/$id/$id.elf"
  # the game's own keys (its package's game.ini)
  own=
  for f in "$PS2MAX/apps/$id/ps2/game.ini" "$PS2MAX/apps/$id/code/ps2/game.ini"; do
    [ -f "$f" ] && own=$f && break
  done
  data=$(sed -n 's/^data=//p' "${own:-/dev/null}")
  case "$data" in
    ../*) mkdir -p "$ROOT/${data#../}"; copy "$DATA/$dirs" "$ROOT/${data#../}/" ;;
    "") copy "$DATA/$dirs" "$ROOT/$id/" ;;
    *) mkdir -p "$ROOT/$id/$data"; copy "$DATA/$dirs" "$ROOT/$id/$data/" ;;
  esac
  # a cover: COVERS/<game>.png or .jpg, made into cover.t8 (your own images)
  cover=
  for c in "${COVERS:-}/$id".png "${COVERS:-}/$id".jpg; do
    [ -f "$c" ] && "$(dirname "$0")/tools/mk-cover.py" "$c" "$ROOT/$id/cover.t8" && cover=cover.t8
  done
  {
    printf '[game]\ntitle=%s\nyear=%s\ntype=%s\nelf=%s.elf\ncover=%s\ndescription=%s\n' \
      "$title" "$year" "$type" "$id" "$cover" "$desc"
    [ -n "$own" ] && grep -E '^(args|data|saves)=' "$own" || true
  } > "$ROOT/$id/game.ini"
done

# the system's swap file, for all games; made ahead, so paging on the card
# changes no file system data
if [ "$(stat -c %s "$ROOT/swap.bin" 2>/dev/null || echo 0)" -lt $((SWAP_MIB << 20)) ]; then
  dd if=/dev/zero of="$ROOT/swap.bin" bs=1M count=$SWAP_MIB conv=fsync status=none
fi

# for storage tests: 16 MiB, each 32-bit word its own byte offset
mkdir -p "$ROOT/test"
python3 -c 'import struct,sys;sys.stdout.buffer.write(b"".join(struct.pack("<I",i*4) for i in range(1<<22)))' \
  > "$ROOT/test/pattern.bin"
