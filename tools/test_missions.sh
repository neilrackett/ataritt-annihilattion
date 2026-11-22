#!/bin/bash
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# test_missions.sh SCRIPT [MISSION...]
#
# Runs a test script on each of the full game's missions and skirmish maps
# in turn (all of them, or those named, such as ac05 cc12 sk01), headless in Hatari as
# test_hatari.sh does, with the game in dist/full: the script's word
# MISSION becomes the mission's name. Each run is tmp/runs/NAME_MISSION,
# NAME the script's; a line a mission says how it ended.
#
# Environment: as test_hatari.sh; DATA defaults to dist/full, and SECS
# (the seconds a run may take) to 300.
set -u
SCRIPT=$1; shift
HERE=$(cd "$(dirname "$0")/.." && pwd)
DATA=${DATA:-$HERE/dist/full}
NAME=$(basename "$SCRIPT" .txt)
MISSIONS=("$@")
if [ ${#MISSIONS[@]} -eq 0 ]; then
  MISSIONS=($(cd "$DATA/DATA" && ls [AC]C[0-9][0-9].DAT SK[0-9][0-9].DAT 2>/dev/null | sed 's/.DAT//' | tr 'A-Z' 'a-z'))
fi
TMP=$(mktemp -d)
for m in "${MISSIONS[@]}"; do
  sed "s/MISSION/$m/g" "$SCRIPT" > "$TMP/$m.txt"
  out=$(DATA=$DATA "$HERE/tools/test_hatari.sh" "${NAME}_$m" "$TMP/$m.txt" "${SECS:-300}" 2>&1)
  res=$(echo "$out" | grep -E "^result -?[0-9]" | tail -1)
  fail=$(echo "$out" | grep -E "failed|script ran out|can't" | head -1)
  mem=$(echo "$out" | grep -E "^sprites:" | tail -1)
  echo "$m: ${res:-no result} ${fail} ${mem}"
done
rm -rf "$TMP"
