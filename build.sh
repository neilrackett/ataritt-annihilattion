#!/bin/bash
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# build.sh [INSTALLER]
# build.sh --full [DISC1 DISC2]
#
# Builds AnnihilaTTion into dist, the folder to copy to the TT: ANNIHILA.TOS
# and, in its DATA folder, the converted game data. Without --full, it plays
# the demo:
#
#   1. Unpacks the Total Annihilation demo (1997) installer, "Total
#      Annihilation.exe", into tmp/ta-demo/data, if that is not done yet:
#      the installer is INSTALLER, or tmp/ta-demo/Total Annihilation.exe.
#      Its size and SHA-256 are checked first; nothing in it runs.
#   2. Converts the demo's data for the TT (tools/convert.py), if the
#      converter or the data is newer than the output.
#   3. Compiles ANNIHILA.TOS with the gcc 13 mintelf cross compiler through stcmd
#      (atarist-toolkit-docker); set CROSS=direct to call
#      m68k-atari-mintelf-gcc from the PATH instead.
#
# With --full, it plays the full game from its two CDs, into dist/full,
# and its expansions from theirs when they are there:
#
#   1. Unpacks the CDs' images into tmp/ta-full (tools/cd_extract.py), if
#      that is not done yet: DISC1 and DISC2, or the CUE sheets (else the
#      images) in tmp/disks' DISK1 and DISK2 folders. Then the expansions'
#      discs, The Core Contingency's and Battle Tactics', from folders of
#      tmp/disks whose names say "Contingency" and "Tactics", into
#      tmp/ta-full/cc and bt. OA_EXPANSIONS=cc, bt or none converts only
#      those.
#   2. Converts the full game's data (about half an hour, and as much
#      again with the expansions; ffmpeg, when there is one, for the
#      films), if the converter or what was unpacked is newer.
#   3. Compiles ANNIHILA.TOS, and copies it into dist/full, beside its DATA.
#
# Stops at the first step that fails. Needs python3 with numpy and Pillow.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$HERE
FULL=
if [ "${1:-}" = --full ]; then
  FULL=1
  shift
fi

# the first CUE sheet, or else image, in a folder of tmp/disks named like $1
find_disc() {
  local f
  for ext in cue CUE img IMG bin BIN iso ISO; do
    f=$(find "$REPO/tmp/disks" -path "*$1*" -name "*.$ext" 2>/dev/null | sort | head -1)
    [ -n "$f" ] && { echo "$f"; return; }
  done
}

if [ -n "$FULL" ]; then
  SRC=$REPO/tmp/ta-full
  OUT=$REPO/dist/full
  PREVIEWS=$REPO/build/previews-full
  WHAT="the full game's data (about half an hour)"
  if [ ! -d "$SRC/data/maps" ]; then
    DISC1=${1:-$(find_disc DISK1)}
    DISC2=${2:-$(find_disc DISK2)}
    if [ -z "$DISC1" ] || [ -z "$DISC2" ]; then
      echo "No images of the Total Annihilation CDs: pass their paths, or put them in tmp/disks (docs/building.md)." >&2
      exit 1
    fi
    echo "Unpacking the CDs (a few minutes)..."
    python3 "$HERE/tools/cd_extract.py" "$DISC1" "$DISC2" "$SRC"
  fi
  for x in cc:Contingency bt:Tactics; do
    disc=$(find_disc "${x#*:}")
    if [ -n "$disc" ] && [ ! -d "$SRC/${x%%:*}/maps" ]; then
      echo "Unpacking $(basename "$disc")..."
      python3 "$HERE/tools/cd_extract.py" "$disc" "$SRC"
    fi
  done
else
  DEMO=$REPO/tmp/ta-demo
  SRC=$DEMO
  OUT=$REPO/dist
  PREVIEWS=$REPO/build/previews
  WHAT="the demo's data (about a minute)"
  INSTALLER=${1:-$DEMO/Total Annihilation.exe}
  SHA=5e41cf05226c274b4ac9e4398f74f6b321506bd7a4317ee1744ff7aceba34c49

  if [ ! -d "$DEMO/data/maps" ]; then
    if [ ! -f "$INSTALLER" ]; then
      echo "No demo installer at $INSTALLER." >&2
      echo "Download Total Annihilation.exe as docs/building.md says, and pass its path." >&2
      exit 1
    fi
    [ "$(wc -c < "$INSTALLER" | tr -d ' ')" = 21540864 ] || { echo "$INSTALLER is not the demo installer (size)." >&2; exit 1; }
    [ "$(shasum -a 256 "$INSTALLER" | cut -d' ' -f1)" = $SHA ] || { echo "$INSTALLER is not the demo installer (SHA-256)." >&2; exit 1; }
    mkdir -p "$DEMO"
    python3 "$HERE/tools/pe_resource.py" "$INSTALLER" ADD 130 1033 "$DEMO/TADemo.hpi"
    python3 "$HERE/tools/hpi_extract.py" "$DEMO/TADemo.hpi" "$DEMO/data"
  fi
fi

if [ ! -f "$OUT/DATA/GAME.DAT" ] || [ -n "$(find "$HERE/tools" -name '*.py' -newer "$OUT/DATA/GAME.DAT")" ] ||
   [ -n "$(find "$SRC" -maxdepth 1 -newer "$OUT/DATA/GAME.DAT" 2>/dev/null)" ]; then
  echo "Converting $WHAT..."
  mkdir -p "$OUT/DATA"
  python3 "$HERE/tools/convert.py" "$SRC/data" "$OUT/DATA" "$PREVIEWS"
fi

cd "$HERE"
if [ "${CROSS:-stcmd}" = direct ]; then
  make
else
  STCMD_NO_TTY=1 STCMD_QUIET=1 stcmd make
fi
if [ -n "$FULL" ]; then
  cp "$REPO/dist/ANNIHILA.TOS" "$OUT/ANNIHILA.TOS"
fi
echo
echo "Done: copy ANNIHILA.TOS and the DATA folder in $OUT to the TT (for example to C:\\ANNIHILA) and run ANNIHILA.TOS."
du -sh "$OUT/DATA"
