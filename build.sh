#!/bin/bash
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# build.sh [INSTALLER]
#
# Builds AnnihilaTTion into local/ANNIHILA, the folder to copy to the TT:
# ANNIHILA.TOS and the converted game data.
#
#   1. Unpacks the Total Annihilation demo (1997) installer, "Total
#      Annihilation.exe", into local/ta-demo/data, if that is not done yet:
#      the installer is INSTALLER, or local/ta-demo/Total Annihilation.exe.
#      Its size and SHA-256 are checked first; nothing in it runs.
#   2. Converts the demo's data for the TT (tools/convert.py), if the
#      converter or the data is newer than the output.
#   3. Compiles ANNIHILA.TOS with the gcc 13 mintelf cross compiler through stcmd
#      (atarist-toolkit-docker); set CROSS=direct to call
#      m68k-atari-mintelf-gcc from the PATH instead.
#
# Stops at the first step that fails. Needs python3 with numpy and Pillow.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$HERE
DEMO=$REPO/local/ta-demo
OUT=$REPO/local/ANNIHILA
INSTALLER=${1:-$DEMO/Total Annihilation.exe}
SHA=5e41cf05226c274b4ac9e4398f74f6b321506bd7a4317ee1744ff7aceba34c49

if [ ! -d "$DEMO/data/maps" ]; then
  if [ ! -f "$INSTALLER" ]; then
    echo "No demo installer at $INSTALLER." >&2
    echo "Download Total Annihilation.exe as README.md's 'Playing the demo' says, and pass its path." >&2
    exit 1
  fi
  [ "$(wc -c < "$INSTALLER" | tr -d ' ')" = 21540864 ] || { echo "$INSTALLER is not the demo installer (size)." >&2; exit 1; }
  [ "$(shasum -a 256 "$INSTALLER" | cut -d' ' -f1)" = $SHA ] || { echo "$INSTALLER is not the demo installer (SHA-256)." >&2; exit 1; }
  mkdir -p "$DEMO"
  python3 "$HERE/tools/pe_resource.py" "$INSTALLER" ADD 130 1033 "$DEMO/TADemo.hpi"
  python3 "$HERE/tools/hpi_extract.py" "$DEMO/TADemo.hpi" "$DEMO/data"
fi

if [ ! -f "$OUT/MISS3.DAT" ] || [ -n "$(find "$HERE/tools" -name '*.py' -newer "$OUT/MISS3.DAT")" ]; then
  echo "Converting the demo's data (about a minute)..."
  python3 "$HERE/tools/convert.py" "$DEMO/data" "$OUT"
fi

cd "$HERE"
if [ "${CROSS:-stcmd}" = direct ]; then
  make
else
  STCMD_NO_TTY=1 STCMD_QUIET=1 stcmd make
fi
cp build/ANNIHILA.TOS "$OUT/ANNIHILA.TOS"
echo
echo "Done: copy $OUT to the TT (for example to C:\\ANNIHILA) and run ANNIHILA.TOS."
du -sh "$OUT"
