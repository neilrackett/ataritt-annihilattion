#!/bin/bash
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# play_hatari.sh
#
# Plays the build (build.sh) in a Hatari window as the target TT
# (hatari_tt.sh; MACHINE=falcon for the Falcon experiment), with
# dist as drive C: and ANNIHILA.TOS started.
# Hatari's TT runs compiled code about twice as fast as a real one, so a real
# TT plays slower than this.
#
# Environment: as hatari_tt.sh.
set -eu
. "$(dirname "$0")/hatari_tt.sh"
[ -f "$GAME/ANNIHILA.TOS" ] || { echo "No build: run build.sh first." >&2; exit 1; }
rm -f "$GAME/OATEST.TXT"
CFG=$REPO/tmp/hatari-play.cfg
: > "$CFG"
exec "$HATARI" --configfile "$CFG" "${HATARI_FLAGS[@]}" --harddrive "$GAME" --auto 'C:\ANNIHILA.TOS'
