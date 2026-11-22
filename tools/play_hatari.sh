#!/bin/bash
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# play_hatari.sh
#
# Plays the build (build.sh) in a Hatari window as the target TT
# (hatari_tt.sh; MACHINE=falcon for the Falcon experiment), with
# dist as drive C: and ANNIHILA.TOS started.
#
# Environment: as hatari_tt.sh, and DATA, another folder of data to play
# instead, such as dist/full (the full game): its ANNIHILA.TOS is the
# newest build's.
set -eu
. "$(dirname "$0")/hatari_tt.sh"
[ -f "$GAME/ANNIHILA.TOS" ] || { echo "No build: run build.sh first." >&2; exit 1; }
DATA=${DATA:-$GAME}
[ "$DATA" -ef "$GAME" ] || cp "$GAME/ANNIHILA.TOS" "$DATA/ANNIHILA.TOS"
rm -f "$DATA/OATEST.TXT"
CFG=$REPO/tmp/hatari-play.cfg
: > "$CFG"
exec "$HATARI" --configfile "$CFG" "${HATARI_FLAGS[@]}" --harddrive "$DATA" --auto 'C:\ANNIHILA.TOS'
