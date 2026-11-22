#!/bin/bash
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# test_hatari.sh NAME TESTFILE [SECONDS]
#
# Runs the program headless in Hatari as the target TT (hatari_tt.sh),
# fast-forwarded, with the converted data and TESTFILE as OATEST.TXT, until
# the script ends or SECONDS of host time pass. The run's folder is
# tmp/runs/NAME: its OALOG.TXT is the program's log, and every
# SHOT*.RAW the script took is also written as PNG.
#
# Environment: as hatari_tt.sh, and DATA (default: dist),
# PRG (the program, default: dist/ANNIHILA.TOS), FAST (default on).
set -u
NAME=$1; TEST=$2; SECS=${3:-300}
. "$(dirname "$0")/hatari_tt.sh"
DATA=${DATA:-$GAME}
R=$REPO/tmp/runs/$NAME
rm -rf "$R"; mkdir -p "$R/hd"
# the data is linked, not copied; only the files the program reads, so that
# nothing written in the run's folder reaches DATA through a link
for f in "$DATA"/*.DAT "$DATA"/*.IMG "$DATA"/*.SND "$DATA"/*.SPR "$DATA"/[A-Z][A-Z][0-9][0-9].TXT; do
  ln "$f" "$R/hd/" 2>/dev/null || cp "$f" "$R/hd/"
done
cp "${PRG:-$GAME/ANNIHILA.TOS}" "$R/hd/ANNIHILA.TOS"
cp "$TEST" "$R/hd/OATEST.TXT"
: > "$R/hatari.cfg"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$HATARI" --configfile "$R/hatari.cfg" \
  "${HATARI_FLAGS[@]}" --fast-forward ${FAST:-on} --sound 44100 --statusbar off --alert-level fatal --conout 2 --cmd-fifo "$R/fifo" \
  --harddrive "$R/hd" --auto 'C:\ANNIHILA.TOS' --screenshot-dir "$R" > "$R/log" 2>&1 &
HP=$!
if [ -n "${PROF:-}" ]; then
  # profile from when the mission is loaded until the script ends
  for i in $(seq 1 200); do grep -aq "loaded" "$R/hd/OALOG.TXT" 2>/dev/null && break; sleep 0.5; done
  echo "hatari-debug symbols prg" > "$R/fifo"
  echo "hatari-debug profile on" > "$R/fifo"
fi
for i in $(seq 1 $((SECS * 2))); do
  kill -0 $HP 2>/dev/null || break
  grep -aq "game_run returned\|script ran out" "$R/hd/OALOG.TXT" 2>/dev/null && { sleep 1; break; }
  sleep 0.5
done
if [ -n "${PROF:-}" ]; then
  echo "hatari-debug profile save $R/profile.txt" > "$R/fifo"; sleep 2
  echo "hatari-debug profile addresses" > "$R/fifo"; sleep 1
fi
echo "hatari-debug quit" > "$R/fifo" 2>/dev/null &
# Hatari quits at once; stop it if it has not after a second
for i in $(seq 1 10); do kill -0 $HP 2>/dev/null || break; sleep 0.1; done
kill -9 $HP 2>/dev/null
python3 "$HERE/tools/raw2png.py" "$R/hd" "$R"
cat "$R/hd/OALOG.TXT" 2>/dev/null | tr -d '\r'
